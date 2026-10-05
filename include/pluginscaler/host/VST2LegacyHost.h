#pragma once

#include "pluginscaler/formats/VST2PluginModule.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pluginscaler::host {

class VST2LegacyHost {
public:
    using HostCallbackSink = formats::VST2PluginModule::HostCallbackSink;

    VST2LegacyHost();
    ~VST2LegacyHost();

    VST2LegacyHost(const VST2LegacyHost&) = delete;
    VST2LegacyHost& operator=(const VST2LegacyHost&) = delete;

    bool start(std::string& error);
    void stop() noexcept;

    bool openPlugin(const std::filesystem::path& path,
                    double sampleRate,
                    std::int32_t blockSize,
                    std::string& error);
    bool closePlugin() noexcept;

    bool reconfigure(double sampleRate, std::int32_t blockSize) noexcept;
    bool setMains(bool active) noexcept;

    bool openEditor(std::string& error);
    bool closeEditor() noexcept;
    HWND editorHostWindow() const noexcept;

    bool processReplacing(float** inputs,
                          float** outputs,
                          std::int32_t frames) noexcept;
    bool processMidiEvents(const formats::vst2abi::VstMidiEvent* events,
                           std::int32_t eventCount) noexcept;

    bool setParameter(std::int32_t index, float value) noexcept;
    float getParameter(std::int32_t index) const noexcept;

    formats::vst2abi::VstIntPtr dispatchOnMainThread(
        std::int32_t opcode,
        std::int32_t index = 0,
        formats::vst2abi::VstIntPtr value = 0,
        void* ptr = nullptr,
        float opt = 0.0f) noexcept;

    bool getChunkOnMainThread(std::int32_t index,
                              std::vector<std::uint8_t>& data) noexcept;
    bool setChunkOnMainThread(std::int32_t index,
                              const void* data,
                              std::size_t bytes) noexcept;

    void setHostTimeInfo(const formats::vst2abi::VstTimeInfo& info) noexcept;
    void setHostCallbackSink(HostCallbackSink sink, void* context) noexcept;

    bool isStarted() const noexcept { return started_.load(std::memory_order_acquire); }
    bool isPluginOpen() const noexcept { return pluginOpen_.load(std::memory_order_acquire); }
    bool isProcessing() const noexcept { return processing_.load(std::memory_order_acquire); }
    bool isEditorOpen() const noexcept { return editorOpen_.load(std::memory_order_acquire); }

    static LRESULT CALLBACK hostWindowProc(HWND hwnd, UINT message,
                                           WPARAM wParam, LPARAM lParam) noexcept;

private:
    struct MainThreadCall {
        std::function<void()> fn;
    };

    bool invokeMainThread(const std::function<void()>& fn) const noexcept;
    void threadMain() noexcept;

    mutable std::mutex lifecycleMutex_;
    formats::VST2PluginModule module_;

    std::thread ownerThread_;
    HWND ownerWindow_{nullptr};
    HWND editorHost_{nullptr};

    HANDLE readyEvent_{nullptr};
    std::atomic<bool> started_{false};
    std::atomic<bool> pluginOpen_{false};
    std::atomic<bool> processing_{false};
    std::atomic<bool> editorOpen_{false};
};

} // namespace pluginscaler::host
