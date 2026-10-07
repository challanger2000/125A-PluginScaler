#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kScalePercent = 150;
constexpr UINT_PTR kCaptureTimer = 1;
constexpr UINT kCaptureIntervalMs = 33;
constexpr DWORD kManagerPollMs = 250;

constexpr DWORD kExitNormal = 0;
constexpr DWORD kExitUserClosed = 20;
constexpr DWORD kExitSourceLost = 21;

const wchar_t* kViewerClass = L"125A.PluginScaler.JBridge150Viewer";
const wchar_t* kManagerMutex = L"Local\\125A.PluginScaler.JBridge150.Manager";

struct AppState {
    HWND source{};
    int nativeW{};
    int nativeH{};
    int scaledW{};
    int scaledH{};
    HDC memoryDc{};
    HBITMAP dib{};
    HGDIOBJ oldBitmap{};
    void* bits{};
    BITMAPINFO bmi{};
    bool leftDown{};
    bool rightDown{};
    bool middleDown{};
    bool dragActive{};
    POINT dragOriginScreen{};
    POINT dragOriginNative{};
    HWND parkedWindow{};
    RECT parkedRect{};
    bool parked{};
    HWND contextRoot{};
    bool contextTopmost{};
    bool restoreOnClose{true};
    DWORD exitCode{kExitNormal};
};

struct CandidateWindow {
    HWND hwnd{};
    HWND editorRoot{};
    DWORD pid{};
    int area{};
    int depth{};
};

struct ViewerChild {
    HWND source{};
    HANDLE process{};
    DWORD processId{};
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::set<DWORD> auxPids() {
    std::set<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const auto image = lower(entry.szExeFile);
            if (image == L"auxhost.exe" || image == L"auxhost64.exe")
                result.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

std::wstring className(HWND hwnd) {
    std::array<wchar_t, 512> buffer{};
    const int n = GetClassNameW(
        hwnd, buffer.data(), static_cast<int>(buffer.size()));
    return n > 0
        ? std::wstring(buffer.data(), static_cast<std::size_t>(n))
        : std::wstring{};
}

std::wstring windowText(HWND hwnd) {
    std::array<wchar_t, 1024> buffer{};
    const int n = GetWindowTextW(
        hwnd, buffer.data(), static_cast<int>(buffer.size()));
    return n > 0
        ? std::wstring(buffer.data(), static_cast<std::size_t>(n))
        : std::wstring{};
}

int clientArea(HWND hwnd) {
    RECT r{};
    if (!GetClientRect(hwnd, &r))
        return 0;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0)
        return 0;
    return w * h;
}

HWND chooseWholeEditorWindow(HWND source) {
    if (!source || !IsWindow(source))
        return nullptr;

    HWND target = source;
    HWND current = source;

    RECT sourceRect{};
    if (!GetWindowRect(source, &sourceRect))
        return nullptr;

    const int sourceW = sourceRect.right - sourceRect.left;
    const int sourceH = sourceRect.bottom - sourceRect.top;

    while (HWND parent = GetParent(current)) {
        RECT r{};
        if (!GetWindowRect(parent, &r))
            break;

        const int w = r.right - r.left;
        const int h = r.bottom - r.top;

        // Stay inside the compact editor/wrapper hierarchy and stop before
        // a large DAW workspace. No plugin-vendor class-name assumptions.
        if (w <= 0 || h <= 0 ||
            w > sourceW + 320 || h > sourceH + 380)
            break;

        target = parent;
        current = parent;
    }

    return target;
}

int depthFrom(HWND hwnd, HWND ancestor) {
    int depth = 0;
    HWND current = hwnd;
    while (current && current != ancestor && depth < 64) {
        current = GetParent(current);
        ++depth;
    }
    return current == ancestor ? depth : 0;
}

struct EnumContext {
    const std::set<DWORD>* pids{};
    std::vector<CandidateWindow>* candidates{};
};

void maybeCandidate(HWND hwnd, EnumContext& ctx) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || !ctx.pids->contains(pid) || !IsWindowVisible(hwnd))
        return;

    RECT cr{};
    if (!GetClientRect(hwnd, &cr))
        return;

    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w < 100 || h < 100)
        return;

    const HWND editorRoot = chooseWholeEditorWindow(hwnd);
    if (!editorRoot || !IsWindow(editorRoot))
        return;

    ctx.candidates->push_back({
        hwnd,
        editorRoot,
        pid,
        w * h,
        depthFrom(hwnd, editorRoot)
    });
}

