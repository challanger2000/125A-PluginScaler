#pragma once

#include <cstdint>

namespace pluginscaler::ipc {

inline constexpr std::uint32_t kProtocolMagic = 0x31535041u; // "APS1"
inline constexpr std::uint16_t kProtocolMajor = 1;
inline constexpr std::uint16_t kProtocolMinor = 0;

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck,
    LoadPlugin,
    LoadResult,
    StatePush,
    StateRequest,
    StateReply,
    ParameterChange,
    MidiEvent,
    GuiOpen,
    GuiClose,
    GuiResize,
    Heartbeat,
    Shutdown
};

struct MessageHeader {
    std::uint32_t magic{kProtocolMagic};
    std::uint16_t major{kProtocolMajor};
    std::uint16_t minor{kProtocolMinor};
    MessageType type{MessageType::Hello};
    std::uint16_t reserved{0};
    std::uint32_t payloadBytes{0};
    std::uint64_t sequence{0};
};

static_assert(sizeof(MessageHeader) == 24);

} // namespace pluginscaler::ipc
