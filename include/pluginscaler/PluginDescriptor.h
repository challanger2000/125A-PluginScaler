#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace pluginscaler {

enum class PluginFormat : std::uint8_t {
    Unknown,
    VST2,
    VST3
};

enum class PluginBitness : std::uint8_t {
    Unknown,
    X86,
    X64
};

struct PluginDescriptor {
    std::filesystem::path path;
    std::string stableId;
    PluginFormat format{PluginFormat::Unknown};
    PluginBitness bitness{PluginBitness::Unknown};
};

} // namespace pluginscaler