BOOL CALLBACK enumChildProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!ctx)
        return FALSE;
    maybeCandidate(hwnd, *ctx);
    return TRUE;
}

BOOL CALLBACK enumTopProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!ctx)
        return FALSE;

    // Always enumerate descendants, including auxhost children embedded
    // under a DAW-owned top-level window.
    maybeCandidate(hwnd, *ctx);
    EnumChildWindows(hwnd, enumChildProc, param);
    return TRUE;
}

bool looksLikeJBridgeShell(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd))
        return false;

    const auto cls = lower(className(hwnd));
    const auto title = lower(windowText(hwnd));

    return cls == L"#32770" &&
           title.find(L"jbridge") != std::wstring::npos;
}

std::vector<HWND> discoverSources() {
    const auto pids = auxPids();
    if (pids.empty())
        return {};

    std::vector<CandidateWindow> candidates;
    EnumContext ctx{&pids, &candidates};
    EnumWindows(enumTopProc, reinterpret_cast<LPARAM>(&ctx));

    // Group candidates by compact editor/wrapper root. This avoids the old
    // "largest NI window wins" behavior and allows multiple simultaneous
    // jBridge plugin instances from any vendor.
    std::map<std::uintptr_t, std::vector<CandidateWindow>> groups;
    for (const auto& candidate : candidates) {
        const auto key = reinterpret_cast<std::uintptr_t>(
            candidate.editorRoot);
        groups[key].push_back(candidate);
    }

    std::vector<HWND> result;
    result.reserve(groups.size());

    for (auto& [key, group] : groups) {
        (void)key;
        if (group.empty())
            continue;

        const HWND root = group.front().editorRoot;
        const bool shell = looksLikeJBridgeShell(root);

        auto chooseBest = [&](bool descendantsOnly) -> HWND {
            const CandidateWindow* best = nullptr;
            for (const auto& c : group) {
                if (descendantsOnly && c.hwnd == root)
                    continue;

                if (!best ||
                    c.area > best->area ||
                    (c.area == best->area && c.depth > best->depth)) {
                    best = &c;
                }
            }
            return best ? best->hwnd : nullptr;
        };

        // If the compact root is the generic jBridge dialog shell, prefer
        // its largest real content descendant. Otherwise use the largest
        // vendor-agnostic candidate in the group.
        HWND source = shell ? chooseBest(true) : nullptr;
        if (!source)
            source = chooseBest(false);

        if (source)
            result.push_back(source);
    }

    return result;
}

bool sourceBelongsToJBridge(HWND source) {
    if (!source || !IsWindow(source))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(source, &pid);
    const auto pids = auxPids();
    return pid && pids.contains(pid);
}

bool createCaptureSurface(AppState& state) {
    RECT cr{};
    if (!GetClientRect(state.source, &cr))
        return false;

    state.nativeW = cr.right - cr.left;
    state.nativeH = cr.bottom - cr.top;
    if (state.nativeW <= 0 || state.nativeH <= 0)
        return false;

    state.scaledW = MulDiv(state.nativeW, kScalePercent, 100);
    state.scaledH = MulDiv(state.nativeH, kScalePercent, 100);

    HDC screen = GetDC(nullptr);
    if (!screen)
        return false;

    state.memoryDc = CreateCompatibleDC(screen);
    state.bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    state.bmi.bmiHeader.biWidth = state.nativeW;
    state.bmi.bmiHeader.biHeight = -state.nativeH;
    state.bmi.bmiHeader.biPlanes = 1;
    state.bmi.bmiHeader.biBitCount = 32;
    state.bmi.bmiHeader.biCompression = BI_RGB;

    state.dib = CreateDIBSection(
        screen, &state.bmi, DIB_RGB_COLORS,
        &state.bits, nullptr, 0);

    ReleaseDC(nullptr, screen);

    if (!state.memoryDc || !state.dib || !state.bits)
        return false;

    state.oldBitmap = SelectObject(state.memoryDc, state.dib);
    return state.oldBitmap != nullptr;
}

