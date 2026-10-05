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
    Shutdown = 4,
    GetEditorRect = 5,
    OpenEditor = 6,
    CloseEditor = 7,
    CaptureEditor = 8,
    SendEditorMouse = 9,
    SetMains = 10,
    PullHostCallbacks = 11
};

#pragma pack(push, 1)
struct EditorRectPayload {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};
};

struct EditorOpenResult {
    std::uint64_t surrogateWindow{0};
    std::uint64_t editorWindow{0};
};

struct EditorBitmapHeader {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t strideBytes{0};
    std::uint32_t format{1}; // 1 = BGRA8, top-down
};

struct EditorMousePayload {
    std::uint32_t message{0};
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t keyFlags{0};
};

struct HostCallbackPayload {
    std::int32_t opcode{0};
    std::int32_t index{0};
    std::int64_t value{0};
    float opt{0.0f};
};
#pragma pack(pop)

static_assert(sizeof(EditorRectPayload) == 16);
static_assert(sizeof(EditorOpenResult) == 16);
static_assert(sizeof(EditorBitmapHeader) == 16);
static_assert(sizeof(EditorMousePayload) == 16);
static_assert(sizeof(HostCallbackPayload) == 20);

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
