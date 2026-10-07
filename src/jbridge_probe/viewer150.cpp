#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr int kScalePercent = 150;
constexpr UINT_PTR kCaptureTimer = 1;
constexpr UINT kCaptureIntervalMs = 33;

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
};

std::wstring lower(std::wstring v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
    return v;
}

std::set<DWORD> auxPids() {
    std::set<DWORD> out;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{}; e.dwSize = sizeof(e);
    if (Process32FirstW(s, &e)) {
        do {
            const auto n = lower(e.szExeFile);
            if (n == L"auxhost.exe" || n == L"auxhost64.exe")
                out.insert(e.th32ProcessID);
        } while (Process32NextW(s, &e));
    }
    CloseHandle(s);
    return out;
}

std::wstring cls(HWND h) {
    std::array<wchar_t, 512> b{};
    const int n = GetClassNameW(h, b.data(), static_cast<int>(b.size()));
    return n > 0 ? std::wstring(b.data(), static_cast<std::size_t>(n)) : L"";
}

struct EnumContext {
    const std::set<DWORD>* pids{};
    std::vector<HWND>* found{};
};

void maybe(HWND h, EnumContext& c) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!c.pids->contains(pid) || !IsWindowVisible(h)) return;
    const auto name = cls(h);
    if (name.rfind(L"NIVSTChildWindow", 0) != 0) return;
    RECT cr{};
    if (!GetClientRect(h, &cr)) return;
    if (cr.right - cr.left < 100 || cr.bottom - cr.top < 100) return;
    c.found->push_back(h);
}

BOOL CALLBACK childProc(HWND h, LPARAM p) {
    auto* c = reinterpret_cast<EnumContext*>(p);
    if (!c) return FALSE;
    maybe(h, *c);
    return TRUE;
}

BOOL CALLBACK topProc(HWND h, LPARAM p) {
    auto* c = reinterpret_cast<EnumContext*>(p);
    if (!c) return FALSE;
    maybe(h, *c);
    EnumChildWindows(h, childProc, p);
    return TRUE;
}

HWND findSource() {
    const auto pids = auxPids();
    std::vector<HWND> found;
    EnumContext c{&pids, &found};
    if (!pids.empty())
        EnumWindows(topProc, reinterpret_cast<LPARAM>(&c));
    if (found.empty()) return nullptr;

    return *std::max_element(found.begin(), found.end(), [](HWND a, HWND b) {
        RECT ra{}, rb{};
        GetClientRect(a, &ra);
        GetClientRect(b, &rb);
        return (ra.right - ra.left) * (ra.bottom - ra.top) <
               (rb.right - rb.left) * (rb.bottom - rb.top);
    });
}

bool createCaptureSurface(AppState& s) {
    RECT cr{};
    if (!GetClientRect(s.source, &cr))
        return false;

    s.nativeW = cr.right - cr.left;
    s.nativeH = cr.bottom - cr.top;
    if (s.nativeW <= 0 || s.nativeH <= 0)
        return false;

    s.scaledW = MulDiv(s.nativeW, kScalePercent, 100);
    s.scaledH = MulDiv(s.nativeH, kScalePercent, 100);

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    s.memoryDc = CreateCompatibleDC(screen);
    s.bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s.bmi.bmiHeader.biWidth = s.nativeW;
    s.bmi.bmiHeader.biHeight = -s.nativeH;
    s.bmi.bmiHeader.biPlanes = 1;
    s.bmi.bmiHeader.biBitCount = 32;
    s.bmi.bmiHeader.biCompression = BI_RGB;

    s.dib = CreateDIBSection(
        screen, &s.bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!s.memoryDc || !s.dib || !s.bits)
        return false;

    s.oldBitmap = SelectObject(s.memoryDc, s.dib);
    return s.oldBitmap != nullptr;
}

void destroyCaptureSurface(AppState& s) {
    if (s.memoryDc && s.oldBitmap)
        SelectObject(s.memoryDc, s.oldBitmap);
    if (s.dib)
        DeleteObject(s.dib);
    if (s.memoryDc)
        DeleteDC(s.memoryDc);
    s = {};
}

bool capture(AppState& s) {
    if (!s.source || !IsWindow(s.source) || !s.memoryDc)
        return false;
    PatBlt(s.memoryDc, 0, 0, s.nativeW, s.nativeH, BLACKNESS);
    return PrintWindow(s.source, s.memoryDc, PW_RENDERFULLCONTENT) != FALSE;
}

POINT toNativeClient(const AppState& s, LPARAM lp) {
    const int x = GET_X_LPARAM(lp);
    const int y = GET_Y_LPARAM(lp);
    POINT p{
        (std::clamp)(MulDiv(x, 100, kScalePercent), 0, (std::max)(0, s.nativeW - 1)),
        (std::clamp)(MulDiv(y, 100, kScalePercent), 0, (std::max)(0, s.nativeH - 1))
    };
    return p;
}

POINT dragNativePoint(const AppState& s) {
    POINT now{};
    if (!GetCursorPos(&now))
        return s.dragOriginNative;

    return POINT{
        s.dragOriginNative.x + MulDiv(now.x - s.dragOriginScreen.x, 100, kScalePercent),
        s.dragOriginNative.y + MulDiv(now.y - s.dragOriginScreen.y, 100, kScalePercent)
    };
}

LPARAM packPoint(POINT p) {
    return MAKELPARAM(
        static_cast<short>((std::clamp)(p.x, static_cast<LONG>(-32768), static_cast<LONG>(32767))),
        static_cast<short>((std::clamp)(p.y, static_cast<LONG>(-32768), static_cast<LONG>(32767))));
}