void destroyCaptureSurface(AppState& state) {
    if (state.memoryDc && state.oldBitmap)
        SelectObject(state.memoryDc, state.oldBitmap);
    if (state.dib)
        DeleteObject(state.dib);
    if (state.memoryDc)
        DeleteDC(state.memoryDc);

    state.memoryDc = nullptr;
    state.dib = nullptr;
    state.oldBitmap = nullptr;
    state.bits = nullptr;
}

bool capture(AppState& state) {
    if (!state.source ||
        !IsWindow(state.source) ||
        !state.memoryDc)
        return false;

    PatBlt(
        state.memoryDc,
        0, 0,
        state.nativeW, state.nativeH,
        BLACKNESS);

    return PrintWindow(
        state.source,
        state.memoryDc,
        PW_RENDERFULLCONTENT) != FALSE;
}

bool parkWholeEditorTree(AppState& state) {
    HWND target = chooseWholeEditorWindow(state.source);
    if (!target || !IsWindow(target))
        return false;

    HWND parent = GetParent(target);

    RECT r{};
    if (!GetWindowRect(target, &r))
        return false;

    POINT tl{r.left, r.top};
    POINT br{r.right, r.bottom};
    if (parent) {
        ScreenToClient(parent, &tl);
        ScreenToClient(parent, &br);
    }

    state.parkedWindow = target;
    state.parkedRect = {tl.x, tl.y, br.x, br.y};

    const int w = static_cast<int>((std::max)(
        static_cast<LONG>(1), br.x - tl.x));
    const int h = static_cast<int>((std::max)(
        static_cast<LONG>(1), br.y - tl.y));

    if (!SetWindowPos(
            target,
            nullptr,
            -30000, -30000,
            w, h,
            SWP_NOZORDER | SWP_NOACTIVATE))
        return false;

    state.parked = true;
    return true;
}

void restoreWholeEditorTree(AppState& state) {
    if (!state.parked ||
        !state.parkedWindow ||
        !IsWindow(state.parkedWindow))
        return;

    const int x = static_cast<int>(state.parkedRect.left);
    const int y = static_cast<int>(state.parkedRect.top);
    const int w = static_cast<int>((std::max)(
        static_cast<LONG>(1),
        state.parkedRect.right - state.parkedRect.left));
    const int h = static_cast<int>((std::max)(
        static_cast<LONG>(1),
        state.parkedRect.bottom - state.parkedRect.top));

    SetWindowPos(
        state.parkedWindow,
        nullptr,
        x, y, w, h,
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_ASYNCWINDOWPOS);

    state.parked = false;
}

POINT toNativeClient(const AppState& state, LPARAM lp) {
    const int x = GET_X_LPARAM(lp);
    const int y = GET_Y_LPARAM(lp);

    return POINT{
        (std::clamp)(
            MulDiv(x, 100, kScalePercent),
            0,
            (std::max)(0, state.nativeW - 1)),
        (std::clamp)(
            MulDiv(y, 100, kScalePercent),
            0,
            (std::max)(0, state.nativeH - 1))
    };
}

POINT dragNativePoint(const AppState& state) {
    POINT now{};
    if (!GetCursorPos(&now))
        return state.dragOriginNative;

    return POINT{
        state.dragOriginNative.x +
            MulDiv(
                now.x - state.dragOriginScreen.x,
                100,
                kScalePercent),
        state.dragOriginNative.y +
            MulDiv(
                now.y - state.dragOriginScreen.y,
                100,
                kScalePercent)
    };
}

LPARAM packPoint(POINT point) {
    return MAKELPARAM(
        static_cast<short>((std::clamp)(
            point.x,
            static_cast<LONG>(-32768),
            static_cast<LONG>(32767))),
        static_cast<short>((std::clamp)(
            point.y,
            static_cast<LONG>(-32768),
            static_cast<LONG>(32767))));
}

WPARAM buttonState(
    const AppState& state,
    WPARAM incoming = 0) {

    WPARAM result =
        incoming &
        (MK_SHIFT |
         MK_CONTROL |
         MK_XBUTTON1 |
         MK_XBUTTON2);

    if (state.leftDown)
        result |= MK_LBUTTON;
    if (state.rightDown)
        result |= MK_RBUTTON;
    if (state.middleDown)
        result |= MK_MBUTTON;

    return result;
}

