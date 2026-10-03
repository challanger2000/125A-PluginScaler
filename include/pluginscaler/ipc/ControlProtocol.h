#pragma once

#include <cstdint>

namespace pluginscaler::ipc {

inline constexpr std::uint32_t kControlMagic = 0x43535041u; // "ASPC"
inline constexpr std::uint16_t kControlVersion = 1;
inline constexpr std::uint32_t kMaxControlPayload = 4u * 1024u * 1024u;

enum class ControlCommand : std::uint16_t {
    GetState = 1,
    SetState = 2,
    GetParameters = 3,
    Shutdown = 4
};

enum class ControlStatus : std::uint32_t {
    Ok = 0,
    InvalidRequest = 1,
    NotReady = 2,
    PluginError = 3,
    PayloadTooLarge = 4
};

#pragma pack(push, 1)
struct ControlMessageHeader {
    std::uint32_t magic{kControlMagic};
    std::uint16_t version{kControlVersion};
    ControlCommand command{ControlCommand::GetState};
    std::int32_t arg0{0};
    std::uint32_t payloadBytes{0};
    ControlStatus status{ControlStatus::Ok};
    std::uint32_t responseBytes{0};
};
#pragma pack(pop)

static_assert(sizeof(ControlMessageHeader) == 24);

} // namespace pluginscaler::ipc
