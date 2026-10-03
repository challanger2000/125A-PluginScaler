#pragma once

#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

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
    bool processReplacing(float** inputs, float** outputs, std::int32_t frames) noexcept;
    bool processMidiEvents(const vst2abi::VstMidiEvent* events,
                           std::int32_t eventCount) noexcept;
    bool setParameter(std::int32_t index, float value) noexcept;
    float getParameter(std::int32_t index) const noexcept;
    bool getChunk(std::int32_t index, std::vector<std::uint8_t>& data) noexcept;
    bool setChunk(std::int32_t index, const void* data, std::size_t bytes) noexcept;
    std::int32_t numParams() const noexcept;
    std::int32_t numInputs() const noexcept;
    std::int32_t numOutputs() const noexcept;

    void close() noexcept;

private:
    bool loadAndOpen(const std::filesystem::path& path, std::string& error);
    void* module_{nullptr};
    vst2abi::AEffect* effect_{nullptr};
    bool effOpenCalled_{false};
    bool mainsOn_{false};
};

} // namespace pluginscaler::formats