void sendClientMouse(
    AppState& state,
    UINT msg,
    WPARAM wp,
    LPARAM lp) {

    if (!IsWindow(state.source))
        return;

    const POINT point = toNativeClient(state, lp);
    PostMessageW(
        state.source,
        msg,
        wp,
        packPoint(point));
}

void sendDragMouse(
    AppState& state,
    UINT msg,
    WPARAM wp) {

    if (!IsWindow(state.source))
        return;

    const POINT point = dragNativePoint(state);
    PostMessageW(
        state.source,
        msg,
        wp,
        packPoint(point));
}

void sendWheel(
    AppState& state,
    UINT msg,
    WPARAM wp,
    LPARAM lp) {

    if (!IsWindow(state.source))
        return;

    POINT point = toNativeClient(state, lp);
    ClientToScreen(state.source, &point);
    PostMessageW(
        state.source,
        msg,
        wp,
        packPoint(point));
}

void syncContextZOrder(HWND viewer, AppState& state) {
    if (!viewer || !IsWindow(viewer))
        return;

    HWND foreground = GetForegroundWindow();
    HWND foregroundRoot =
        foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;

    const bool sameContext =
        foreground == viewer ||
        (state.contextRoot &&
         foregroundRoot == state.contextRoot);

    if (sameContext == state.contextTopmost)
        return;

    SetWindowPos(
        viewer,
        sameContext ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOACTIVATE);

    state.contextTopmost = sameContext;
}

