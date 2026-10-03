#define NOMINMAX
#include "pluginscaler/formats/vst2/VST2LegacyABI.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <cstddef>
#include <string>
#include <thread>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct ProxyManifest {
    std::string effectName{"125A PluginScaler"};
    std::string vendor{"125A"};
    std::string product{"PluginScaler VST2 Proxy"};
    VstInt32 uniqueId{0x31535041};
    VstInt32 version{100};
    VstInt32 numPrograms{1};
    VstInt32 numParams{0};
    VstInt32 numInputs{2};
    VstInt32 numOutputs{2};
    VstInt32 flags{1 << 4};
    std::vector<float> parameterDefaults;
    bool valid{false};
};

struct ProxyInstance {
    AEffect effect{};
    AudioMasterCallback host{nullptr};
    ProxyManifest manifest{};

    pluginscaler::ipc::AudioSharedChannel channel;
    PROCESS_INFORMATION helperProcess{};
    HANDLE controlPipe{INVALID_HANDLE_VALUE};
    std::vector<std::uint8_t> stateChunk;
    VstRect editorRect{};
    bool editorOpen{false};

    double sampleRate{48000.0};
    VstInt32 blockSize{512};
    bool mainsOn{false};
    bool bridgeStarted{false};
    std::uint64_t sequence{0};
    std::array<pluginscaler::ipc::MidiSharedEvent, pluginscaler::ipc::kMaxMidiEvents> pendingMidi{};
    std::uint32_t pendingMidiCount{0};
    std::vector<float> parameterValues;
    std::uint32_t parameterGeneration{1};
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

ProxyManifest loadManifest() {
    ProxyManifest m;
    const auto path = getenvWide(L"PLUGINSCALER_TARGET_MANIFEST");
    if (path.empty()) return m;

    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return m;

    std::string line;
    bool formatOk = false;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const auto key = line.substr(0, pos);
        const auto value = line.substr(pos + 1);
        try {
            if (key == "format") formatOk = (value == "125A-PluginScaler-VST2-Manifest-1");
            else if (key == "effect") m.effectName = value;
            else if (key == "vendor") m.vendor = value;
            else if (key == "product") m.product = value;
            else if (key == "uniqueId") m.uniqueId = static_cast<VstInt32>(std::stol(value));
            else if (key == "version") m.version = static_cast<VstInt32>(std::stol(value));
            else if (key == "programs") m.numPrograms = static_cast<VstInt32>(std::stol(value));
            else if (key == "params") m.numParams = static_cast<VstInt32>(std::stol(value));
            else if (key == "inputs") m.numInputs = static_cast<VstInt32>(std::stol(value));
            else if (key == "outputs") m.numOutputs = static_cast<VstInt32>(std::stol(value));
            else if (key == "flags") m.flags = static_cast<VstInt32>(std::stol(value));
            else if (key.rfind("param.", 0) == 0) {
                const auto index = static_cast<std::size_t>(std::stoul(key.substr(6)));
                if (m.parameterDefaults.size() <= index)
                    m.parameterDefaults.resize(index + 1, 0.0f);
                m.parameterDefaults[index] = std::stof(value);
            }
        } catch (...) {
            return ProxyManifest{};
        }
    }

    m.valid = formatOk &&
        m.numPrograms >= 0 && m.numParams >= 0 &&
        m.numInputs >= 0 && m.numOutputs >= 0 &&
        m.numParams <= static_cast<VstInt32>(pluginscaler::ipc::kMaxParameters);
    if (m.valid)
        m.parameterDefaults.resize(static_cast<std::size_t>(m.numParams), 0.0f);
    return m;
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

bool readExact(HANDLE pipe, void* data, DWORD bytes) noexcept {
    auto* p = static_cast<std::uint8_t*>(data);
    DWORD done = 0;
    while (done < bytes) {
        DWORD got = 0;
        if (!ReadFile(pipe, p + done, bytes - done, &got, nullptr) || got == 0)
            return false;
        done += got;
    }
    return true;
}

bool writeExact(HANDLE pipe, const void* data, DWORD bytes) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    DWORD done = 0;
    while (done < bytes) {
        DWORD sent = 0;
        if (!WriteFile(pipe, p + done, bytes - done, &sent, nullptr) || sent == 0)
            return false;
        done += sent;
    }
    return true;
}

bool controlCall(ProxyInstance* inst,
                 pluginscaler::ipc::ControlCommand command,
                 std::int32_t arg0,
                 const void* payload,
                 std::uint32_t payloadBytes,
                 std::vector<std::uint8_t>& reply) {
    reply.clear();
    if (!inst || inst->controlPipe == INVALID_HANDLE_VALUE ||
        payloadBytes > pluginscaler::ipc::kMaxControlPayload)
        return false;

    pluginscaler::ipc::ControlMessageHeader req{};
    req.command = command;
    req.arg0 = arg0;
    req.payloadBytes = payloadBytes;

    if (!writeExact(inst->controlPipe, &req, sizeof(req)) ||
        (payloadBytes && !writeExact(inst->controlPipe, payload, payloadBytes)))
        return false;

    pluginscaler::ipc::ControlMessageHeader resp{};
    if (!readExact(inst->controlPipe, &resp, sizeof(resp)) ||
        resp.magic != pluginscaler::ipc::kControlMagic ||
        resp.version != pluginscaler::ipc::kControlVersion ||
        resp.command != command ||
        resp.status != pluginscaler::ipc::ControlStatus::Ok ||
        resp.responseBytes > pluginscaler::ipc::kMaxControlPayload)
        return false;

    reply.resize(resp.responseBytes);
    return resp.responseBytes == 0 ||
           readExact(inst->controlPipe, reply.data(), resp.responseBytes);
}

