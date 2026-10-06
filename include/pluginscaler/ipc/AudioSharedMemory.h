#pragma once

#include <atomic>
#include <cstdint>

namespace pluginscaler::ipc {

inline constexpr std::uint32_t kAudioSharedMagic = 0x41505341u; // "ASPA"
inline constexpr std::uint32_t kAudioSharedVersion = 6;
inline constexpr std::uint32_t kMaxParameters = 4096;
inline constexpr std::uint32_t kMaxMidiEvents = 256;
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
    std::uint32_t midiEventCount{0};
    std::uint32_t outputMidiEventCount{0};
    std::uint32_t parameterCount{0};
    std::uint32_t parameterGeneration{0};
    std::uint8_t reserved[8]{};
};

struct HostTimeShared {
    double samplePos{0.0};
    double sampleRate{0.0};
    double nanoSeconds{0.0};
    double ppqPos{0.0};
    double tempo{0.0};
    double barStartPos{0.0};
    double cycleStartPos{0.0};
    double cycleEndPos{0.0};
    std::int32_t timeSigNumerator{0};
    std::int32_t timeSigDenominator{0};
    std::int32_t smpteOffset{0};
    std::int32_t smpteFrameRate{0};
    std::int32_t samplesToNextClock{0};
    std::int32_t flags{0};
};

struct MidiSharedEvent {
    std::int32_t deltaFrames{0};
    std::int32_t flags{0};
    std::uint8_t data[4]{};
    std::uint8_t reserved[4]{};
};

struct AudioSharedBlock {
    AudioSharedHeader header{};
    HostTimeShared hostTime{};
    MidiSharedEvent midiEvents[kMaxMidiEvents]{};
    MidiSharedEvent outputMidiEvents[kMaxMidiEvents]{};
    float parameterValues[kMaxParameters]{};
    float inputs[kMaxAudioChannels][kMaxAudioFrames]{};
    float outputs[kMaxAudioChannels][kMaxAudioFrames]{};
};

static_assert(alignof(AudioSharedHeader) == 64);
static_assert(sizeof(AudioSharedHeader) == 64);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

} // namespace pluginscaler::ipc
