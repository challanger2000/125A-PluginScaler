#pragma once

#include <atomic>
#include <cstdint>

namespace pluginscaler::ipc {

inline constexpr std::uint32_t kAudioSharedMagic = 0x41505341u; // "ASPA"
inline constexpr std::uint32_t kAudioSharedVersion = 2;
inline constexpr std::uint32_t kMaxAudioChannels = 8;
inline constexpr std::uint32_t kMaxAudioFrames = 2048;

enum class AudioBlockState : std::uint32_t {
    Idle = 0,
    InputReady = 1,
    Processing = 2,
    OutputReady = 3,
    Shutdown = 4,
    Error = 5
};

struct alignas(64) AudioSharedHeader {
    std::uint32_t magic{kAudioSharedMagic};
    std::uint32_t version{kAudioSharedVersion};
    std::uint32_t inputChannels{0};
    std::uint32_t outputChannels{0};
    std::uint32_t frames{0};
    std::uint32_t sampleRateHz{0};
    std::uint64_t sequence{0};
    std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(AudioBlockState::Idle)};
    std::uint32_t errorCode{0};
    std::uint8_t reserved[24]{};
};

struct AudioSharedBlock {
    AudioSharedHeader header{};
    float inputs[kMaxAudioChannels][kMaxAudioFrames]{};
    float outputs[kMaxAudioChannels][kMaxAudioFrames]{};
};

static_assert(alignof(AudioSharedHeader) == 64);

} // namespace pluginscaler::ipc
