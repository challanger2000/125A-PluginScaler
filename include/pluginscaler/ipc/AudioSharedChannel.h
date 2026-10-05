#pragma once

#include "pluginscaler/ipc/AudioSharedMemory.h"

#include <chrono>
#include <filesystem>
#include <string>

namespace pluginscaler::ipc {

class AudioSharedChannel {
public:
    AudioSharedChannel() = default;
    ~AudioSharedChannel();

    AudioSharedChannel(const AudioSharedChannel&) = delete;
    AudioSharedChannel& operator=(const AudioSharedChannel&) = delete;

    bool create(const std::wstring& mappingName, const std::wstring& inputReadyEvent,
                const std::wstring& outputReadyEvent);
    bool open(const std::wstring& mappingName, const std::wstring& inputReadyEvent,
              const std::wstring& outputReadyEvent);
    void close() noexcept;

    AudioSharedBlock* block() noexcept { return block_; }
    const AudioSharedBlock* block() const noexcept { return block_; }

    bool signalInput() noexcept;
    bool signalOutput() noexcept;
    bool waitForInput(std::chrono::milliseconds timeout) noexcept;
    bool waitForOutput(std::chrono::milliseconds timeout) noexcept;

    // Non-owning native synchronization handle for callers that must wait on
    // the audio output event together with another process/event handle.
    void* outputEventHandle() const noexcept { return outputEvent_; }

private:
    void* mapping_{nullptr};
    void* inputEvent_{nullptr};
    void* outputEvent_{nullptr};
    AudioSharedBlock* block_{nullptr};
};

} // namespace pluginscaler::ipc
