#include "pluginscaler/formats/VST2PluginModule.h"

#include <windows.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace pluginscaler::formats {
namespace {

using namespace vst2abi;

std::mutex g_hostModuleMutex;
std::unordered_map<AEffect*, VST2PluginModule*> g_hostModules;
thread_local VST2PluginModule* g_constructingModule = nullptr;

VST2PluginModule* moduleForHostCallback(AEffect* effect) {
    if (!effect)
        return g_constructingModule;

    {
        std::lock_guard<std::mutex> lock(g_hostModuleMutex);
        const auto it = g_hostModules.find(effect);
        if (it != g_hostModules.end())
            return it->second;
    }

    // Some VST2 plugins call the host with their non-null AEffect from inside
    // VSTPluginMain(), before we can register that pointer in g_hostModules.
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
        if (module)
            (void)module->editorIdle();
        return 0;
    case AudioMasterWantMidi:
        if (module)
            module->noteWantMidiRequest();
        return 1;
    case AudioMasterNeedIdle:
        // The helper owns a Win32 message loop and calls effEditIdle
        // periodically while the editor is open.
        return 1;
    case AudioMasterAutomate:
    case AudioMasterBeginEdit:
    case AudioMasterEndEdit:
        // GUI gestures are notifications. Queue them to the proxy and return
        // immediately; never block the legacy plugin waiting for the DAW.
        if (module)
            module->emitHostCallback(opcode, index, value, opt);
        return 1;
    case AudioMasterUpdateDisplay:
        // These are normal GUI/program-change notifications. The x86 helper
        // currently has no automation backchannel to the x64 host yet, but the
        // callback itself must be acknowledged so legacy editors do not wait
        // for a host response that never comes.
        return 1;
    case AudioMasterGetTime:
        return module
            ? reinterpret_cast<VstIntPtr>(module->hostTimeInfo())
            : 0;
    case AudioMasterGetSampleRate:
        return module ? static_cast<VstIntPtr>(module->sampleRate()) : 48000;
    case AudioMasterGetBlockSize:
        return module ? static_cast<VstIntPtr>(module->blockSize()) : 512;
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
            std::strcmp(static_cast<const char*>(ptr), "receiveVstMidiEvent") == 0)
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
    {
        std::lock_guard<std::mutex> lock(g_hostModuleMutex);
        g_hostModules[effect_] = this;
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

    if (!callDispatcherSafely(effect_, EffMainsChanged, 0, 1, nullptr, 0.0f)) {
        error = "exception during mains-on";
        close();
        return false;
    }

    mainsOn_ = true;
    return true;
}

bool VST2PluginModule::setMains(bool active) noexcept {
    if (!effect_ || !effect_->dispatcher)
        return false;
    if (mainsOn_ == active)
        return true;
    if (!callDispatcherSafely(effect_, EffMainsChanged, 0,
                              active ? 1 : 0, nullptr, 0.0f))
        return false;
    mainsOn_ = active;
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

bool VST2PluginModule::processReplacing(float** inputs, float** outputs, std::int32_t frames) noexcept {
    if (!effect_ || !mainsOn_ || frames <= 0) return false;
    return callProcessReplacingSafely(effect_, inputs, outputs, frames);
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
    VstIntPtr result = 0;
    return callDispatcherSafely(effect_, EffSetChunk, index,
                                static_cast<VstIntPtr>(bytes),
                                const_cast<void*>(data), 0.0f, &result) &&
           result != 0;
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

    if (effect) {
        std::lock_guard<std::mutex> lock(g_hostModuleMutex);
        g_hostModules.erase(effect);
    }

    if (effect && mainsOn_ && effect->dispatcher)
        (void)callDispatcherSafely(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
    mainsOn_ = false;

    if (effect && effOpenCalled_ && effect->dispatcher)
        (void)callDispatcherSafely(effect, EffClose, 0, 0, nullptr, 0.0f);
    effOpenCalled_ = false;

    if (module_) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }

    pluginDirectoryAnsi_.clear();
    editorIdleActive_.clear(std::memory_order_release);
}

} // namespace pluginscaler::formats
