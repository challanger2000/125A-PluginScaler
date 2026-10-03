#include "pluginscaler/formats/vst2/VST2LegacyABI.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct ProxyInstance {
    AEffect effect{};
    AudioMasterCallback host{nullptr};

    pluginscaler::ipc::AudioSharedChannel channel;
    PROCESS_INFORMATION helperProcess{};

    double sampleRate{48000.0};
    VstInt32 blockSize{512};
    bool mainsOn{false};
    bool bridgeStarted{false};
    std::uint64_t sequence{0};
};

std::atomic<std::uint64_t> g_instanceCounter{1};

std::wstring getenvWide(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};

    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) return {};
    value.resize(written);
    return value;
}

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

ProxyInstance* self(AEffect* effect) noexcept {
    return effect ? static_cast<ProxyInstance*>(effect->object) : nullptr;
}

void zeroOutputs(AEffect* effect, float** outputs, VstInt32 frames) noexcept {
    if (!effect || !outputs || frames <= 0) return;
    const auto channels = std::max<VstInt32>(0, effect->numOutputs);
    for (VstInt32 ch = 0; ch < channels; ++ch) {
        if (outputs[ch])
            std::fill(outputs[ch], outputs[ch] + frames, 0.0f);
    }
}

void stopBridge(ProxyInstance* inst) noexcept {
    if (!inst) return;

    if (inst->bridgeStarted) {
        if (auto* block = inst->channel.block()) {
            block->header.state.store(
                static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::Shutdown),
                std::memory_order_release);
            inst->channel.signalInput();
        }

        if (inst->helperProcess.hProcess) {
            const DWORD wait = WaitForSingleObject(inst->helperProcess.hProcess, 1500);
            if (wait == WAIT_TIMEOUT)
                TerminateProcess(inst->helperProcess.hProcess, 1);
            CloseHandle(inst->helperProcess.hProcess);
        }
        if (inst->helperProcess.hThread)
            CloseHandle(inst->helperProcess.hThread);

        inst->helperProcess = {};
        inst->channel.close();
        inst->bridgeStarted = false;
    }
}

bool startBridge(ProxyInstance* inst) {
    if (!inst) return false;
    if (inst->bridgeStarted) return true;

    const std::wstring helper = getenvWide(L"PLUGINSCALER_HELPER_X86");
    const std::wstring target = getenvWide(L"PLUGINSCALER_TARGET_VST2");
    if (helper.empty() || target.empty())
        return false;

    const auto id = g_instanceCounter.fetch_add(1, std::memory_order_relaxed);
    const std::wstring suffix =
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(id);

    const std::wstring mapName = L"Local\\125A_PluginScaler_Proxy_Map_" + suffix;
    const std::wstring inEvent = L"Local\\125A_PluginScaler_Proxy_In_" + suffix;
    const std::wstring outEvent = L"Local\\125A_PluginScaler_Proxy_Out_" + suffix;

    if (!inst->channel.create(mapName, inEvent, outEvent))
        return false;

    std::wstring command =
        quote(helper) + L" --serve-vst2-shm " +
        quote(target) + L" " +
        quote(mapName) + L" " +
        quote(inEvent) + L" " +
        quote(outEvent);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        inst->channel.close();
        return false;
    }

    inst->helperProcess = pi;
    inst->bridgeStarted = true;
    return true;
}

VstIntPtr __cdecl dispatcher(AEffect* effect, VstInt32 opcode, VstInt32,
                             VstIntPtr value, void* ptr, float opt) {
    auto* inst = self(effect);
    if (!inst) return 0;

    switch (opcode) {
    case EffOpen:
        return 1;

    case EffClose:
        stopBridge(inst);
        delete inst;
        return 1;

    case EffSetSampleRate:
        inst->sampleRate = opt > 0.0f ? static_cast<double>(opt) : 48000.0;
        return 1;

    case EffSetBlockSize:
        inst->blockSize = value > 0 ? static_cast<VstInt32>(value) : 512;
        return 1;

    case EffMainsChanged:
        inst->mainsOn = value != 0;
        if (inst->mainsOn)
            return startBridge(inst) ? 1 : 0;
        stopBridge(inst);
        return 1;

    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A PluginScaler");
        return 1;

    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A");
        return 1;

    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "PluginScaler VST2 Proxy");
        return 1;

    case EffGetVendorVersion:
        return 100;

    case EffCanDo:
        return 0;

    default:
        return 0;
    }
}

