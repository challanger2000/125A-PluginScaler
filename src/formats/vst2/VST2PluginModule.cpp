#include "pluginscaler/formats/VST2PluginModule.h"

#include <windows.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

namespace pluginscaler::formats {
namespace {

using namespace vst2abi;

constexpr std::size_t kHostModuleSlots = 128;
constexpr std::uintptr_t kClaimedHostSlot = 1;

struct HostModuleSlot {
    std::atomic<std::uintptr_t> effectKey{0};
    std::atomic<VST2PluginModule*> module{nullptr};
};

std::array<HostModuleSlot, kHostModuleSlots> g_hostModuleSlots{};
thread_local VST2PluginModule* g_constructingModule = nullptr;
thread_local bool g_inRealtimeProcess = false;

bool registerHostModule(AEffect* effect, VST2PluginModule* module) noexcept {
    if (!effect || !module)
        return false;
    const auto key = reinterpret_cast<std::uintptr_t>(effect);
    for (auto& slot : g_hostModuleSlots) {
        std::uintptr_t expected = 0;
        if (!slot.effectKey.compare_exchange_strong(
                expected, kClaimedHostSlot,
                std::memory_order_acq_rel,
                std::memory_order_relaxed))
            continue;
        slot.module.store(module, std::memory_order_relaxed);
        slot.effectKey.store(key, std::memory_order_release);
        return true;
    }
    return false;
}

void unregisterHostModule(AEffect* effect) noexcept {
    if (!effect)
        return;
    const auto key = reinterpret_cast<std::uintptr_t>(effect);
    for (auto& slot : g_hostModuleSlots) {
        if (slot.effectKey.load(std::memory_order_acquire) != key)
            continue;
        std::uintptr_t expected = key;
        if (slot.effectKey.compare_exchange_strong(
                expected, kClaimedHostSlot,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            slot.module.store(nullptr, std::memory_order_release);
            slot.effectKey.store(0, std::memory_order_release);
        }
        return;
    }
}

VST2PluginModule* moduleForHostCallback(AEffect* effect) noexcept {
    if (!effect)
        return g_constructingModule;

    const auto key = reinterpret_cast<std::uintptr_t>(effect);
    for (auto& slot : g_hostModuleSlots) {
        if (slot.effectKey.load(std::memory_order_acquire) == key)
            return slot.module.load(std::memory_order_acquire);
    }

    // Some VST2 plugins call the host with their non-null AEffect from inside
    // VSTPluginMain(), before registration can happen.
    return g_constructingModule;
}

VstIntPtr __cdecl hostCallback(AEffect* effect, VstInt32 opcode, VstInt32 index,
                               VstIntPtr value, void* ptr, float opt) {
    auto* module = moduleForHostCallback(effect);
    switch (opcode) {
    case AudioMasterVersion:
        return 2400;
    case AudioMasterCurrentId:
        return module ? static_cast<VstIntPtr>(module->uniqueId()) : 0;
    case AudioMasterIdle:
        // The plug-in is yielding to the host (for example during a modal
        // editor loop). Never recurse into the plug-in dispatcher here.
        return 0;
    case AudioMasterWantMidi:
        if (module)
            module->noteWantMidiRequest();
        return 1;
    case AudioMasterNeedIdle:
        // Deprecated VST2 idle contract: schedule effIdle from the host owner
        // thread until the plug-in returns 0. This is distinct from effEditIdle.
        if (module)
            module->requestLegacyIdle();
        return module ? 1 : 0;
    case AudioMasterSizeWindow:
        return module &&
               module->requestHostWindowResize(
                   index, static_cast<std::int32_t>(value))
            ? 1
            : 0;
    case AudioMasterAutomate:
    case AudioMasterBeginEdit:
    case AudioMasterEndEdit:
    case AudioMasterIOChanged:
    case AudioMasterUpdateDisplay:
        // Notifications are forwarded asynchronously by the helper. The
        // callback itself must never wait for the x64 proxy/DAW.
        if (module)
            module->emitHostCallback(opcode, index, value, opt);
        return 1;
    case AudioMasterGetTime:
        return module
            ? reinterpret_cast<VstIntPtr>(
                  module->hostTimeInfoForRequest(value))
            : 0;
    case AudioMasterGetSampleRate:
        return module ? static_cast<VstIntPtr>(module->sampleRate()) : 48000;
    case AudioMasterGetBlockSize:
        return module ? static_cast<VstIntPtr>(module->blockSize()) : 512;
    case AudioMasterGetCurrentProcessLevel:
        return g_inRealtimeProcess ? 2 : 1;
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
    case AudioMasterGetDirectory:
        return module
            ? reinterpret_cast<VstIntPtr>(module->pluginDirectoryAnsi())
            : 0;
    case AudioMasterCanDo:
        if (!ptr) return 0;
        if (std::strcmp(static_cast<const char*>(ptr), "sendVstEvents") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "sendVstMidiEvent") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "receiveVstEvents") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "receiveVstMidiEvent") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "sizeWindow") == 0)
            return 1;
        return 0;
    default:
        (void)value;
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

bool callSetParameterSafely(AEffect* effect, VstInt32 index, float value) noexcept {
    if (!effect || !effect->setParameter) return false;
#if defined(_MSC_VER)
    __try {
        effect->setParameter(effect, index, value);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    effect->setParameter(effect, index, value);
    return true;
#endif
}

float callGetParameterSafely(AEffect* effect, VstInt32 index) noexcept {
    if (!effect || !effect->getParameter) return 0.0f;
#if defined(_MSC_VER)
    __try {
        return effect->getParameter(effect, index);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
#else
    return effect->getParameter(effect, index);
#endif
}

bool callProcessReplacingSafely(AEffect* effect,
                                float** inputs,
                                float** outputs,
                                VstInt32 frames) noexcept {
    if (!effect || !effect->processReplacing) return false;
#if defined(_MSC_VER)
    __try {
        effect->processReplacing(effect, inputs, outputs, frames);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    effect->processReplacing(effect, inputs, outputs, frames);
    return true;
#endif
}

std::string queryString(AEffect* effect, VstInt32 opcode) {
    if (!effect || !effect->dispatcher) return {};
    std::array<char, 256> buffer{};
    VstIntPtr ignored = 0;
    if (!callDispatcherSafely(effect, opcode, 0, 0, buffer.data(), 0.0f, &ignored))
        return {};
    if (!ignored) return {};
    buffer.back() = '\0';
    return std::string(buffer.data());
}

std::string win32Error(const char* prefix) {
    return std::string(prefix) + " (Win32=" + std::to_string(GetLastError()) + ")";
}

std::string wideToAnsi(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<std::size_t>(bytes), char{});
    if (WideCharToMultiByte(
            CP_ACP, 0, text.c_str(), -1, result.data(), bytes,
            nullptr, nullptr) <= 0)
        return {};
    result.resize(static_cast<std::size_t>(bytes - 1));
    return result;
}

} // namespace

VST2PluginModule::~VST2PluginModule() {
    close();
}

bool VST2PluginModule::loadAndOpen(const std::filesystem::path& path, std::string& error) {
    close();
    wantsMidi_ = false;

    if (path.empty()) {
        error = "empty plugin path";
        return false;
    }

    pluginDirectoryAnsi_ = wideToAnsi(path.parent_path().wstring());

    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        error = win32Error("LoadLibraryExW failed");
        return false;
    }
    module_ = module;

    EntryProc entry = reinterpret_cast<EntryProc>(GetProcAddress(module, "VSTPluginMain"));
    if (!entry)
        entry = reinterpret_cast<EntryProc>(GetProcAddress(module, "main"));

    if (!entry) {
        error = "VST2 entry point not found";
        close();
        return false;
    }

    bool entryException = false;
    g_constructingModule = this;
    AEffect* effect = callEntrySafely(entry, hostCallback, entryException);
    g_constructingModule = nullptr;
    if (entryException) {
        error = "exception while creating VST2 instance";
        close();
        return false;
    }
    if (!effect) {
        error = "VST2 entry point returned null";
        close();
        return false;
    }
    effect_ = effect;
    if (!registerHostModule(effect_, this)) {
        error = "VST2 host instance registry is full";
        close();
        return false;
    }

    if (effect->magic != kEffectMagic) {
        error = "invalid AEffect magic";
        close();
        return false;
    }
    if (!effect->dispatcher) {
        error = "AEffect dispatcher is null";
        close();
        return false;
    }

    if (!callDispatcherSafely(effect, EffOpen, 0, 0, nullptr, 0.0f)) {
        error = "exception during effOpen";
        close();
        return false;
    }

    effOpenCalled_ = true;

    VstIntPtr vstVersion = 0;
    if (callDispatcherSafely(
            effect_, EffGetVstVersion, 0, 0, nullptr, 0.0f, &vstVersion))
        supportsStartStopProcess_ = vstVersion >= 2400;
    else
        supportsStartStopProcess_ = false;

    return true;
}

VST2ProbeResult VST2PluginModule::probe(const std::filesystem::path& path) {
    VST2ProbeResult result;
    std::string error;
    if (!loadAndOpen(path, error)) {
        result.error = std::move(error);
        return result;
    }

    result.loaded = true;
    result.opened = true;
    result.entryPoint =
        GetProcAddress(static_cast<HMODULE>(module_), "VSTPluginMain") ? "VSTPluginMain" : "main";
    result.uniqueId = effect_->uniqueId;
    result.version = effect_->version;
    result.numPrograms = effect_->numPrograms;
    result.numParams = effect_->numParams;
    result.numInputs = effect_->numInputs;
    result.numOutputs = effect_->numOutputs;
    result.flags = effect_->flags;

    VstIntPtr category = 0;
    if (callDispatcherSafely(effect_, EffGetPlugCategory, 0, 0, nullptr, 0.0f, &category))
        result.plugCategory = static_cast<std::int32_t>(category);

    VstIntPtr midiInputs = 0;
    if (callDispatcherSafely(effect_, EffGetNumMidiInputChannels, 0, 0, nullptr, 0.0f, &midiInputs) &&
        midiInputs > 0)
        result.midiInputChannels = static_cast<std::int32_t>(midiInputs);

    const auto queryCanDo = [&](const char* capability) {
        VstIntPtr value = 0;
        return callDispatcherSafely(effect_, EffCanDo, 0, 0,
                                    const_cast<char*>(capability), 0.0f, &value) &&
               value > 0;
    };
    result.receivesVstEvents = queryCanDo("receiveVstEvents");
    result.receivesVstMidiEvents = queryCanDo("receiveVstMidiEvent");
    result.wantsMidi = wantsMidi_;

    if (effect_->numParams > 0 && effect_->getParameter) {
        const auto count = static_cast<std::size_t>(effect_->numParams);
        result.parameterDefaults.resize(count);
        result.parameterNames.resize(count);
        result.parameterLabels.resize(count);
        result.parameterAutomatable.resize(count, true);

        for (VstInt32 i = 0; i < effect_->numParams; ++i) {
            const auto pos = static_cast<std::size_t>(i);
            result.parameterDefaults[pos] = callGetParameterSafely(effect_, i);

            std::array<char, 8> name{};
            VstIntPtr nameResult = 0;
            if (callDispatcherSafely(effect_, EffGetParamName, i, 0,
                                     name.data(), 0.0f, &nameResult)) {
                name.back() = '\0';
                result.parameterNames[pos] = name.data();
            }

            std::array<char, 8> label{};
            VstIntPtr labelResult = 0;
            if (callDispatcherSafely(effect_, EffGetParamLabel, i, 0,
                                     label.data(), 0.0f, &labelResult)) {
                label.back() = '\0';
                result.parameterLabels[pos] = label.data();
            }

            VstIntPtr automateResult = 1;
            if (callDispatcherSafely(effect_, EffCanBeAutomated, i, 0,
                                     nullptr, 0.0f, &automateResult))
                result.parameterAutomatable[pos] = automateResult != 0;
        }
    }

    if (effect_->numPrograms > 0) {
        result.programNames.resize(static_cast<std::size_t>(effect_->numPrograms));
        for (VstInt32 i = 0; i < effect_->numPrograms; ++i) {
            std::array<char, 24> name{};
            VstIntPtr programResult = 0;
            if (callDispatcherSafely(effect_, EffGetProgramNameIndexed, i, 0,
                                     name.data(), 0.0f, &programResult) &&
                programResult != 0) {
                name.back() = '\0';
                result.programNames[static_cast<std::size_t>(i)] = name.data();
            }
        }
    }

    result.effectName = queryString(effect_, EffGetEffectName);
    result.vendor = queryString(effect_, EffGetVendorString);
    result.product = queryString(effect_, EffGetProductString);

    close();
    result.closed = true;
    return result;
}

bool VST2PluginModule::openForProcessing(const std::filesystem::path& path,
                                         double sampleRate,
                                         std::int32_t blockSize,
                                         std::string& error) {
    if (sampleRate <= 0.0 || blockSize <= 0) {
        error = "invalid audio configuration";
        return false;
    }

    if (!loadAndOpen(path, error))
        return false;

    sampleRate_ = sampleRate;
    blockSize_ = blockSize;
    timeInfo_ = {};
    timeInfo_.sampleRate = sampleRate_;

    if (!callDispatcherSafely(effect_, EffSetSampleRate, 0, 0, nullptr,
                              static_cast<float>(sampleRate_)) ||
        !callDispatcherSafely(effect_, EffSetBlockSize, 0,
                              static_cast<VstIntPtr>(blockSize_), nullptr, 0.0f)) {
        error = "exception while configuring VST2 audio";
        close();
        return false;
    }

    if (!effect_->processReplacing) {
        error = "processReplacing not available";
        close();
        return false;
    }

    if (!setMains(true)) {
        error = "exception during VST2 processing start";
        close();
        return false;
    }

    return true;
}

bool VST2PluginModule::setMains(bool active) noexcept {
    if (!effect_ || !effect_->dispatcher)
        return false;
    if (mainsOn_ == active)
        return true;

    if (active) {
        if (!callDispatcherSafely(
                effect_, EffMainsChanged, 0, 1, nullptr, 0.0f))
            return false;
        mainsOn_ = true;

        if (supportsStartStopProcess_) {
            VstIntPtr ignored = 0;
            if (!callDispatcherSafely(
                    effect_, EffStartProcess, 0, 0, nullptr, 0.0f, &ignored)) {
                (void)callDispatcherSafely(
                    effect_, EffMainsChanged, 0, 0, nullptr, 0.0f);
                mainsOn_ = false;
                return false;
            }
            processStarted_ = true;
        }
        return true;
    }

    if (processStarted_) {
        VstIntPtr ignored = 0;
        (void)callDispatcherSafely(
            effect_, EffStopProcess, 0, 0, nullptr, 0.0f, &ignored);
        processStarted_ = false;
    }

    if (!callDispatcherSafely(
            effect_, EffMainsChanged, 0, 0, nullptr, 0.0f))
        return false;
    mainsOn_ = false;
    return true;
}

bool VST2PluginModule::reconfigureProcessing(double sampleRate,
                                             std::int32_t blockSize) noexcept {
    if (!effect_ || !effect_->dispatcher || sampleRate <= 0.0 || blockSize <= 0)
        return false;

    const bool wasOn = mainsOn_;
    if (wasOn && !setMains(false))
        return false;

    sampleRate_ = sampleRate;
    blockSize_ = blockSize;
    timeInfo_.sampleRate = sampleRate_;

    if (!callDispatcherSafely(effect_, EffSetSampleRate, 0, 0, nullptr,
                              static_cast<float>(sampleRate_)) ||
        !callDispatcherSafely(effect_, EffSetBlockSize, 0,
                              static_cast<VstIntPtr>(blockSize_), nullptr, 0.0f))
        return false;

    return !wasOn || setMains(true);
}

void VST2PluginModule::setHostTimeInfo(const VstTimeInfo& info) noexcept {
    timeInfo_ = info;
    if (timeInfo_.sampleRate <= 0.0)
        timeInfo_.sampleRate = sampleRate_;
}

const VstTimeInfo* VST2PluginModule::hostTimeInfoForRequest(
    VstIntPtr requestedFlags) noexcept {
    constexpr VstInt32 kValidityMask =
        VstNanosValid | VstPpqPosValid | VstTempoValid | VstBarsValid |
        VstCyclePosValid | VstTimeSigValid | VstSmpteValid | VstClockValid;

    timeInfoView_ = timeInfo_;
    const auto requested = static_cast<VstInt32>(requestedFlags);
    const auto stateFlags = timeInfo_.flags & ~kValidityMask;
    const auto validRequested =
        timeInfo_.flags & requested & kValidityMask;
    timeInfoView_.flags = stateFlags | validRequested;
    return &timeInfoView_;
}

bool VST2PluginModule::processReplacing(float** inputs, float** outputs,
                                        std::int32_t frames) noexcept {
    if (!effect_ || !mainsOn_ || frames <= 0)
        return false;
    g_inRealtimeProcess = true;
    const bool ok = callProcessReplacingSafely(
        effect_, inputs, outputs, frames);
    g_inRealtimeProcess = false;
    return ok;
}

bool VST2PluginModule::processMidiEvents(const VstMidiEvent* events,
                                         std::int32_t eventCount) noexcept {
    if (!effect_ || !mainsOn_ || !effect_->dispatcher || !events || eventCount <= 0)
        return eventCount == 0;

    constexpr std::int32_t kMaxRealtimeMidiEvents = 256;
    if (eventCount > kMaxRealtimeMidiEvents)
        return false;

    struct FixedVstEvents {
        VstInt32 numEvents;
        VstIntPtr reserved;
        VstEvent* events[kMaxRealtimeMidiEvents];
    };
    static_assert(offsetof(FixedVstEvents, events) == offsetof(VstEvents, events));

    FixedVstEvents list{};
    list.numEvents = eventCount;
    for (std::int32_t i = 0; i < eventCount; ++i)
        list.events[i] =
            reinterpret_cast<VstEvent*>(const_cast<VstMidiEvent*>(&events[i]));

    VstIntPtr result = 0;
    // No heap allocation is allowed on the realtime event path.
    return callDispatcherSafely(
        effect_, EffProcessEvents, 0, 0, &list, 0.0f, &result);
}

bool VST2PluginModule::setParameter(std::int32_t index, float value) noexcept {
    if (!effect_ || index < 0 || index >= effect_->numParams) return false;
    return callSetParameterSafely(effect_, index, value);
}

float VST2PluginModule::getParameter(std::int32_t index) const noexcept {
    if (!effect_ || index < 0 || index >= effect_->numParams) return 0.0f;
    return callGetParameterSafely(effect_, index);
}

VstIntPtr VST2PluginModule::dispatch(std::int32_t opcode,
                                     std::int32_t index,
                                     VstIntPtr value,
                                     void* ptr,
                                     float opt) noexcept {
    if (!effect_ || !effect_->dispatcher)
        return 0;

    if (opcode == EffSetProgram) {
        VstIntPtr ignored = 0;
        (void)callDispatcherSafely(
            effect_, EffBeginSetProgram, 0, 0, nullptr, 0.0f, &ignored);

        VstIntPtr result = 0;
        const bool ok = callDispatcherSafely(
            effect_, EffSetProgram, index, value, ptr, opt, &result);

        (void)callDispatcherSafely(
            effect_, EffEndSetProgram, 0, 0, nullptr, 0.0f, &ignored);
        return ok ? result : 0;
    }

    VstIntPtr result = 0;
    if (!callDispatcherSafely(effect_, opcode, index, value, ptr, opt, &result))
        return 0;
    return result;
}

bool VST2PluginModule::getChunk(std::int32_t index,
                                std::vector<std::uint8_t>& data) noexcept {
    data.clear();
    if (!effect_ || !effect_->dispatcher) return false;

    void* chunk = nullptr;
    VstIntPtr bytes = 0;
    if (!callDispatcherSafely(effect_, EffGetChunk, index, 0, &chunk, 0.0f, &bytes) ||
        bytes <= 0 || !chunk)
        return false;

    const auto size = static_cast<std::size_t>(bytes);
    data.resize(size);
    std::memcpy(data.data(), chunk, size);
    return true;
}

bool VST2PluginModule::setChunk(std::int32_t index, const void* data,
                                std::size_t bytes) noexcept {
    if (!effect_ || !effect_->dispatcher || !data || bytes == 0 ||
        bytes > static_cast<std::size_t>(INTPTR_MAX))
        return false;

    VstPatchChunkInfo info{};
    info.version = 1;
    info.pluginUniqueID = effect_->uniqueId;
    info.pluginVersion = effect_->version;
    info.numElements = index != 0 ? 1 : effect_->numPrograms;

    VstIntPtr beginLoadResult = 0;
    const auto beginLoadOpcode =
        index != 0 ? EffBeginLoadProgram : EffBeginLoadBank;
    if (!callDispatcherSafely(effect_, beginLoadOpcode, 0, 0,
                              &info, 0.0f, &beginLoadResult))
        return false;

    // VST2 uses a negative return as an explicit rejection. Zero must remain
    // compatible with older plugins that simply do not implement this hint.
    if (beginLoadResult < 0)
        return false;

    VstIntPtr ignored = 0;
    (void)callDispatcherSafely(
        effect_, EffBeginSetProgram, 0, 0, nullptr, 0.0f, &ignored);

    VstIntPtr result = 0;
    const bool dispatched = callDispatcherSafely(
        effect_, EffSetChunk, index,
        static_cast<VstIntPtr>(bytes),
        const_cast<void*>(data), 0.0f, &result);

    (void)callDispatcherSafely(
        effect_, EffEndSetProgram, 0, 0, nullptr, 0.0f, &ignored);

    return dispatched && result != 0;
}

bool VST2PluginModule::editorRect(VstRect& rect) noexcept {
    if (!effect_ || !effect_->dispatcher) return false;
    VstRect* pluginRect = nullptr;
    VstIntPtr result = 0;
    if (!callDispatcherSafely(effect_, EffEditGetRect, 0, 0, &pluginRect, 0.0f, &result) ||
        !result || !pluginRect)
        return false;
    rect = *pluginRect;
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool VST2PluginModule::openEditor(void* parentWindow) noexcept {
    if (!effect_ || !effect_->dispatcher || !parentWindow) return false;
    if (editorOpen_.load(std::memory_order_acquire))
        return true;

    // Old VST2 editors may create their HWND successfully and still return 0.
    VstIntPtr ignored = 0;
    const bool ok = callDispatcherSafely(
        effect_, EffEditOpen, 0, 0, parentWindow, 0.0f, &ignored);
    if (ok)
        editorOpen_.store(true, std::memory_order_release);
    return ok;
}

bool VST2PluginModule::closeEditor() noexcept {
    if (!effect_ || !effect_->dispatcher) return false;
    if (!editorOpen_.exchange(false, std::memory_order_acq_rel))
        return true;
    VstIntPtr result = 0;
    return callDispatcherSafely(
        effect_, EffEditClose, 0, 0, nullptr, 0.0f, &result);
}

bool VST2PluginModule::editorIdle() noexcept {
    if (!effect_ || !effect_->dispatcher) return false;
    if (!editorOpen_.load(std::memory_order_acquire))
        return true;

    if (editorIdleActive_.test_and_set(std::memory_order_acquire))
        return true;

    VstIntPtr result = 0;
    const bool ok = callDispatcherSafely(
        effect_, EffEditIdle, 0, 0, nullptr, 0.0f, &result);
    editorIdleActive_.clear(std::memory_order_release);
    return ok;
}

bool VST2PluginModule::serviceLegacyIdle() noexcept {
    if (!effect_ || !effect_->dispatcher)
        return false;
    if (!legacyIdleRequested_.load(std::memory_order_acquire))
        return true;

    if (legacyIdleActive_.test_and_set(std::memory_order_acquire))
        return true;

    VstIntPtr result = 0;
    const bool ok = callDispatcherSafely(
        effect_, EffIdle, 0, 0, nullptr, 0.0f, &result);
    if (!ok || result == 0)
        legacyIdleRequested_.store(false, std::memory_order_release);

    legacyIdleActive_.clear(std::memory_order_release);
    return ok;
}

std::int32_t VST2PluginModule::numParams() const noexcept {
    return effect_ ? effect_->numParams : 0;
}

std::int32_t VST2PluginModule::numInputs() const noexcept {
    return effect_ ? effect_->numInputs : 0;
}

std::int32_t VST2PluginModule::numOutputs() const noexcept {
    return effect_ ? effect_->numOutputs : 0;
}

VST2AudioProbeResult VST2PluginModule::probeAudio(const std::filesystem::path& path,
                                                 double sampleRate,
                                                 std::int32_t blockSize) {
    VST2AudioProbeResult result;
    std::string error;
    if (!openForProcessing(path, sampleRate, blockSize, error)) {
        result.error = std::move(error);
        return result;
    }

    result.loaded = true;
    result.opened = true;
    result.configured = true;
    result.mainsOn = true;

    const auto inputsCount = std::max<std::int32_t>(1, effect_->numInputs);
    const auto outputsCount = std::max<std::int32_t>(1, effect_->numOutputs);

    std::vector<std::vector<float>> inputStorage(
        static_cast<std::size_t>(inputsCount),
        std::vector<float>(static_cast<std::size_t>(blockSize), 0.0f));
    std::vector<std::vector<float>> outputStorage(
        static_cast<std::size_t>(outputsCount),
        std::vector<float>(static_cast<std::size_t>(blockSize), 0.0f));

    for (std::int32_t ch = 0; ch < inputsCount; ++ch) {
        for (std::int32_t i = 0; i < blockSize; ++i) {
            inputStorage[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] =
                static_cast<float>((i + 1) * (ch + 1)) / 100.0f;
        }
    }

    std::vector<float*> inputs(static_cast<std::size_t>(inputsCount));
    std::vector<float*> outputs(static_cast<std::size_t>(outputsCount));
    for (std::int32_t ch = 0; ch < inputsCount; ++ch)
        inputs[static_cast<std::size_t>(ch)] = inputStorage[static_cast<std::size_t>(ch)].data();
    for (std::int32_t ch = 0; ch < outputsCount; ++ch)
        outputs[static_cast<std::size_t>(ch)] = outputStorage[static_cast<std::size_t>(ch)].data();

    if (!processReplacing(inputs.data(), outputs.data(), blockSize)) {
        result.error = "exception during processReplacing";
        close();
        result.closed = true;
        return result;
    }

    result.processed = true;
    result.firstOutputLeft = outputStorage[0][0];
    result.firstOutputRight =
        outputStorage[static_cast<std::size_t>(std::min<std::int32_t>(1, outputsCount - 1))][0];

    if (!callDispatcherSafely(effect_, EffMainsChanged, 0, 0, nullptr, 0.0f)) {
        result.error = "exception during mains-off";
        mainsOn_ = false;
        close();
        result.closed = true;
        return result;
    }

    mainsOn_ = false;
    result.mainsOff = true;
    close();
    result.closed = true;
    return result;
}

void VST2PluginModule::emitHostCallback(std::int32_t opcode,
                                            std::int32_t index,
                                            VstIntPtr value,
                                            float opt) noexcept {
    if (hostCallbackSink_)
        hostCallbackSink_(hostCallbackContext_, opcode, index, value, opt);
}

void VST2PluginModule::close() noexcept {
    auto* effect = effect_;
    effect_ = nullptr;

    if (effect && editorOpen_.exchange(false, std::memory_order_acq_rel) &&
        effect->dispatcher) {
        (void)callDispatcherSafely(
            effect, EffEditClose, 0, 0, nullptr, 0.0f);
    }

    if (effect && effect->dispatcher && processStarted_) {
        VstIntPtr ignored = 0;
        (void)callDispatcherSafely(
            effect, EffStopProcess, 0, 0, nullptr, 0.0f, &ignored);
    }
    processStarted_ = false;
    legacyIdleRequested_.store(false, std::memory_order_release);
    legacyIdleActive_.clear(std::memory_order_release);

    if (effect && mainsOn_ && effect->dispatcher)
        (void)callDispatcherSafely(
            effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
    mainsOn_ = false;
    supportsStartStopProcess_ = false;

    if (effect && effOpenCalled_ && effect->dispatcher)
        (void)callDispatcherSafely(effect, EffClose, 0, 0, nullptr, 0.0f);
    effOpenCalled_ = false;

    // Keep the callback registration alive through stop/mains-off/effClose;
    // legacy plugins may still call the host during those lifecycle calls.
    if (effect)
        unregisterHostModule(effect);

    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }

    pluginDirectoryAnsi_.clear();
    editorIdleActive_.clear(std::memory_order_release);
}

} // namespace pluginscaler::formats
