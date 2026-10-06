#pragma once

#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>

namespace pluginscaler::formats {

struct VST2ProbeResult {
    bool loaded{false};
    bool opened{false};
    bool closed{false};
    std::string entryPoint;
    std::string effectName;
    std::string vendor;
    std::string product;
    std::int32_t uniqueId{0};
    std::int32_t version{0};
    std::int32_t numPrograms{0};
    std::int32_t numParams{0};
    std::int32_t numInputs{0};
    std::int32_t numOutputs{0};
    std::int32_t flags{0};
    std::int32_t plugCategory{vst2abi::PlugCategUnknown};
    std::int32_t midiInputChannels{0};
    bool receivesVstEvents{false};
    bool receivesVstMidiEvents{false};
    bool wantsMidi{false};
    std::vector<float> parameterDefaults;
    std::vector<std::string> parameterNames;
    std::vector<std::string> parameterLabels;
    std::vector<bool> parameterAutomatable;
    std::vector<std::string> programNames;
    std::string error;
};

struct VST2AudioProbeResult {
    bool loaded{false};
    bool opened{false};
    bool configured{false};
    bool mainsOn{false};
    bool processed{false};
    bool mainsOff{false};
    bool closed{false};
    float firstOutputLeft{0.0f};
    float firstOutputRight{0.0f};
    std::string error;
};

class VST2PluginModule {
public:
    using HostCallbackSink = bool (*)(void* context,
                                      std::int32_t opcode,
                                      std::int32_t index,
                                      vst2abi::VstIntPtr value,
                                      float opt,
                                      std::int32_t numInputs,
                                      std::int32_t numOutputs,
                                      std::int32_t initialDelay,
                                      std::int32_t effectFlags) noexcept;
    using HostWindowResizeSink = bool (*)(void* context,
                                         std::int32_t width,
                                         std::int32_t height) noexcept;
    using HostMidiOutputSink = bool (*)(void* context,
                                       const vst2abi::VstEvents* events) noexcept;

    VST2PluginModule() = default;
    ~VST2PluginModule();

    VST2PluginModule(const VST2PluginModule&) = delete;
    VST2PluginModule& operator=(const VST2PluginModule&) = delete;

    VST2ProbeResult probe(const std::filesystem::path& path);
    VST2AudioProbeResult probeAudio(const std::filesystem::path& path,
                                    double sampleRate = 48000.0,
                                    std::int32_t blockSize = 64);