void __cdecl processReplacing(AEffect* effect, float** inputs, float** outputs,
                               VstInt32 frames) {
    auto* inst = self(effect);
    if (!inst || !inst->mainsOn || frames <= 0 ||
        frames > static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioFrames)) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    if (!startBridge(inst)) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    auto* block = inst->channel.block();
    if (!block) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    const std::uint32_t inChannels =
        static_cast<std::uint32_t>(std::clamp<VstInt32>(
            effect->numInputs, 0, static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioChannels)));
    const std::uint32_t outChannels =
        static_cast<std::uint32_t>(std::clamp<VstInt32>(
            effect->numOutputs, 0, static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioChannels)));

    block->header.inputChannels = inChannels;
    block->header.outputChannels = outChannels;
    block->header.frames = static_cast<std::uint32_t>(frames);
    block->header.sampleRateHz =
        static_cast<std::uint32_t>(std::llround(inst->sampleRate));
    block->header.sequence = ++inst->sequence;
    block->header.errorCode = 0;

    for (std::uint32_t ch = 0; ch < inChannels; ++ch) {
        if (inputs && inputs[ch])
            std::copy(inputs[ch], inputs[ch] + frames, block->inputs[ch]);
        else
            std::fill(block->inputs[ch], block->inputs[ch] + frames, 0.0f);
    }

    for (std::uint32_t ch = 0; ch < outChannels; ++ch)
        std::fill(block->outputs[ch], block->outputs[ch] + frames, 0.0f);

    block->header.state.store(
        static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::InputReady),
        std::memory_order_release);

    if (!inst->channel.signalInput() ||
        !inst->channel.waitForOutput(std::chrono::milliseconds(1000))) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    const auto state = static_cast<pluginscaler::ipc::AudioBlockState>(
        block->header.state.load(std::memory_order_acquire));

    if (state != pluginscaler::ipc::AudioBlockState::OutputReady ||
        block->header.errorCode != 0) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    for (std::uint32_t ch = 0; ch < outChannels; ++ch) {
        if (outputs && outputs[ch])
            std::copy(block->outputs[ch], block->outputs[ch] + frames, outputs[ch]);
    }
}

void __cdecl process(AEffect* effect, float** inputs, float** outputs, VstInt32 frames) {
    processReplacing(effect, inputs, outputs, frames);
}

void __cdecl setParameter(AEffect*, VstInt32, float) {}
float __cdecl getParameter(AEffect*, VstInt32) { return 0.0f; }

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host) return nullptr;

    const VstIntPtr hostVersion = host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f);
    if (hostVersion <= 0) return nullptr;

    auto* inst = new ProxyInstance{};
    inst->host = host;

    inst->effect.magic = kEffectMagic;
    inst->effect.dispatcher = dispatcher;
    inst->effect.process = process;
    inst->effect.setParameter = setParameter;
    inst->effect.getParameter = getParameter;
    inst->effect.numPrograms = 1;
    inst->effect.numParams = 0;
    inst->effect.numInputs = 2;
    inst->effect.numOutputs = 2;
    inst->effect.flags = 1 << 4; // effFlagsCanReplacing
    inst->effect.object = inst;
    inst->effect.uniqueId = 0x31535041; // "1SPA"
    inst->effect.version = 100;
    inst->effect.processReplacing = processReplacing;

    return &inst->effect;
}
