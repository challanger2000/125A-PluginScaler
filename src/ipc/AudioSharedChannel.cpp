#include "pluginscaler/ipc/AudioSharedChannel.h"

#include <windows.h>

#include <cstring>

namespace pluginscaler::ipc {

namespace {

HANDLE asHandle(void* p) noexcept {
    return static_cast<HANDLE>(p);
}

bool waitEvent(void* eventHandle, std::chrono::milliseconds timeout) noexcept {
    if (!eventHandle) return false;
    const auto ms = timeout.count() < 0
        ? INFINITE
        : static_cast<DWORD>(timeout.count());
    return WaitForSingleObject(asHandle(eventHandle), ms) == WAIT_OBJECT_0;
}

} // namespace

AudioSharedChannel::~AudioSharedChannel() {
    close();
}

bool AudioSharedChannel::create(const std::wstring& mappingName,
                                const std::wstring& inputReadyEvent,
                                const std::wstring& outputReadyEvent) {
    close();

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, static_cast<DWORD>(sizeof(AudioSharedBlock)),
                                        mappingName.c_str());
    if (!mapping) return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioSharedBlock));
    if (!view) {
        CloseHandle(mapping);
        return false;
    }

    HANDLE inputEvent = CreateEventW(nullptr, FALSE, FALSE, inputReadyEvent.c_str());
    HANDLE outputEvent = CreateEventW(nullptr, FALSE, FALSE, outputReadyEvent.c_str());
    if (!inputEvent || !outputEvent) {
        if (inputEvent) CloseHandle(inputEvent);
        if (outputEvent) CloseHandle(outputEvent);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    inputEvent_ = inputEvent;
    outputEvent_ = outputEvent;
    block_ = static_cast<AudioSharedBlock*>(view);

    std::memset(block_, 0, sizeof(AudioSharedBlock));
    block_->header.magic = kAudioSharedMagic;
    block_->header.version = kAudioSharedVersion;
    block_->header.state.store(static_cast<std::uint32_t>(AudioBlockState::Idle),
                               std::memory_order_release);
    return true;
}

bool AudioSharedChannel::open(const std::wstring& mappingName,
                              const std::wstring& inputReadyEvent,
                              const std::wstring& outputReadyEvent) {
    close();

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    if (!mapping) return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioSharedBlock));
    if (!view) {
        CloseHandle(mapping);
        return false;
    }

    HANDLE inputEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, inputReadyEvent.c_str());
    HANDLE outputEvent = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, outputReadyEvent.c_str());
    if (!inputEvent || !outputEvent) {
        if (inputEvent) CloseHandle(inputEvent);
        if (outputEvent) CloseHandle(outputEvent);
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return false;
    }

    mapping_ = mapping;
    inputEvent_ = inputEvent;
    outputEvent_ = outputEvent;
    block_ = static_cast<AudioSharedBlock*>(view);

    if (block_->header.magic != kAudioSharedMagic ||
        block_->header.version != kAudioSharedVersion) {
        close();
        return false;
    }

    return true;
}

void AudioSharedChannel::close() noexcept {
    if (block_) UnmapViewOfFile(block_);
    block_ = nullptr;

    if (inputEvent_) CloseHandle(asHandle(inputEvent_));
    inputEvent_ = nullptr;

    if (outputEvent_) CloseHandle(asHandle(outputEvent_));
    outputEvent_ = nullptr;

    if (mapping_) CloseHandle(asHandle(mapping_));
    mapping_ = nullptr;
}

bool AudioSharedChannel::signalInput() noexcept {
    return inputEvent_ && SetEvent(asHandle(inputEvent_)) != FALSE;
}

bool AudioSharedChannel::signalOutput() noexcept {
    return outputEvent_ && SetEvent(asHandle(outputEvent_)) != FALSE;
}

bool AudioSharedChannel::waitForInput(std::chrono::milliseconds timeout) noexcept {
    return waitEvent(inputEvent_, timeout);
}

bool AudioSharedChannel::waitForOutput(std::chrono::milliseconds timeout) noexcept {
    return waitEvent(outputEvent_, timeout);
}

} // namespace pluginscaler::ipc