    bool openForProcessing(const std::filesystem::path& path,
                           double sampleRate,
                           std::int32_t blockSize,
                           std::string& error);
    bool reconfigureProcessing(double sampleRate,
                               std::int32_t blockSize) noexcept;
    bool setMains(bool active) noexcept;
    void setHostTimeInfo(const vst2abi::VstTimeInfo& info) noexcept;
    void setHostRuntimeContext(std::int32_t processLevel,
                               std::int32_t automationState) noexcept {
        hostProcessLevel_ = processLevel;
        hostAutomationState_ = automationState;
    }
    const vst2abi::VstTimeInfo* hostTimeInfoForRequest(
        vst2abi::VstIntPtr requestedFlags) noexcept;
    double sampleRate() const noexcept { return sampleRate_; }
    std::int32_t blockSize() const noexcept { return blockSize_; }
    std::int32_t hostProcessLevel() const noexcept {
        return hostProcessLevel_;
    }
    std::int32_t hostAutomationState() const noexcept {
        return hostAutomationState_;
    }
    std::int32_t uniqueId() const noexcept { return effect_ ? effect_->uniqueId : 0; }
    const char* pluginDirectoryAnsi() const noexcept {
        return pluginDirectoryAnsi_.empty() ? nullptr : pluginDirectoryAnsi_.c_str();
    }
    void noteWantMidiRequest() noexcept { wantsMidi_ = true; }
    bool wantsMidi() const noexcept { return wantsMidi_; }
    void setHostCallbackSink(HostCallbackSink sink, void* context) noexcept {
        hostCallbackSink_ = sink;
        hostCallbackContext_ = context;
    }
    bool emitHostCallback(std::int32_t opcode,
                          std::int32_t index,
                          vst2abi::VstIntPtr value,
                          float opt) noexcept;
    void setHostWindowResizeSink(HostWindowResizeSink sink,
                                 void* context) noexcept {
        hostWindowResizeSink_ = sink;
        hostWindowResizeContext_ = context;
    }
    bool requestHostWindowResize(std::int32_t width,
                                 std::int32_t height) noexcept {
        return hostWindowResizeSink_
            ? hostWindowResizeSink_(hostWindowResizeContext_, width, height)
            : false;
    }
    void setHostMidiOutputSink(HostMidiOutputSink sink,
                               void* context) noexcept {
        hostMidiOutputSink_ = sink;
        hostMidiOutputContext_ = context;
    }
    bool emitHostMidiOutput(const vst2abi::VstEvents* events) noexcept {
        return hostMidiOutputSink_
            ? hostMidiOutputSink_(hostMidiOutputContext_, events)
            : false;
    }
    bool processReplacing(float** inputs, float** outputs, std::int32_t frames) noexcept;
    bool processMidiEvents(const vst2abi::VstMidiEvent* events,
                           std::int32_t eventCount) noexcept;
    bool setParameter(std::int32_t index, float value) noexcept;
    float getParameter(std::int32_t index) const noexcept;
    vst2abi::VstIntPtr dispatch(std::int32_t opcode,
                                std::int32_t index = 0,
                                vst2abi::VstIntPtr value = 0,
                                void* ptr = nullptr,
                                float opt = 0.0f) noexcept;
    bool getChunk(std::int32_t index, std::vector<std::uint8_t>& data) noexcept;
    bool setChunk(std::int32_t index, const void* data, std::size_t bytes) noexcept;
    bool editorRect(vst2abi::VstRect& rect) noexcept;
    bool openEditor(void* parentWindow) noexcept;
    bool closeEditor() noexcept;
    bool editorIdle() noexcept;
    bool serviceLegacyIdle() noexcept;
    void requestLegacyIdle() noexcept {
        legacyIdleRequested_.store(true, std::memory_order_release);
    }
    std::int32_t numParams() const noexcept;
    std::int32_t numInputs() const noexcept;
    std::int32_t numOutputs() const noexcept;
    void* nativeModuleHandle() const noexcept { return module_; }

    void close() noexcept;

private:
    bool loadAndOpen(const std::filesystem::path& path, std::string& error);
    void* module_{nullptr};
    vst2abi::AEffect* effect_{nullptr};
    bool effOpenCalled_{false};
    bool mainsOn_{false};
    bool supportsStartStopProcess_{false};
    bool processStarted_{false};
    double sampleRate_{48000.0};
    std::int32_t blockSize_{512};
    bool wantsMidi_{false};
    std::string pluginDirectoryAnsi_;
    std::atomic<bool> editorOpen_{false};
    std::atomic<bool> legacyIdleRequested_{false};
    std::atomic_flag legacyIdleActive_ = ATOMIC_FLAG_INIT;
    std::atomic_flag editorIdleActive_ = ATOMIC_FLAG_INIT;
    vst2abi::VstTimeInfo timeInfo_{};
    vst2abi::VstTimeInfo timeInfoView_{};
    std::int32_t hostProcessLevel_{0};
    std::int32_t hostAutomationState_{0};
    HostCallbackSink hostCallbackSink_{nullptr};
    void* hostCallbackContext_{nullptr};
    HostWindowResizeSink hostWindowResizeSink_{nullptr};
    void* hostWindowResizeContext_{nullptr};
    HostMidiOutputSink hostMidiOutputSink_{nullptr};
    void* hostMidiOutputContext_{nullptr};
};

} // namespace pluginscaler::formats