void refreshParametersFromHelper(ProxyInstance* inst) {
    if (!inst) return;
    std::vector<std::uint8_t> reply;
    if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetParameters,
                     0, nullptr, 0, reply) ||
        reply.size() % sizeof(float) != 0)
        return;

    const auto count = reply.size() / sizeof(float);
    const auto copyCount = (std::min)(count, inst->parameterValues.size());
    const auto* values = reinterpret_cast<const float*>(reply.data());
    for (std::size_t i = 0; i < copyCount; ++i)
        inst->parameterValues[i] = values[i];
    ++inst->parameterGeneration;
    if (inst->parameterGeneration == 0)
        inst->parameterGeneration = 1;
}

void stopBridge(ProxyInstance* inst) noexcept {
    if (!inst) return;

    if (inst->bridgeStarted) {
        if (inst->editorOpen && inst->controlPipe != INVALID_HANDLE_VALUE) {
            std::vector<std::uint8_t> ignored;
            (void)controlCall(inst, pluginscaler::ipc::ControlCommand::CloseEditor,
                              0, nullptr, 0, ignored);
            inst->editorOpen = false;
        }
        if (inst->controlPipe != INVALID_HANDLE_VALUE) {
            std::vector<std::uint8_t> ignored;
            (void)controlCall(inst, pluginscaler::ipc::ControlCommand::Shutdown,
                              0, nullptr, 0, ignored);
            CloseHandle(inst->controlPipe);
            inst->controlPipe = INVALID_HANDLE_VALUE;
        }

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
    const std::wstring controlPipeName = L"\\\\.\\pipe\\125A_PluginScaler_Control_" + suffix;

    if (!inst->channel.create(mapName, inEvent, outEvent))
        return false;

    std::wstring command =
        quote(helper) + L" --serve-vst2-shm " +
        quote(target) + L" " +
        quote(mapName) + L" " +
        quote(inEvent) + L" " +
        quote(outEvent) + L" " +
        quote(controlPipeName);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        inst->channel.close();
        return false;
    }

    HANDLE control = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100 && control == INVALID_HANDLE_VALUE; ++attempt) {
        control = CreateFileW(controlPipeName.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (control != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_PIPE_BUSY &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        WaitNamedPipeW(controlPipeName.c_str(), 20);
        Sleep(10);
    }

    if (control == INVALID_HANDLE_VALUE) {
        TerminateProcess(pi.hProcess, 2);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        inst->channel.close();
        return false;
    }

    inst->controlPipe = control;
    inst->helperProcess = pi;
    inst->bridgeStarted = true;

    if (!inst->stateChunk.empty()) {
        std::vector<std::uint8_t> ignored;
        if (controlCall(inst, pluginscaler::ipc::ControlCommand::SetState,
                        0, inst->stateChunk.data(),
                        static_cast<std::uint32_t>(inst->stateChunk.size()), ignored))
            refreshParametersFromHelper(inst);
    }

    return true;
}

VstIntPtr __cdecl dispatcher(AEffect* effect, VstInt32 opcode, VstInt32 index,
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

    case EffEditGetRect: {
        if (!ptr || !startBridge(inst)) return 0;
        std::vector<std::uint8_t> reply;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetEditorRect,
                         0, nullptr, 0, reply) ||
            reply.size() != sizeof(pluginscaler::ipc::EditorRectPayload))
            return 0;
        pluginscaler::ipc::EditorRectPayload remote{};
        std::memcpy(&remote, reply.data(), sizeof(remote));
        inst->editorRect.left = static_cast<std::int16_t>(remote.left);
        inst->editorRect.top = static_cast<std::int16_t>(remote.top);
        inst->editorRect.right = static_cast<std::int16_t>(remote.right);
        inst->editorRect.bottom = static_cast<std::int16_t>(remote.bottom);
        *static_cast<VstRect**>(ptr) = &inst->editorRect;
        return 1;
    }

    case EffEditOpen: {
        if (!ptr || !startBridge(inst)) return 0;
        pluginscaler::ipc::EditorOpenPayload request{};
        request.parentWindow = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(ptr));
        std::vector<std::uint8_t> ignored;
        const bool ok = controlCall(inst, pluginscaler::ipc::ControlCommand::OpenEditor,
                                    0, &request, sizeof(request), ignored);
        inst->editorOpen = ok;
        return ok ? 1 : 0;
    }

    case EffEditClose: {
        if (!inst->bridgeStarted || inst->controlPipe == INVALID_HANDLE_VALUE)
            return 1;
        std::vector<std::uint8_t> ignored;
        const bool ok = controlCall(inst, pluginscaler::ipc::ControlCommand::CloseEditor,
                                    0, nullptr, 0, ignored);
        inst->editorOpen = false;
        return ok ? 1 : 0;
    }

    case EffGetChunk: {
        if (!ptr || !startBridge(inst)) return 0;
        std::vector<std::uint8_t> reply;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetState,
                         index, nullptr, 0, reply) ||
            reply.empty())
            return 0;
        inst->stateChunk = std::move(reply);
        *static_cast<void**>(ptr) = inst->stateChunk.data();
        return static_cast<VstIntPtr>(inst->stateChunk.size());
    }

    case EffSetChunk: {
        if (!ptr || value <= 0 || !startBridge(inst) ||
            static_cast<std::uint64_t>(value) > pluginscaler::ipc::kMaxControlPayload)
            return 0;
        std::vector<std::uint8_t> ignored;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::SetState,
                         index, ptr, static_cast<std::uint32_t>(value), ignored))
            return 0;
        inst->stateChunk.assign(static_cast<const std::uint8_t*>(ptr),
                                static_cast<const std::uint8_t*>(ptr) +
                                    static_cast<std::size_t>(value));
        refreshParametersFromHelper(inst);
        return 1;
    }

    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.effectName.c_str());
        return 1;

    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.vendor.c_str());
        return 1;

    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.product.c_str());
        return 1;

    case EffGetVendorVersion:
        return inst->manifest.version;

    case EffProcessEvents: {
        if (!ptr) return 0;
        auto* events = static_cast<VstEvents*>(ptr);
        if (events->numEvents < 0) return 0;

        const auto count = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(events->numEvents),
            pluginscaler::ipc::kMaxMidiEvents);
        auto** eventPtrs = reinterpret_cast<VstEvent**>(
            reinterpret_cast<std::uint8_t*>(events) + offsetof(VstEvents, events));

        std::uint32_t written = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ev = eventPtrs[i];
            if (!ev || ev->type != kVstMidiType ||
                ev->byteSize < static_cast<VstInt32>(sizeof(VstMidiEvent)))
                continue;
            auto* midi = reinterpret_cast<VstMidiEvent*>(ev);
            auto& dst = inst->pendingMidi[written++];
            dst.deltaFrames = midi->deltaFrames;
            dst.flags = midi->flags;
            for (int b = 0; b < 4; ++b)
                dst.data[b] = static_cast<std::uint8_t>(midi->midiData[b]);
        }
        inst->pendingMidiCount = written;
        return 1;
    }

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
    block->header.midiEventCount = inst->pendingMidiCount;
    for (std::uint32_t i = 0; i < inst->pendingMidiCount; ++i)
        block->midiEvents[i] = inst->pendingMidi[i];

    block->header.parameterCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(inst->parameterValues.size(),
                              pluginscaler::ipc::kMaxParameters));
    block->header.parameterGeneration = inst->parameterGeneration;
    for (std::uint32_t i = 0; i < block->header.parameterCount; ++i)
        block->parameterValues[i] = inst->parameterValues[i];

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

    inst->pendingMidiCount = 0;

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