WPARAM buttonState(const AppState& s, WPARAM incoming = 0) {
    WPARAM state = incoming &
        (MK_SHIFT | MK_CONTROL | MK_XBUTTON1 | MK_XBUTTON2);
    if (s.leftDown) state |= MK_LBUTTON;
    if (s.rightDown) state |= MK_RBUTTON;
    if (s.middleDown) state |= MK_MBUTTON;
    return state;
}

void sendClientMouse(AppState& s, UINT msg, WPARAM wp, LPARAM lp) {
    if (!IsWindow(s.source)) return;
    const POINT p = toNativeClient(s, lp);
    PostMessageW(s.source, msg, wp, packPoint(p));
}

void sendDragMouse(AppState& s, UINT msg, WPARAM wp) {
    if (!IsWindow(s.source)) return;
    const POINT p = dragNativePoint(s);
    PostMessageW(s.source, msg, wp, packPoint(p));
}

void sendWheel(AppState& s, UINT msg, WPARAM wp, LPARAM lp) {
    if (!IsWindow(s.source)) return;
    POINT p = toNativeClient(s, lp);
    ClientToScreen(s.source, &p);
    PostMessageW(s.source, msg, wp, packPoint(p));
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(state));
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, kCaptureTimer, kCaptureIntervalMs, nullptr);
        return 0;

    case WM_TIMER:
        if (state && wp == kCaptureTimer) {
            if (!IsWindow(state->source)) {
                KillTimer(hwnd, kCaptureTimer);
                MessageBoxW(hwnd,
                    L"Das jBridge-Pluginfenster wurde geschlossen.",
                    L"125A jBridge 150% Viewer",
                    MB_OK | MB_ICONINFORMATION);
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
                0, 0, state->scaledW, state->scaledH,
                0, 0, state->nativeW, state->nativeH,
                state->bits, &state->bmi,
                DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (state) {
            if (state->leftDown && state->dragActive)
                sendDragMouse(*state, WM_MOUSEMOVE, buttonState(*state, wp));
            else
                sendClientMouse(*state, WM_MOUSEMOVE, buttonState(*state, wp), lp);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (state) {
            state->leftDown = true;
            state->dragActive = true;
            state->dragOriginNative = toNativeClient(*state, lp);
            GetCursorPos(&state->dragOriginScreen);
            SetCapture(hwnd);
            SetFocus(hwnd);
            PostMessageW(state->source, WM_LBUTTONDOWN,
                         buttonState(*state, wp),
                         packPoint(state->dragOriginNative));
        }
        return 0;

    case WM_LBUTTONUP:
        if (state) {
            if (state->dragActive)
                sendDragMouse(*state, WM_LBUTTONUP,
                              buttonState(*state, wp) & ~MK_LBUTTON);
            else
                sendClientMouse(*state, WM_LBUTTONUP,
                                buttonState(*state, wp) & ~MK_LBUTTON, lp);
            state->leftDown = false;
            state->dragActive = false;
            if (!state->rightDown && !state->middleDown && GetCapture() == hwnd)
                ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        if (state)
            sendClientMouse(*state, WM_LBUTTONDBLCLK, buttonState(*state, wp) | MK_LBUTTON, lp);
        return 0;

    case WM_RBUTTONDOWN:
        if (state) {
            state->rightDown = true;
            SetCapture(hwnd);
            sendClientMouse(*state, WM_RBUTTONDOWN, buttonState(*state, wp), lp);
        }
        return 0;

    case WM_RBUTTONUP:
        if (state) {
            sendClientMouse(*state, WM_RBUTTONUP, buttonState(*state, wp) & ~MK_RBUTTON, lp);
            state->rightDown = false;
            if (!state->leftDown && !state->middleDown && GetCapture() == hwnd)
                ReleaseCapture();
        }
        return 0;

    case WM_MBUTTONDOWN:
        if (state) {
            state->middleDown = true;
            SetCapture(hwnd);
            sendClientMouse(*state, WM_MBUTTONDOWN, buttonState(*state, wp), lp);
        }
        return 0;

    case WM_MBUTTONUP:
        if (state) {
            sendClientMouse(*state, WM_MBUTTONUP, buttonState(*state, wp) & ~MK_MBUTTON, lp);
            state->middleDown = false;
            if (!state->leftDown && !state->rightDown && GetCapture() == hwnd)
                ReleaseCapture();
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
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kCaptureTimer);
        if (GetCapture() == hwnd)
            ReleaseCapture();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HWND source = findSource();
    if (!source) {
        MessageBoxW(nullptr,
            L"Kein sichtbares NI-VST-Fenster in jBridge gefunden.\n\n"
            L"FM7 oder Pro-53 ueber jBridge oeffnen und erneut starten.",
            L"125A jBridge 150% Viewer",
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return 2;
    }

    AppState state{};
    state.source = source;
    if (!createCaptureSurface(state)) {
        MessageBoxW(nullptr,
            L"Capture-Oberflaeche konnte nicht erstellt werden.",
            L"125A jBridge 150% Viewer",
            MB_OK | MB_ICONERROR);
        return 3;
    }

    if (!capture(state)) {
        destroyCaptureSurface(state);
        MessageBoxW(nullptr,
            L"PrintWindow-Capture ist fehlgeschlagen.",
            L"125A jBridge 150% Viewer",
            MB_OK | MB_ICONERROR);
        return 4;
    }

    const wchar_t* className = L"125A.PluginScaler.JBridge150Viewer";
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    RECT wr{0, 0, state.scaledW, state.scaledH};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND window = CreateWindowExW(
        0, className, L"125A jBridge 150% Interactive Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, instance, &state);

    if (!window) {
        destroyCaptureSurface(state);
        return 5;
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    destroyCaptureSurface(state);
    return static_cast<int>(msg.wParam);
}
