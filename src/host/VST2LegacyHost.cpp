#include "pluginscaler/host/VST2LegacyHost.h"

#include <algorithm>

namespace pluginscaler::host {
namespace {

constexpr UINT kCallMessage = WM_APP + 0x125;
constexpr UINT_PTR kEditorIdleTimer = 0x125A;
constexpr UINT kEditorIdleMs = 20;
constexpr wchar_t kOwnerClass[] = L"125A_VST2LegacyHost_Owner";
constexpr wchar_t kEditorHostClass[] = L"125A_VST2LegacyHost_Editor";

bool registerWindowClasses() noexcept {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSW owner{};
    owner.lpfnWndProc = &VST2LegacyHost::hostWindowProc;
    owner.hInstance = instance;
    owner.lpszClassName = kOwnerClass;
    if (!RegisterClassW(&owner) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSW editor{};
    editor.lpfnWndProc = DefWindowProcW;
    editor.hInstance = instance;
    editor.lpszClassName = kEditorHostClass;
    if (!RegisterClassW(&editor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    return true;
}

} // namespace

VST2LegacyHost::VST2LegacyHost() {
    module_.setHostWindowResizeSink(&VST2LegacyHost::hostResizeThunk, this);
}

VST2LegacyHost::~VST2LegacyHost() {
    stop();
}

bool VST2LegacyHost::start(std::string& error) {
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    if (started_.load(std::memory_order_acquire))
        return true;

    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!readyEvent_) {
        error = "CreateEventW failed";
        return false;
    }

    ownerThread_ = std::thread([this] { threadMain(); });

    const DWORD wait = WaitForSingleObject(readyEvent_, 5000);
    CloseHandle(readyEvent_);
    readyEvent_ = nullptr;

    if (wait != WAIT_OBJECT_0 || !ownerWindow_) {
        error = "VST2 host owner thread did not initialize";
        if (ownerThread_.joinable())
            ownerThread_.join();
        return false;
    }

    started_.store(true, std::memory_order_release);
    return true;
}

void VST2LegacyHost::stop() noexcept {
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    if (!ownerThread_.joinable()) {
        started_.store(false, std::memory_order_release);
        return;
    }

    processing_.store(false, std::memory_order_release);

    if (ownerWindow_ && IsWindow(ownerWindow_)) {
        (void)invokeMainThread([this] {
            KillTimer(ownerWindow_, kEditorIdleTimer);
            if (editorOpen_.load(std::memory_order_acquire)) {
                (void)module_.closeEditor();
                editorOpen_.store(false, std::memory_order_release);
            }
            if (editorHost_ && IsWindow(editorHost_)) {
                DestroyWindow(editorHost_);
                editorHost_ = nullptr;
            }
            module_.close();
            pluginOpen_.store(false, std::memory_order_release);
        });

        PostMessageW(ownerWindow_, WM_CLOSE, 0, 0);
    }

    ownerThread_.join();
    ownerWindow_ = nullptr;
    started_.store(false, std::memory_order_release);
}

bool VST2LegacyHost::openPlugin(const std::filesystem::path& path,
                                double sampleRate,
                                std::int32_t blockSize,
                                std::string& error) {
    if (!started_.load(std::memory_order_acquire)) {
        error = "VST2 host is not started";
        return false;
    }
    if (path.empty() || sampleRate <= 0.0 || blockSize <= 0) {
        error = "invalid VST2 host open arguments";
        return false;
    }

    bool ok = false;
    std::string localError;
    if (!invokeMainThread([&] {
            processing_.store(false, std::memory_order_release);
            KillTimer(ownerWindow_, kEditorIdleTimer);

            if (editorOpen_.load(std::memory_order_acquire)) {
                (void)module_.closeEditor();
                editorOpen_.store(false, std::memory_order_release);
            }
            if (editorHost_ && IsWindow(editorHost_)) {
                DestroyWindow(editorHost_);
                editorHost_ = nullptr;
            }

            module_.close();
            pluginOpen_.store(false, std::memory_order_release);

            ok = module_.openForProcessing(path, sampleRate, blockSize, localError);
            pluginOpen_.store(ok, std::memory_order_release);
            processing_.store(false, std::memory_order_release);
            if (ok)
                SetTimer(ownerWindow_, kEditorIdleTimer, kEditorIdleMs, nullptr);
        })) {
        error = "failed to marshal plugin open to VST2 owner thread";
        return false;
    }

    if (!ok)
        error = std::move(localError);
    return ok;
}

bool VST2LegacyHost::closePlugin() noexcept {
    if (!started_.load(std::memory_order_acquire))
        return true;

    processing_.store(false, std::memory_order_release);
    bool ok = true;
    if (!invokeMainThread([&] {
            KillTimer(ownerWindow_, kEditorIdleTimer);
            if (editorOpen_.load(std::memory_order_acquire)) {
                ok = module_.closeEditor() && ok;
                editorOpen_.store(false, std::memory_order_release);
            }
            if (editorHost_ && IsWindow(editorHost_)) {
                DestroyWindow(editorHost_);
                editorHost_ = nullptr;
            }
            module_.close();
            pluginOpen_.store(false, std::memory_order_release);
        }))
        return false;
    return ok;
}

bool VST2LegacyHost::reconfigure(double sampleRate,
                                 std::int32_t blockSize) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire) ||
        sampleRate <= 0.0 || blockSize <= 0)
        return false;