void __cdecl setParameter(AEffect* effect, VstInt32 index, float value) {
    auto* inst = self(effect);
    if (!inst || index < 0 ||
        index >= static_cast<VstInt32>(inst->parameterValues.size()))
        return;
    inst->parameterValues[static_cast<std::size_t>(index)] =
        std::clamp(value, 0.0f, 1.0f);
    ++inst->parameterGeneration;
    if (inst->parameterGeneration == 0)
        inst->parameterGeneration = 1;
}

float __cdecl getParameter(AEffect* effect, VstInt32 index) {
    auto* inst = self(effect);
    if (!inst || index < 0 ||
        index >= static_cast<VstInt32>(inst->parameterValues.size()))
        return 0.0f;
    return inst->parameterValues[static_cast<std::size_t>(index)];
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host) return nullptr;

    const VstIntPtr hostVersion = host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f);
    if (hostVersion <= 0) return nullptr;

    auto* inst = new ProxyInstance{};
    inst->host = host;
    inst->manifest = loadManifest();
    inst->parameterValues = inst->manifest.parameterDefaults;

    inst->effect.magic = kEffectMagic;
    inst->effect.dispatcher = dispatcher;
    inst->effect.process = process;
    inst->effect.setParameter = setParameter;
    inst->effect.getParameter = getParameter;
    inst->effect.numPrograms = inst->manifest.numPrograms;
    inst->effect.numParams = inst->manifest.numParams;
    inst->effect.numInputs = inst->manifest.numInputs;
    inst->effect.numOutputs = inst->manifest.numOutputs;
    inst->effect.flags = inst->manifest.flags;
    inst->effect.object = inst;
    inst->effect.uniqueId = inst->manifest.uniqueId;
    inst->effect.version = inst->manifest.version;
    inst->effect.processReplacing = processReplacing;

    return &inst->effect;
}
