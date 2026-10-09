#pragma once

#include <cstdint>

namespace pluginscaler::ipc {

inline constexpr std::uint32_t kVst2CallbackMagic = 0x42433241u; // "A2CB"
inline constexpr std::uint16_t kVst2CallbackVersion = 1;

#pragma pack(push, 1)
struct VST2CallbackEvent {
    std::uint32_t magic{kVst2CallbackMagic};
    std::uint16_t version{kVst2CallbackVersion};
    std::uint16_t reserved{0};
    std::int32_t opcode{0};
    std::int32_t index{0};
    std::int64_t value{0};
    float opt{0.0f};
};
#pragma pack(pop)

static_assert(sizeof(VST2CallbackEvent) == 28);

} // namespace pluginscaler::ipc
