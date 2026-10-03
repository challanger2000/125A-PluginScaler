#pragma once

#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <filesystem>
#include <string>

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
    std::string error;
};

class VST2PluginModule {
public:
    VST2PluginModule() = default;
    ~VST2PluginModule();

    VST2PluginModule(const VST2PluginModule&) = delete;
    VST2PluginModule& operator=(const VST2PluginModule&) = delete;

    VST2ProbeResult probe(const std::filesystem::path& path);
    void close() noexcept;

private:
    void* module_{nullptr};
    vst2abi::AEffect* effect_{nullptr};
    bool effOpenCalled_{false};
};

} // namespace pluginscaler::formats