    processing_.store(false, std::memory_order_release);
    bool ok = false;
    if (!invokeMainThread([&] {
            ok = module_.reconfigureProcessing(sampleRate, blockSize);
        }))
        return false;
    processing_.store(ok, std::memory_order_release);
    return ok;
}

bool VST2LegacyHost::setMains(bool active) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return false;

    if (!active)
        processing_.store(false, std::memory_order_release);

    bool ok = false;
    if (!invokeMainThread([&] { ok = module_.setMains(active); }))
        return false;

    processing_.store(active && ok, std::memory_order_release);
    return ok;
}

bool VST2LegacyHost::openEditor(std::string& error) {
    if (!pluginOpen_.load(std::memory_order_acquire)) {
        error = "plugin is not open";
        return false;
    }

    bool ok = false;
    if (!invokeMainThread([&] {
            if (editorOpen_.load(std::memory_order_acquire)) {
                ok = true;
                return;
            }

            formats::vst2abi::VstRect rect{};
            if (!module_.editorRect(rect)) {
                error = "effEditGetRect failed";
                return;
            }

            const int width = rect.right - rect.left;
            const int height = rect.bottom - rect.top;
            if (width <= 0 || height <= 0) {
                error = "invalid VST2 editor size";
                return;
            }

            RECT frame{0, 0, width, height};
            if (!AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
                error = "AdjustWindowRectEx failed";
                return;
            }

            HWND host = CreateWindowExW(
                0, kEditorHostClass, L"125A Legacy VST2 Host",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                CW_USEDEFAULT, CW_USEDEFAULT,
                frame.right - frame.left,
                frame.bottom - frame.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!host) {
                error = "failed to create same-process VST2 editor host window";
                return;
            }

            editorHost_ = host;

            // Critical legacy invariant:
            // effEditOpen receives the final same-process host HWND. The plugin's
            // own editor HWND is never reparented afterwards.
            if (!module_.openEditor(host)) {
                DestroyWindow(host);
                editorHost_ = nullptr;
                error = "effEditOpen failed";
                return;
            }

            // Historical Steinberg examples query the rectangle again after open.
            // Some old editors change size during effEditOpen.
            formats::vst2abi::VstRect after{};
            if (module_.editorRect(after)) {
                const int afterWidth = after.right - after.left;
                const int afterHeight = after.bottom - after.top;
                if (afterWidth > 0 && afterHeight > 0 &&
                    (afterWidth != width || afterHeight != height)) {
                    RECT adjusted{0, 0, afterWidth, afterHeight};
                    if (AdjustWindowRectEx(&adjusted, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
                        SetWindowPos(host, nullptr, 0, 0,
                                     adjusted.right - adjusted.left,
                                     adjusted.bottom - adjusted.top,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                }
            }

            ShowWindow(host, SW_SHOWNA);
            UpdateWindow(host);

            // effEditIdle shares the existing owner-thread timer with the
            // deprecated effIdle service.
            editorOpen_.store(true, std::memory_order_release);
            ok = true;
        }))
        return false;

    return ok;
}

bool VST2LegacyHost::closeEditor() noexcept {
    if (!started_.load(std::memory_order_acquire))
        return true;

    bool ok = true;
    if (!invokeMainThread([&] {
            if (editorOpen_.load(std::memory_order_acquire))
                ok = module_.closeEditor();
            editorOpen_.store(false, std::memory_order_release);

            if (editorHost_ && IsWindow(editorHost_)) {
                DestroyWindow(editorHost_);
                editorHost_ = nullptr;
            }
        }))
        return false;
    return ok;
}

HWND VST2LegacyHost::editorHostWindow() const noexcept {
    return editorHost_;
}

bool VST2LegacyHost::hostResizeThunk(void* context,
                                     std::int32_t width,
                                     std::int32_t height) noexcept {
    auto* self = static_cast<VST2LegacyHost*>(context);
    return self ? self->resizeEditorHost(width, height) : false;
}

bool VST2LegacyHost::resizeEditorHost(std::int32_t width,
                                      std::int32_t height) noexcept {
    if (width <= 0 || height <= 0 || !ownerWindow_ || !editorHost_ ||
        !IsWindow(ownerWindow_) || !IsWindow(editorHost_))
        return false;

    DWORD ownerThreadId = GetWindowThreadProcessId(ownerWindow_, nullptr);
    if (ownerThreadId == 0 || GetCurrentThreadId() != ownerThreadId)
        return false;

    RECT frame{0, 0, width, height};
    if (!AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0))
        return false;

    return SetWindowPos(editorHost_, nullptr, 0, 0,
                        frame.right - frame.left,
                        frame.bottom - frame.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

bool VST2LegacyHost::processReplacing(float** inputs,
                                      float** outputs,
                                      std::int32_t frames) noexcept {
    if (!processing_.load(std::memory_order_acquire))
        return false;

    // No GUI/control message round-trip and no host mutex on the audio path.
    return module_.processReplacing(inputs, outputs, frames);
}

bool VST2LegacyHost::processMidiEvents(
    const formats::vst2abi::VstMidiEvent* events,
    std::int32_t eventCount) noexcept {
    if (!processing_.load(std::memory_order_acquire))
        return false;
    return module_.processMidiEvents(events, eventCount);
}

bool VST2LegacyHost::setParameter(std::int32_t index, float value) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return false;
    return module_.setParameter(index, value);
}

float VST2LegacyHost::getParameter(std::int32_t index) const noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return 0.0f;
    return module_.getParameter(index);
}

formats::vst2abi::VstIntPtr VST2LegacyHost::dispatchOnMainThread(
    std::int32_t opcode,
    std::int32_t index,
    formats::vst2abi::VstIntPtr value,
    void* ptr,
    float opt) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return 0;

    formats::vst2abi::VstIntPtr result = 0;
    if (!invokeMainThread([&] {
            result = module_.dispatch(opcode, index, value, ptr, opt);
        }))
        return 0;
    return result;
}

bool VST2LegacyHost::getChunkOnMainThread(
    std::int32_t index,
    std::vector<std::uint8_t>& data) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return false;

    bool ok = false;
    if (!invokeMainThread([&] { ok = module_.getChunk(index, data); }))
        return false;
    return ok;
}

