#define NOMINMAX
#include "pluginscaler/formats/VST2PluginModule.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/Protocol.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <cstdint>

namespace {

int runVst2Probe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);

    std::cout
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "entry=" << result.entryPoint << '\n'
        << "effect=" << result.effectName << '\n'
        << "vendor=" << result.vendor << '\n'
        << "product=" << result.product << '\n'
        << "uniqueId=" << result.uniqueId << '\n'
        << "version=" << result.version << '\n'
        << "programs=" << result.numPrograms << '\n'
        << "params=" << result.numParams << '\n'
        << "inputs=" << result.numInputs << '\n'
        << "outputs=" << result.numOutputs << '\n'
        << "flags=" << result.flags << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.closed) ? 0 : 2;
}

int writeVst2Manifest(const std::filesystem::path& path,
                      const std::filesystem::path& manifestPath) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);
    if (!(result.loaded && result.opened && result.closed))
        return 20;

    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) return 21;

    out << "format=125A-PluginScaler-VST2-Manifest-1\n"
        << "effect=" << result.effectName << "\n"
        << "vendor=" << result.vendor << "\n"
        << "product=" << result.product << "\n"
        << "uniqueId=" << result.uniqueId << "\n"
        << "version=" << result.version << "\n"
        << "programs=" << result.numPrograms << "\n"
        << "params=" << result.numParams << "\n"
        << "inputs=" << result.numInputs << "\n"
        << "outputs=" << result.numOutputs << "\n"
        << "flags=" << result.flags << "\n";
    for (std::size_t i = 0; i < result.parameterDefaults.size(); ++i)
        out << "param." << i << "=" << result.parameterDefaults[i] << "\n";
    return out ? 0 : 22;
}

int runVst2AudioProbe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probeAudio(path, 48000.0, 64);

    std::cout << std::fixed << std::setprecision(4)
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "configured=" << (result.configured ? 1 : 0) << '\n'
        << "mainsOn=" << (result.mainsOn ? 1 : 0) << '\n'
        << "processed=" << (result.processed ? 1 : 0) << '\n'
        << "mainsOff=" << (result.mainsOff ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "outL=" << result.firstOutputLeft << '\n'
        << "outR=" << result.firstOutputRight << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.configured &&
            result.mainsOn && result.processed && result.mainsOff &&
            result.closed) ? 0 : 3;
}

struct EditorGuiContext {
    pluginscaler::formats::VST2PluginModule* module{nullptr};
    std::mutex* moduleMutex{nullptr};
    HWND surrogate{nullptr};
    HWND editor{nullptr};
};

inline constexpr UINT kEditorOpenMessage = WM_APP + 0x125;
inline constexpr UINT kEditorCloseMessage = WM_APP + 0x126;
inline constexpr UINT kEditorShutdownMessage = WM_APP + 0x127;
inline constexpr UINT kEditorCaptureMessage = WM_APP + 0x128;
inline constexpr UINT_PTR kEditorIdleTimer = 0x125A;

struct EditorCaptureRequest {
    std::vector<std::uint8_t>* bytes{nullptr};
};

struct EditorChildCandidate {
    HWND hwnd{nullptr};
    LONG area{0};
};

BOOL CALLBACK largestChildProc(HWND hwnd, LPARAM param) {
    auto* candidate = reinterpret_cast<EditorChildCandidate*>(param);
    if (!candidate || !IsWindow(hwnd))
        return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc))
        return TRUE;

    const LONG width = rc.right - rc.left;
    const LONG height = rc.bottom - rc.top;
    const LONG area = width > 0 && height > 0 ? width * height : 0;
    if (area > candidate->area) {
        candidate->hwnd = hwnd;
        candidate->area = area;
    }
    return TRUE;
}

BOOL CALLBACK collectChildProc(HWND hwnd, LPARAM param) {
    auto* windows = reinterpret_cast<std::vector<HWND>*>(param);
    if (windows && IsWindow(hwnd))
        windows->push_back(hwnd);
    return TRUE;
}

