#pragma once

#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <cstddef>
#include <atomic>

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
    const vst2abi::VstTimeInfo* hostTimeInfo() const noexcept { return &timeInfo_; }
    double sampleRate() const noexcept { return sampleRate_; }
    std::int32_t blockSize() const noexcept { return blockSize_; }
    void noteWantMidiRequest() noexcept { wantsMidi_ = true; }
    bool wantsMidi() const noexcept { return wantsMidi_; }
    void noteAutomation(std::int32_t index, float value) noexcept;
    bool readAutomation(std::uint32_t& generation,
                        std::int32_t& index,
                        float& value) const noexcept;
    bool processReplacing(float** inputs, float** outputs, std::int32_t frames) noexcept;
    bool processMidiEvents(const vst2abi::VstMidiEvent* events,
                           std::int32_t eventCount) noexcept;
    bool setParameter(std::int32_t index, float value) noexcept;
    float getParameter(std::int32_t index) const noexcept;
    bool getChunk(std::int32_t index, std::vector<std::uint8_t>& data) noexcept;
    bool setChunk(std::int32_t index, const void* data, std::size_t bytes) noexcept;
    bool editorRect(vst2abi::VstRect& rect) noexcept;
    bool openEditor(void* parentWindow) noexcept;
    bool closeEditor() noexcept;
    bool editorIdle() noexcept;
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
    double sampleRate_{48000.0};
    std::int32_t blockSize_{512};
    bool wantsMidi_{false};
    vst2abi::VstTimeInfo timeInfo_{};
    std::atomic<std::uint32_t> automationGeneration_{0};
    std::atomic<std::int32_t> automationIndex_{-1};
    std::atomic<std::uint32_t> automationValueBits_{0};
};

} // namespace pluginscaler::formats