LRESULT CALLBACK viewerWindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp) {

    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(
            hwnd,
            kCaptureTimer,
            kCaptureIntervalMs,
            nullptr);
        return 0;

    case WM_TIMER:
        if (state && wp == kCaptureTimer) {
            syncContextZOrder(hwnd, *state);

            if (!IsWindow(state->source)) {
                // Host lifecycle: the editor disappeared. Do not restore the
                // parked wrapper onscreen; the host is closing/rebuilding it.
                state->restoreOnClose = false;
                state->exitCode = kExitSourceLost;
                KillTimer(hwnd, kCaptureTimer);
                DestroyWindow(hwnd);
                return 0;
            }

            if (capture(*state))
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        if (state && state->bits) {
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(
                dc,
                0, 0,
                state->scaledW, state->scaledH,
                0, 0,
                state->nativeW, state->nativeH,
                state->bits,
                &state->bmi,
                DIB_RGB_COLORS,
                SRCCOPY);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (state) {
            if (state->leftDown && state->dragActive) {
                sendDragMouse(
                    *state,
                    WM_MOUSEMOVE,
                    buttonState(*state, wp));
            } else {
                sendClientMouse(
                    *state,
                    WM_MOUSEMOVE,
                    buttonState(*state, wp),
                    lp);
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (state) {
            state->leftDown = true;
            state->dragActive = true;
            state->dragOriginNative =
                toNativeClient(*state, lp);
            GetCursorPos(&state->dragOriginScreen);
            SetCapture(hwnd);
            SetFocus(hwnd);

            PostMessageW(
                state->source,
                WM_LBUTTONDOWN,
                buttonState(*state, wp),
                packPoint(state->dragOriginNative));
        }
        return 0;

    case WM_LBUTTONUP:
        if (state) {
            if (state->dragActive) {
                sendDragMouse(
                    *state,
                    WM_LBUTTONUP,
                    buttonState(*state, wp) &
                        ~MK_LBUTTON);
            } else {
                sendClientMouse(
                    *state,
                    WM_LBUTTONUP,
                    buttonState(*state, wp) &
                        ~MK_LBUTTON,
                    lp);
            }

            state->leftDown = false;
            state->dragActive = false;

            if (!state->rightDown &&
                !state->middleDown &&
                GetCapture() == hwnd) {
                ReleaseCapture();
            }
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (state) {
            sendClientMouse(
                *state,
                WM_LBUTTONDBLCLK,
                buttonState(*state, wp) |
                    MK_LBUTTON,
                lp);
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (state) {
            state->rightDown = true;
            SetCapture(hwnd);
            sendClientMouse(
                *state,
                WM_RBUTTONDOWN,
                buttonState(*state, wp),
                lp);
        }
        return 0;

    case WM_RBUTTONUP:
        if (state) {
            sendClientMouse(
                *state,
                WM_RBUTTONUP,
                buttonState(*state, wp) &
                    ~MK_RBUTTON,
                lp);

            state->rightDown = false;

            if (!state->leftDown &&
                !state->middleDown &&
                GetCapture() == hwnd) {
                ReleaseCapture();
            }
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (state) {
            state->middleDown = true;
            SetCapture(hwnd);
            sendClientMouse(
                *state,
                WM_MBUTTONDOWN,
                buttonState(*state, wp),
                lp);
        }
        return 0;

    case WM_MBUTTONUP:
        if (state) {
            sendClientMouse(
                *state,
                WM_MBUTTONUP,
                buttonState(*state, wp) &
                    ~MK_MBUTTON,
                lp);

            state->middleDown = false;

            if (!state->leftDown &&
                !state->rightDown &&
                GetCapture() == hwnd) {
                ReleaseCapture();
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (state)
            sendWheel(*state, WM_MOUSEWHEEL, wp, lp);
        return 0;

    case WM_MOUSEHWHEEL:
        if (state)
            sendWheel(*state, WM_MOUSEHWHEEL, wp, lp);
        return 0;

    case WM_CAPTURECHANGED:
        if (state) {
            state->leftDown = false;
            state->rightDown = false;
            state->middleDown = false;
            state->dragActive = false;
        }
        return 0;

    case WM_CLOSE:
        if (state) {
            state->exitCode = kExitUserClosed;
            state->restoreOnClose = true;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kCaptureTimer);

        if (state && state->contextTopmost) {
            SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE |
                SWP_NOSIZE |
                SWP_NOACTIVATE);
            state->contextTopmost = false;
        }

        if (GetCapture() == hwnd)
            ReleaseCapture();

        if (state &&
            state->restoreOnClose) {
            restoreWholeEditorTree(*state);
        }

        PostQuitMessage(
            static_cast<int>(
                state ? state->exitCode : kExitNormal));
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool registerViewerClass(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = viewerWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kViewerClass;

    return RegisterClassW(&wc) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int runViewer(
    HINSTANCE instance,
    int show,
    HWND source) {

    if (!sourceBelongsToJBridge(source))
        return 2;

    AppState state{};
    state.source = source;
    state.contextRoot =
        GetAncestor(source, GA_ROOT);

    if (!createCaptureSurface(state))
        return 3;

    if (!capture(state)) {
        destroyCaptureSurface(state);
        return 4;
    }

    if (!registerViewerClass(instance)) {
        destroyCaptureSurface(state);
        return 5;
    }

    constexpr DWORD viewerStyle =
        WS_OVERLAPPED |
        WS_CAPTION |
        WS_SYSMENU |
        WS_MINIMIZEBOX;

    RECT wr{
        0, 0,
        state.scaledW,
        state.scaledH
    };

    AdjustWindowRectEx(
        &wr,
        viewerStyle,
        FALSE,
        0);

    HWND window = CreateWindowExW(
        0,
        kViewerClass,
        L"125A jBridge 150% Interactive Viewer",
        viewerStyle | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        instance,
        &state);

    if (!window) {
        destroyCaptureSurface(state);
        return 6;
    }

    ShowWindow(window, show);
    UpdateWindow(window);
    syncContextZOrder(window, state);

    if (!parkWholeEditorTree(state)) {
        state.restoreOnClose = true;
        DestroyWindow(window);
        destroyCaptureSurface(state);
        return 7;
    }

    if (!capture(state)) {
        state.restoreOnClose = true;
        DestroyWindow(window);
        destroyCaptureSurface(state);
        return 8;
    }

    InvalidateRect(window, nullptr, FALSE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    destroyCaptureSurface(state);
    return static_cast<int>(msg.wParam);
}

std::wstring executablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD n = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));

    if (!n || n >= path.size())
        return {};

    return std::wstring(
        path.data(),
        static_cast<std::size_t>(n));
}

bool spawnViewerChild(
    HWND source,
    ViewerChild& child) {

    const std::wstring exe = executablePath();
    if (exe.empty())
        return false;

    std::wstring command =
        L"\"" + exe + L"\" --viewer " +
        std::to_wstring(
            reinterpret_cast<std::uintptr_t>(source));

    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        return false;
    }

    CloseHandle(pi.hThread);

    child.source = source;
    child.process = pi.hProcess;
    child.processId = pi.dwProcessId;
    return true;
}

bool childTracksSource(
    const std::vector<ViewerChild>& children,
    HWND source) {

    return std::any_of(
        children.begin(),
        children.end(),
        [source](const ViewerChild& child) {
            return child.source == source;
        });
}

void reapChildren(
    std::vector<ViewerChild>& children,
    std::set<std::uintptr_t>& suppressed) {

    auto it = children.begin();
    while (it != children.end()) {
        if (WaitForSingleObject(
                it->process, 0) != WAIT_OBJECT_0) {
            ++it;
            continue;
        }

        DWORD exitCode = 0;
        GetExitCodeProcess(
            it->process,
            &exitCode);

        if (exitCode == kExitUserClosed &&
            IsWindow(it->source)) {
            suppressed.insert(
                reinterpret_cast<std::uintptr_t>(
                    it->source));
        }

        CloseHandle(it->process);
        it = children.erase(it);
    }
}

void pruneSuppressed(
    std::set<std::uintptr_t>& suppressed) {

    auto it = suppressed.begin();
    while (it != suppressed.end()) {
        HWND hwnd = reinterpret_cast<HWND>(*it);
        if (!IsWindow(hwnd))
            it = suppressed.erase(it);
        else
            ++it;
    }
}

bool isSuppressed(
    const std::set<std::uintptr_t>& suppressed,
    HWND source) {

    return suppressed.contains(
        reinterpret_cast<std::uintptr_t>(source));
}

int runManager(HINSTANCE instance, int show) {
    HANDLE mutex = CreateMutexW(
        nullptr,
        TRUE,
        kManagerMutex);

    if (!mutex)
        return 30;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    std::vector<ViewerChild> children;
    std::set<std::uintptr_t> suppressed;
    unsigned idleWithoutJBridge = 0;

    for (;;) {
        reapChildren(children, suppressed);
        pruneSuppressed(suppressed);

        const auto sources = discoverSources();

        for (HWND source : sources) {
            if (!IsWindow(source) ||
                childTracksSource(children, source) ||
                isSuppressed(suppressed, source)) {
                continue;
            }

            ViewerChild child{};
            if (spawnViewerChild(source, child))
                children.push_back(child);
        }

        // If every jBridge process is gone, leave after a short grace period.
        // Otherwise remain resident so track switches/reopens/new instances
        // can be detected automatically.
        if (auxPids().empty() && children.empty()) {
            ++idleWithoutJBridge;
            if (idleWithoutJBridge >= 8)
                break;
        } else {
            idleWithoutJBridge = 0;
        }

        const DWORD wait = MsgWaitForMultipleObjects(
            0,
            nullptr,
            FALSE,
            kManagerPollMs,
            QS_ALLINPUT);

        if (wait == WAIT_OBJECT_0) {
            MSG msg{};
            while (PeekMessageW(
                       &msg,
                       nullptr,
                       0, 0,
                       PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    ReleaseMutex(mutex);
                    CloseHandle(mutex);
                    return static_cast<int>(msg.wParam);
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }

    for (auto& child : children) {
        if (child.process)
            CloseHandle(child.process);
    }

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

bool parseViewerSource(
    PWSTR commandLine,
    HWND& source) {

    source = nullptr;

    if (!commandLine)
        return false;

    std::wstring args = commandLine;
    constexpr const wchar_t* prefix = L"--viewer ";

    if (args.rfind(prefix, 0) != 0)
        return false;

    const wchar_t* number =
        args.c_str() + std::wcslen(prefix);

    wchar_t* end = nullptr;
    const unsigned long long value =
        std::wcstoull(number, &end, 10);

    if (end == number || value == 0)
        return false;

    source = reinterpret_cast<HWND>(
        static_cast<std::uintptr_t>(value));

    return true;
}

} // namespace

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR commandLine,
    int show) {

    HWND source = nullptr;

    if (parseViewerSource(commandLine, source))
        return runViewer(instance, show, source);

    return runManager(instance, show);
}
