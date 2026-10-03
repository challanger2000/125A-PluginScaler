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
            strcpy_s(static_cast<char*>(ptr), 64, "125A");
            return 1;
        }
        return 0;
    case AudioMasterGetProductString:
        if (ptr) {
            strcpy_s(static_cast<char*>(ptr), 64, "125A PluginScaler");
            return 1;
        }
        return 0;
    case AudioMasterCanDo:
        return 0;
    default:
        return 0;
    }
}


AEffect* callEntrySafely(EntryProc entry, AudioMasterCallback host, bool& exception) noexcept {
    exception = false;
#if defined(_MSC_VER)
    __try {
        return entry(host);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        exception = true;
        return nullptr;
    }
#else
    return entry(host);
#endif
}

bool callDispatcherSafely(AEffect* effect,
                          VstInt32 opcode,
                          VstInt32 index,
                          VstIntPtr value,
                          void* ptr,
                          float opt,
                          VstIntPtr* result = nullptr) noexcept {
#if defined(_MSC_VER)
    __try {
        const auto r = effect->dispatcher(effect, opcode, index, value, ptr, opt);
        if (result) *result = r;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (result) *result = 0;
        return false;
    }
#else
    const auto r = effect->dispatcher(effect, opcode, index, value, ptr, opt);
    if (result) *result = r;
    return true;
#endif
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

    bool entryException = false;
    AEffect* effect = callEntrySafely(entry, hostCallback, entryException);
    if (entryException) {
        result.error = "exception while creating VST2 instance";
        close();
        return result;
    }

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

    if (!callDispatcherSafely(effect, EffOpen, 0, 0, nullptr, 0.0f)) {
        result.error = "exception during effOpen";
        close();
        return result;
    }
    effOpenCalled_ = true;

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
        (void)callDispatcherSafely(effect, EffClose, 0, 0, nullptr, 0.0f);
    }
    effOpenCalled_ = false;

    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
}

} // namespace pluginscaler::formats