LRESULT CALLBACK editorSurrogateProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ctx = reinterpret_cast<EditorGuiContext*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ctx = static_cast<EditorGuiContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) ctx->surrogate = hwnd;
    }

    if (!ctx)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case kEditorOpenMessage: {
        std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
        if (ctx->editor && IsWindow(ctx->editor))
            return reinterpret_cast<LRESULT>(ctx->editor);

        if (!ctx->module->openEditor(hwnd))
            return 0;

        EditorChildCandidate candidate{};
        EnumChildWindows(hwnd, largestChildProc,
                         reinterpret_cast<LPARAM>(&candidate));
        HWND child = candidate.hwnd;
        if (!child || !IsWindow(child))
            return 0;

        RECT rc{};
        if (GetClientRect(child, &rc)) {
            const int width = static_cast<int>((std::max)(1L, rc.right - rc.left));
            const int height = static_cast<int>((std::max)(1L, rc.bottom - rc.top));
            SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, width, height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(child, SW_SHOWNA);
            RedrawWindow(hwnd, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            UpdateWindow(child);
        }

        ctx->editor = child;
        SetTimer(hwnd, kEditorIdleTimer, 30, nullptr);
        return reinterpret_cast<LRESULT>(child);
    }
    case kEditorCloseMessage: {
        KillTimer(hwnd, kEditorIdleTimer);
        std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
        const bool ok = ctx->module->closeEditor();
        ctx->editor = nullptr;
        ShowWindow(hwnd, SW_HIDE);
        return ok ? 1 : 0;
    }
    case WM_TIMER:
        if (wp == kEditorIdleTimer && ctx->editor && IsWindow(ctx->editor)) {
            std::lock_guard<std::mutex> lock(*ctx->moduleMutex);
            (void)ctx->module->editorIdle();
            UpdateWindow(ctx->editor);
            return 0;
        }
        break;
    case kEditorCaptureMessage: {
        auto* request = reinterpret_cast<EditorCaptureRequest*>(lp);
        if (!request || !request->bytes || !ctx->editor || !IsWindow(ctx->editor))
            return 0;

        std::vector<HWND> candidates;
        if (ctx->surrogate && IsWindow(ctx->surrogate))
            candidates.push_back(ctx->surrogate);
        candidates.push_back(ctx->editor);
        EnumChildWindows(ctx->surrogate, collectChildProc,
                         reinterpret_cast<LPARAM>(&candidates));

        std::vector<std::uint8_t> bestPixels;
        std::uint32_t bestWidth = 0;
        std::uint32_t bestHeight = 0;
        std::uint32_t bestStride = 0;
        std::uint64_t bestScore = 0;

        auto scorePixels = [](const std::uint8_t* p, std::size_t bytes) -> std::uint64_t {
            if (!p || bytes < 4) return 0;
            std::uint8_t minValue = 255;
            std::uint8_t maxValue = 0;
            std::uint64_t nonBlack = 0;
            std::uint64_t variation = 0;
            const std::size_t step = (std::max<std::size_t>)(4, bytes / 4096u);
            std::uint8_t prev = 0;
            bool havePrev = false;
            for (std::size_t i = 0; i + 2 < bytes; i += step) {
                const std::uint8_t b = p[i + 0];
                const std::uint8_t g = p[i + 1];
                const std::uint8_t rr = p[i + 2];
                minValue = (std::min)({minValue, b, g, rr});
                maxValue = (std::max)({maxValue, b, g, rr});
                if (rr > 6 || g > 6 || b > 6)
                    ++nonBlack;
                const std::uint8_t luma =
                    static_cast<std::uint8_t>((static_cast<unsigned>(rr) * 3u +
                                               static_cast<unsigned>(g) * 6u +
                                               static_cast<unsigned>(b)) / 10u);
                if (havePrev)
                    variation += static_cast<std::uint64_t>(
                        luma > prev ? luma - prev : prev - luma);
                prev = luma;
                havePrev = true;
            }
            if (maxValue <= static_cast<std::uint8_t>(minValue + 3))
                return 0;
            return nonBlack * 1024ull + variation;
        };

        auto tryCapture = [&](HWND target, int mode) {
            if (!target || !IsWindow(target))
                return;

            RECT rc{};
            if (!GetClientRect(target, &rc))
                return;
            const int width = rc.right - rc.left;
            const int height = rc.bottom - rc.top;
            if (width <= 0 || height <= 0)
                return;

            const std::size_t stride = static_cast<std::size_t>(width) * 4u;
            const std::size_t pixelBytes = stride * static_cast<std::size_t>(height);
            if (pixelBytes == 0 ||
                pixelBytes + sizeof(pluginscaler::ipc::EditorBitmapHeader) >
                    pluginscaler::ipc::kMaxControlPayload)
                return;

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width;
            bmi.bmiHeader.biHeight = -height;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            HDC screen = GetDC(nullptr);
            if (!screen) return;
            HDC mem = CreateCompatibleDC(screen);
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS,
                                              &bits, nullptr, 0);
            ReleaseDC(nullptr, screen);
            if (!mem || !bitmap || !bits) {
                if (bitmap) DeleteObject(bitmap);
                if (mem) DeleteDC(mem);
                return;
            }

            std::memset(bits, 0, pixelBytes);
            HGDIOBJ old = SelectObject(mem, bitmap);

            RedrawWindow(target, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            UpdateWindow(target);

            bool apiOk = false;
            if (mode == 0) {
                apiOk = PrintWindow(target, mem, PW_CLIENTONLY | 0x00000002) != FALSE;
            } else if (mode == 1) {
                SendMessageW(target, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                             PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
                apiOk = true;
            } else {
                HDC source = GetDC(target);
                if (source) {
                    apiOk = BitBlt(mem, 0, 0, width, height,
                                   source, 0, 0, SRCCOPY) != FALSE;
                    ReleaseDC(target, source);
                }
            }

            SelectObject(mem, old);

            if (apiOk) {
                const auto score = scorePixels(
                    static_cast<const std::uint8_t*>(bits), pixelBytes);
                if (score > bestScore) {
                    bestScore = score;
                    bestWidth = static_cast<std::uint32_t>(width);
                    bestHeight = static_cast<std::uint32_t>(height);
                    bestStride = static_cast<std::uint32_t>(stride);
                    bestPixels.resize(pixelBytes);
                    std::memcpy(bestPixels.data(), bits, pixelBytes);
                }
            }

            DeleteObject(bitmap);
            DeleteDC(mem);
        };

        for (HWND candidate : candidates) {
            tryCapture(candidate, 0);
            tryCapture(candidate, 1);
            tryCapture(candidate, 2);
        }

        if (bestScore == 0 || bestPixels.empty())
            return 0;

        pluginscaler::ipc::EditorBitmapHeader header{};
        header.width = bestWidth;
        header.height = bestHeight;
        header.strideBytes = bestStride;

        request->bytes->resize(sizeof(header) + bestPixels.size());
        std::memcpy(request->bytes->data(), &header, sizeof(header));
        std::memcpy(request->bytes->data() + sizeof(header),
                    bestPixels.data(), bestPixels.size());
        return 1;
    }
    case kEditorShutdownMessage:
        KillTimer(hwnd, kEditorIdleTimer);
        DestroyWindow(hwnd);
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int runSharedVst2Server(const std::filesystem::path& path,
                        const std::wstring& mappingName,
                        const std::wstring& inputEvent,
                        const std::wstring& outputEvent,
                        const std::wstring& controlPipeName) {
    using namespace pluginscaler;

    ipc::AudioSharedChannel channel;
    if (!channel.open(mappingName, inputEvent, outputEvent)) {
        std::cerr << "error=shared-channel-open\n";
        return 10;
    }

    auto* block = channel.block();
    if (!block) return 11;

    formats::VST2PluginModule module;
    std::mutex moduleMutex;
    std::vector<std::uint8_t> persistedChunk;
    std::int32_t persistedChunkIndex = 0;
    std::atomic<bool> controlStop{false};

    std::int32_t configuredBlockSize = 512;
    std::uint32_t configuredSampleRate = 48000;
    std::uint32_t appliedParameterGeneration = 0;

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        std::string error;
        if (!module.openForProcessing(path, 48000.0, 512, error)) {
            std::cerr << "error=" << error << '\n';
            return 12;
        }
    }

    EditorGuiContext guiContext{};
    guiContext.module = &module;
    guiContext.moduleMutex = &moduleMutex;

    HANDLE guiReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!guiReady)
        return 15;

    std::thread guiThread([&] {
        static const wchar_t* kClassName = L"125A_PluginScaler_EditorSurrogate";
        WNDCLASSW wc{};
        wc.lpfnWndProc = editorSurrogateProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        ATOM atom = RegisterClassW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            SetEvent(guiReady);
            return;
        }

        HWND surrogate = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
            L"125A PluginScaler Editor Surrogate",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 32, 32,
            nullptr, nullptr, GetModuleHandleW(nullptr), &guiContext);
        SetEvent(guiReady);
        if (!surrogate)
            return;

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });

    WaitForSingleObject(guiReady, 3000);
    CloseHandle(guiReady);
    if (!guiContext.surrogate) {
        if (guiThread.joinable()) guiThread.join();
        return 16;
    }

    auto readExact = [](HANDLE pipe, void* data, DWORD bytes) -> bool {
        auto* p = static_cast<std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD got = 0;
            if (!ReadFile(pipe, p + done, bytes - done, &got, nullptr) || got == 0)
                return false;
            done += got;
        }
        return true;
    };
    auto writeExact = [](HANDLE pipe, const void* data, DWORD bytes) -> bool {
        const auto* p = static_cast<const std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD sent = 0;
            if (!WriteFile(pipe, p + done, bytes - done, &sent, nullptr) || sent == 0)
                return false;
            done += sent;
        }
        return true;
    };

    HANDLE controlServerPipe = CreateNamedPipeW(
        controlPipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (controlServerPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "error=control-pipe-create\n";
        return 14;
    }

    std::thread controlThread([&] {
        HANDLE pipe = controlServerPipe;

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            return;
        }

        while (!controlStop.load(std::memory_order_acquire)) {
            ipc::ControlMessageHeader req{};
            if (!readExact(pipe, &req, sizeof(req)))
                break;

            ipc::ControlMessageHeader resp{};
            resp.command = req.command;
            resp.arg0 = req.arg0;

            if (req.magic != ipc::kControlMagic ||
                req.version != ipc::kControlVersion ||
                req.payloadBytes > ipc::kMaxControlPayload) {
                resp.status = ipc::ControlStatus::InvalidRequest;
                if (!writeExact(pipe, &resp, sizeof(resp))) break;
                continue;
            }

            std::vector<std::uint8_t> payload(req.payloadBytes);
            if (req.payloadBytes &&
                !readExact(pipe, payload.data(), req.payloadBytes))
                break;

            std::vector<std::uint8_t> reply;

            if (req.command == ipc::ControlCommand::OpenEditor) {
                if (!payload.empty()) {
                    resp.status = ipc::ControlStatus::InvalidRequest;
                } else {
                    const LRESULT editorResult = SendMessageW(
                        guiContext.surrogate, kEditorOpenMessage, 0, 0);
                    HWND editor = reinterpret_cast<HWND>(editorResult);
                    if (!editor || !IsWindow(editor)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        ipc::EditorOpenResult result{};
                        result.surrogateWindow = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(guiContext.surrogate));
                        result.editorWindow = static_cast<std::uint64_t>(
                            reinterpret_cast<std::uintptr_t>(editor));
                        reply.resize(sizeof(result));
                        std::memcpy(reply.data(), &result, sizeof(result));
                    }
                }
            } else if (req.command == ipc::ControlCommand::CloseEditor) {
                if (!SendMessageW(guiContext.surrogate,
                                  kEditorCloseMessage, 0, 0))
                    resp.status = ipc::ControlStatus::PluginError;
            } else if (req.command == ipc::ControlCommand::CaptureEditor) {
                EditorCaptureRequest capture{};
                capture.bytes = &reply;
                if (!SendMessageW(guiContext.surrogate,
                                  kEditorCaptureMessage, 0,
                                  reinterpret_cast<LPARAM>(&capture)))
                    resp.status = ipc::ControlStatus::PluginError;
            } else if (req.command == ipc::ControlCommand::SendEditorMouse) {
                if (payload.size() != sizeof(ipc::EditorMousePayload) ||
                    !guiContext.editor || !IsWindow(guiContext.editor)) {
                    resp.status = ipc::ControlStatus::InvalidRequest;
                } else {
                    ipc::EditorMousePayload mouse{};
                    std::memcpy(&mouse, payload.data(), sizeof(mouse));
                    const UINT message = static_cast<UINT>(mouse.message);
                    const bool allowed =
                        message == WM_MOUSEMOVE ||
                        message == WM_LBUTTONDOWN ||
                        message == WM_LBUTTONUP ||
                        message == WM_RBUTTONDOWN ||
                        message == WM_RBUTTONUP;
                    if (!allowed) {
                        resp.status = ipc::ControlStatus::InvalidRequest;
                    } else {
                        const LPARAM coords = MAKELPARAM(
                            static_cast<short>(mouse.x),
                            static_cast<short>(mouse.y));
                        SendMessageW(guiContext.editor, message,
                                     static_cast<WPARAM>(mouse.keyFlags), coords);
                    }
                }
            } else if (req.command == ipc::ControlCommand::Shutdown) {
                controlStop.store(true, std::memory_order_release);
            } else {
                std::lock_guard<std::mutex> lock(moduleMutex);
                switch (req.command) {
                case ipc::ControlCommand::GetState: {
                    if (!module.getChunk(req.arg0, reply)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = reply;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                }
                case ipc::ControlCommand::SetState:
                    if (payload.empty() ||
                        !module.setChunk(req.arg0, payload.data(), payload.size())) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = payload;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                case ipc::ControlCommand::GetParameters: {
                    const auto count = std::max<std::int32_t>(0, module.numParams());
                    reply.resize(static_cast<std::size_t>(count) * sizeof(float));
                    auto* values = reinterpret_cast<float*>(reply.data());
                    for (std::int32_t i = 0; i < count; ++i)
                        values[i] = module.getParameter(i);
                    break;
                }
                case ipc::ControlCommand::GetEditorRect: {
                    formats::vst2abi::VstRect rect{};
                    if (!module.editorRect(rect)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        ipc::EditorRectPayload out{};
                        out.left = rect.left;
                        out.top = rect.top;
                        out.right = rect.right;
                        out.bottom = rect.bottom;
                        reply.resize(sizeof(out));
                        std::memcpy(reply.data(), &out, sizeof(out));
                    }
                    break;
                }
                default:
                    resp.status = ipc::ControlStatus::InvalidRequest;
                    break;
                }
            }

            if (reply.size() > ipc::kMaxControlPayload) {
                reply.clear();
                resp.status = ipc::ControlStatus::PayloadTooLarge;
            }
            resp.responseBytes = static_cast<std::uint32_t>(reply.size());

            if (!writeExact(pipe, &resp, sizeof(resp))) break;
            if (!reply.empty() &&
                !writeExact(pipe, reply.data(), resp.responseBytes))
                break;

            if (req.command == ipc::ControlCommand::Shutdown)
                break;
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });

    int resultCode = 0;
    for (;;) {
        if (!channel.waitForInput(std::chrono::seconds(10))) {
            if (controlStop.load(std::memory_order_acquire))
                break;
            std::cerr << "error=input-timeout\n";
            resultCode = 13;
            break;
        }

        const auto state = static_cast<ipc::AudioBlockState>(
            block->header.state.load(std::memory_order_acquire));

        if (state == ipc::AudioBlockState::Shutdown)
            break;

        if (state != ipc::AudioBlockState::InputReady) {
            block->header.errorCode = 101;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        if (block->header.frames == 0 || block->header.frames > ipc::kMaxAudioFrames ||
            block->header.sampleRateHz == 0 ||
            block->header.inputChannels > ipc::kMaxAudioChannels ||
            block->header.outputChannels > ipc::kMaxAudioChannels ||
            block->header.midiEventCount > ipc::kMaxMidiEvents ||
            block->header.parameterCount > ipc::kMaxParameters) {
            block->header.errorCode = 102;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Processing),
                                  std::memory_order_release);

        bool ok = true;
        {
            std::lock_guard<std::mutex> lock(moduleMutex);

            const auto requestedBlockSize = static_cast<std::int32_t>(block->header.frames);
            const auto requestedSampleRate = block->header.sampleRateHz;
            if (configuredBlockSize != requestedBlockSize ||
                configuredSampleRate != requestedSampleRate) {
                module.close();

                std::string error;
                if (!module.openForProcessing(path, static_cast<double>(requestedSampleRate),
                                              requestedBlockSize, error)) {
                    std::cerr << "error=" << error << '\n';
                    ok = false;
                } else {
                    configuredBlockSize = requestedBlockSize;
                    configuredSampleRate = requestedSampleRate;
                    appliedParameterGeneration = 0;
                    if (!persistedChunk.empty())
                        ok = module.setChunk(persistedChunkIndex,
                                             persistedChunk.data(),
                                             persistedChunk.size());
                }
            }

            if (ok && block->header.parameterCount > 0 &&
                appliedParameterGeneration != block->header.parameterGeneration) {
                for (std::uint32_t i = 0; i < block->header.parameterCount; ++i) {
                    if (!module.setParameter(static_cast<std::int32_t>(i),
                                             block->parameterValues[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    appliedParameterGeneration = block->header.parameterGeneration;
            }

            if (ok && block->header.midiEventCount > 0) {
                std::vector<formats::vst2abi::VstMidiEvent> midi(
                    static_cast<std::size_t>(block->header.midiEventCount));
                for (std::uint32_t i = 0; i < block->header.midiEventCount; ++i) {
                    auto& dst = midi[static_cast<std::size_t>(i)];
                    const auto& src = block->midiEvents[i];
                    dst.type = formats::vst2abi::kVstMidiType;
                    dst.byteSize = sizeof(formats::vst2abi::VstMidiEvent);
                    dst.deltaFrames = src.deltaFrames;
                    dst.flags = src.flags;
                    for (int b = 0; b < 4; ++b)
                        dst.midiData[b] = static_cast<char>(src.data[b]);
                }
                ok = module.processMidiEvents(midi.data(),
                                              static_cast<std::int32_t>(midi.size()));
            }

            if (ok) {
                const auto inCount = std::max<std::uint32_t>(1, block->header.inputChannels);
                const auto outCount = std::max<std::uint32_t>(1, block->header.outputChannels);
                std::vector<float*> inputs(inCount);
                std::vector<float*> outputs(outCount);
                for (std::uint32_t ch = 0; ch < inCount; ++ch)
                    inputs[ch] = block->inputs[ch];
                for (std::uint32_t ch = 0; ch < outCount; ++ch)
                    outputs[ch] = block->outputs[ch];

                ok = module.processReplacing(
                    inputs.data(), outputs.data(),
                    static_cast<std::int32_t>(block->header.frames));
            }
        }

        block->header.errorCode = ok ? 0u : 103u;
        block->header.state.store(static_cast<std::uint32_t>(
                                      ok ? ipc::AudioBlockState::OutputReady
                                         : ipc::AudioBlockState::Error),
                                  std::memory_order_release);
        channel.signalOutput();
    }

    controlStop.store(true, std::memory_order_release);

    // Wake a blocked control pipe server during normal audio shutdown.
    HANDLE wake = CreateFileW(controlPipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE)
        CloseHandle(wake);

    if (controlThread.joinable())
        controlThread.join();

    if (guiContext.surrogate && IsWindow(guiContext.surrogate))
        SendMessageW(guiContext.surrogate, kEditorShutdownMessage, 0, 0);
    if (guiThread.joinable())
        guiThread.join();

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        module.close();
    }
    return resultCode;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2")
        return runVst2Probe(argv[2]);

    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2-audio")
        return runVst2AudioProbe(argv[2]);

    if (argc == 4 && std::wstring_view(argv[1]) == L"--write-vst2-manifest")
        return writeVst2Manifest(argv[2], argv[3]);

    if (argc == 7 && std::wstring_view(argv[1]) == L"--serve-vst2-shm")
        return runSharedVst2Server(argv[2], argv[3], argv[4], argv[5], argv[6]);

    std::cout << "125A PluginScaler Helper\n"
              << "protocol=" << pluginscaler::ipc::kProtocolMajor << "."
              << pluginscaler::ipc::kProtocolMinor << "\n"
              << "usage:\n"
              << "  PluginScalerHelper --probe-vst2 <plugin.dll>\n"
              << "  PluginScalerHelper --probe-vst2-audio <plugin.dll>\n"
              << "  PluginScalerHelper --write-vst2-manifest <plugin.dll> <manifest.txt>\n"
              << "  PluginScalerHelper --serve-vst2-shm <plugin.dll> <map> <in-event> <out-event> <control-pipe>\n";
    return 0;
}