bool VST2LegacyHost::setChunkOnMainThread(
    std::int32_t index,
    const void* data,
    std::size_t bytes) noexcept {
    if (!pluginOpen_.load(std::memory_order_acquire))
        return false;

    bool ok = false;
    if (!invokeMainThread([&] { ok = module_.setChunk(index, data, bytes); }))
        return false;
    return ok;
}

void VST2LegacyHost::setHostTimeInfo(
    const formats::vst2abi::VstTimeInfo& info) noexcept {
    module_.setHostTimeInfo(info);
}

void VST2LegacyHost::setHostCallbackSink(
    HostCallbackSink sink, void* context) noexcept {
    module_.setHostCallbackSink(sink, context);
}

bool VST2LegacyHost::invokeMainThread(
    const std::function<void()>& fn) const noexcept {
    HWND hwnd = ownerWindow_;
    if (!hwnd || !IsWindow(hwnd))
        return false;

    if (GetCurrentThreadId() == GetWindowThreadProcessId(hwnd, nullptr)) {
        fn();
        return true;
    }

    MainThreadCall call{fn};
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(
        hwnd, kCallMessage, 0, reinterpret_cast<LPARAM>(&call),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
    return sent != 0 && result == 1;
}

void VST2LegacyHost::threadMain() noexcept {
    if (!registerWindowClasses()) {
        if (readyEvent_)
            SetEvent(readyEvent_);
        return;
    }

    HWND owner = CreateWindowExW(
        0, kOwnerClass, L"",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
    ownerWindow_ = owner;

    if (readyEvent_)
        SetEvent(readyEvent_);
    if (!owner)
        return;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ownerWindow_ = nullptr;
}

LRESULT CALLBACK VST2LegacyHost::hostWindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    auto* self = reinterpret_cast<VST2LegacyHost*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<VST2LegacyHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    }

    if (message == kCallMessage) {
        auto* call = reinterpret_cast<MainThreadCall*>(lParam);
        if (!call)
            return 0;
        call->fn();
        return 1;
    }

    if (message == WM_TIMER && wParam == kEditorIdleTimer) {
        if (self) {
            (void)self->module_.serviceLegacyIdle();
            if (self->editorOpen_.load(std::memory_order_acquire))
                (void)self->module_.editorIdle();
        }
        return 0;
    }

    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }

    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace pluginscaler::host
