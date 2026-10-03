#include "pluginscaler/formats/VST2PluginModule.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <utility>

namespace pluginscaler::formats {
namespace {

using namespace vst2abi;

VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void* ptr, float) {
    switch (opcode) {
    case AudioMasterVersion:
        return 2400;
    case AudioMasterGetSampleRate:
        return 44100;
    case AudioMasterGetBlockSize:
        return 512;
    case AudioMasterGetVendorVersion:
        return 1000;
    case AudioMasterGetVendorString:
        if (ptr) {
            std::strncpy(static_cast<char*>(ptr), "125A", 63);
            static_cast<char*>(ptr)[63] = '\0';
            return 1;
        }
        return 0;
    case AudioMasterGetProductString:
        if (ptr) {
            std::strncpy(static_cast<char*>(ptr), "125A PluginScaler", 63);
            static_cast<char*>(ptr)[63] = '\0';
            return 1;
        }
        return 0;
    case AudioMasterCanDo:
        return 0;
    default:
        return 0;
    }
}

std::string queryString(AEffect* effect, VstInt32 opcode) {
    if (!effect || !effect->dispatcher) return {};
    std::array<char, 256> buffer{};
    const auto ok = effect->dispatcher(effect, opcode, 0, 0, buffer.data(), 0.0f);
    if (!ok) return {};
    buffer.back() = '\0';
    return std::string(buffer.data());
}

std::string win32Error(const char* prefix) {
    return std::string(prefix) + " (Win32=" + std::to_string(GetLastError()) + ")";
}

} // namespace

VST2PluginModule::~VST2PluginModule() {
    close();
}

VST2ProbeResult VST2PluginModule::probe(const std::filesystem::path& path) {
    close();

    VST2ProbeResult result;
    if (path.empty()) {
        result.error = "empty plugin path";
        return result;
    }

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        result.error = win32Error("LoadLibraryExW failed");
        return result;
    }
    module_ = module;

    EntryProc entry = reinterpret_cast<EntryProc>(GetProcAddress(module, "VSTPluginMain"));
    if (entry) {
        result.entryPoint = "VSTPluginMain";
    } else {
        entry = reinterpret_cast<EntryProc>(GetProcAddress(module, "main"));
        if (entry) result.entryPoint = "main";
    }

    if (!entry) {
        result.error = "VST2 entry point not found";
        close();
        return result;
    }

    AEffect* effect = nullptr;
#if defined(_MSC_VER)
    __try {
        effect = entry(hostCallback);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result.error = "exception while creating VST2 instance";
        close();
        return result;
    }
#else
    effect = entry(hostCallback);
#endif

    if (!effect) {
        result.error = "VST2 entry point returned null";
        close();
        return result;
    }
    effect_ = effect;

    if (effect->magic != kEffectMagic) {
        result.error = "invalid AEffect magic";
        close();
        return result;
    }
    if (!effect->dispatcher) {
        result.error = "AEffect dispatcher is null";
        close();
        return result;
    }

    result.loaded = true;
    result.uniqueId = effect->uniqueId;
    result.version = effect->version;
    result.numPrograms = effect->numPrograms;
    result.numParams = effect->numParams;
    result.numInputs = effect->numInputs;
    result.numOutputs = effect->numOutputs;

#if defined(_MSC_VER)
    __try {
        effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f);
        effOpenCalled_ = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result.error = "exception during effOpen";
        close();
        return result;
    }
#else
    effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f);
    effOpenCalled_ = true;
#endif

    result.opened = true;
    result.effectName = queryString(effect, EffGetEffectName);
    result.vendor = queryString(effect, EffGetVendorString);
    result.product = queryString(effect, EffGetProductString);

    close();
    result.closed = true;
    return result;
}

void VST2PluginModule::close() noexcept {
    auto* effect = effect_;
    effect_ = nullptr;

    if (effect && effOpenCalled_ && effect->dispatcher) {
#if defined(_MSC_VER)
        __try {
            effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Probe teardown must not throw into the helper.
        }
#else
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
#endif
    }
    effOpenCalled_ = false;

    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

} // namespace pluginscaler::formats
