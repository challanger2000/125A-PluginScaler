// 125A PluginScaler: intentionally small, external Windows GUI experiment.
// Does not load VST2/VST3 modules, touch audio or inject code into the host.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <magnification.h>

#include <array>
#include <algorithm>
#include <string>
#include "scaler/geometry.hpp"

namespace {
constexpr wchar_t kControllerClass[] = L"125A.VST3Scaler.Controller";
constexpr wchar_t kViewerClass[] = L"125A.VST3Scaler.Viewer";
constexpr int kHotkey = 1;
constexpr int kCombo = 101;
constexpr int kStatus = 102;
constexpr UINT_PTR kRefreshTimer = 10;

struct App {
    HINSTANCE instance{};
    HWND controller{};
    HWND status{};
    HWND combo{};
    HWND viewer{};
    HWND magnifier{};
    HWND source{};
    HWND mouseTarget{};
    WNDPROC originalMagnifierProc{};
    RECT sourceRect{};
    DWORD sourcePid{};
    DWORD sourceThread{};
    int percent{150};
    bool leftDown{};
    bool rightDown{};
} g;

void set_status(const std::wstring& message) {
    if (g.status) SetWindowTextW(g.status, message.c_str());
}

bool source_bounds(HWND hwnd, RECT& out) {
    RECT client{};
    if (!hwnd || !IsWindow(hwnd) || !GetClientRect(hwnd, &client) ||
        client.right <= client.left || client.bottom <= client.top) return false;
    POINT tl{client.left, client.top}, br{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
    out = {tl.x, tl.y, br.x, br.y};
    return out.right > out.left && out.bottom > out.top;
}

void apply_scale() {
    if (!g.viewer || !g.magnifier || !IsWindow(g.viewer)) return;
    MAGTRANSFORM transform{};
    transform.v[0][0] = transform.v[1][1] = static_cast<float>(g.percent) / 100.0f;
    transform.v[2][2] = 1.0f;
    if (!MagSetWindowTransform(g.magnifier, &transform)) {
        set_status(L"Fehler: Magnification-Transformation nicht möglich.");
        return;
    }
    const int width = scaler::scaled_pixels(g.sourceRect.right - g.sourceRect.left, g.percent);
    const int height = scaler::scaled_pixels(g.sourceRect.bottom - g.sourceRect.top, g.percent);
    if (width < 1 || height < 1) return;
    RECT outer{0, 0, width, height};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.viewer, GWL_STYLE));
    const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(g.viewer, GWL_EXSTYLE));
    const UINT dpi = GetDpiForWindow(g.viewer);
    if (!AdjustWindowRectExForDpi(&outer, style, FALSE, exstyle, dpi)) return;
    SetWindowPos(g.viewer, nullptr, 0, 0, outer.right - outer.left,
                 outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    MoveWindow(g.magnifier, 0, 0, width, height, TRUE);
    SetWindowTextW(g.viewer, (L"125A PluginScaler — " + std::to_wstring(g.percent) +
                             L"% (experimentell)").c_str());
}

HWND editor_child_at(POINT screen) {
    HWND h = g.source;
    for (int depth = 0; depth < 24 && IsWindow(h); ++depth) {
        POINT local = screen;
        if (!ScreenToClient(h, &local)) break;
        HWND child = RealChildWindowFromPoint(h, local);
        if (!child || child == h || !IsWindow(child) || !IsWindowVisible(child)) break;
        h = child;
    }
    return h;
}

// A best-effort message forwarder. Some plug-ins depend on GetCursorPos,
// focus, raw input, or real OS mouse capture. Such editors are NOT supported
// by this prototype merely because the graphic is magnified.
void forward_mouse(HWND receiver, UINT msg, WPARAM wp, LPARAM lp) {
    if (!IsWindow(g.source) || !IsWindow(g.magnifier)) return;
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        ScreenToClient(receiver, &p);
    }
    MapWindowPoints(receiver, g.magnifier, &p, 1);
    RECT magClient{};
    GetClientRect(g.magnifier, &magClient);
    const bool holding = g.leftDown || g.rightDown;
    if (!holding && (p.x < 0 || p.y < 0 ||
                     p.x >= magClient.right || p.y >= magClient.bottom)) return;
    POINT screen{
        g.sourceRect.left + scaler::source_pixel(p.x, g.percent),
        g.sourceRect.top + scaler::source_pixel(p.y, g.percent)
    };
    HWND target = holding && IsWindow(g.mouseTarget) ?
                  g.mouseTarget : editor_child_at(screen);
    if (!IsWindow(target)) return;

    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        if (!holding) {
            g.mouseTarget = target;
            SetCapture(g.viewer);
        }
        if (msg == WM_LBUTTONDOWN) g.leftDown = true;
        if (msg == WM_RBUTTONDOWN) g.rightDown = true;
    }
    if (g.leftDown) wp |= MK_LBUTTON;
    if (g.rightDown) wp |= MK_RBUTTON;
    // Mouse-up messages must no longer claim their own button is pressed.
    if (msg == WM_LBUTTONUP) wp &= ~static_cast<WPARAM>(MK_LBUTTON);
    if (msg == WM_RBUTTONUP) wp &= ~static_cast<WPARAM>(MK_RBUTTON);

    LPARAM mapped{};
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        mapped = MAKELPARAM(static_cast<short>(screen.x), static_cast<short>(screen.y));
    } else {
        POINT local = screen;
        if (!ScreenToClient(target, &local)) return;
        mapped = MAKELPARAM(static_cast<short>(local.x), static_cast<short>(local.y));
    }
    // Post, never SendMessage: a stalled foreign editor must not hang this UI.
    PostMessageW(target, msg, wp, mapped);
    if (msg == WM_LBUTTONUP) g.leftDown = false;
    if (msg == WM_RBUTTONUP) g.rightDown = false;
    if (!g.leftDown && !g.rightDown && GetCapture() == g.viewer) {
        ReleaseCapture();
        g.mouseTarget = nullptr;
    }
}

bool mouse_message(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return true;
    default: return false;
    }
}

LRESULT CALLBACK magnifier_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (mouse_message(msg)) {
        forward_mouse(hwnd, msg, wp, lp);
        return 0;
    }
    return CallWindowProcW(g.originalMagnifierProc, hwnd, msg, wp, lp);
}

void refresh() {
    DWORD pid{};
    const DWORD thread = IsWindow(g.source) ? GetWindowThreadProcessId(g.source, &pid) : 0;
    if (!thread || pid != g.sourcePid || thread != g.sourceThread) {
        set_status(L"Pluginfenster geschlossen. Neues Fenster mit Strg+Alt+F8 wählen.");
        if (IsWindow(g.viewer)) DestroyWindow(g.viewer);
        g.source = nullptr;
        return;
    }
    if (IsIconic(GetAncestor(g.source, GA_ROOT))) return;
    RECT bounds{};
    if (!source_bounds(g.source, bounds)) return;
    const bool resized = (bounds.right - bounds.left != g.sourceRect.right - g.sourceRect.left) ||
                         (bounds.bottom - bounds.top != g.sourceRect.bottom - g.sourceRect.top);
    g.sourceRect = bounds;
    if (resized) apply_scale();
    if (g.magnifier) MagSetWindowSource(g.magnifier, bounds);
}

LRESULT CALLBACK viewer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.viewer = hwnd;
        g.magnifier = CreateWindowW(WC_MAGNIFIER, L"",
                                    WS_CHILD | WS_VISIBLE,
                                    0, 0, 1, 1, hwnd, nullptr, g.instance, nullptr);
        if (!g.magnifier) return -1;
        SetLastError(0);
        auto previous = SetWindowLongPtrW(g.magnifier, GWLP_WNDPROC,
                         reinterpret_cast<LONG_PTR>(magnifier_proc));
        if (!previous && GetLastError() != 0) return -1;
        g.originalMagnifierProc = reinterpret_cast<WNDPROC>(previous);
        HWND excludes[]{hwnd, g.controller};
        MagSetWindowFilterList(g.magnifier, MW_FILTERMODE_EXCLUDE, 2, excludes);
        SetTimer(hwnd, kRefreshTimer, 33, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == kRefreshTimer) refresh();
        return 0;
    case WM_SIZE:
        // A manually resized frame may clip; the image keeps its chosen zoom.
        // Deliberately do not introduce a second, undocumented stretch factor.
        return 0;
    case WM_CAPTURECHANGED:
        g.leftDown = g.rightDown = false;
        g.mouseTarget = nullptr;
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        g.viewer = nullptr;
        g.magnifier = nullptr;
        g.originalMagnifierProc = nullptr;
        g.mouseTarget = nullptr;
        g.leftDown = g.rightDown = false;
        return 0;
    default:
        if (mouse_message(msg)) {
            forward_mouse(hwnd, msg, wp, lp);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void select_source() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    HWND candidate = WindowFromPoint(cursor);
    // Select the first reasonably sized visible HWND under the cursor, or
    // an ancestor of a very small child-control HWND.
    for (int depth = 0; candidate && depth < 24; ++depth, candidate = GetParent(candidate)) {
        if (candidate == g.controller || candidate == g.viewer ||
            IsChild(g.controller, candidate) ||
            (g.viewer && IsChild(g.viewer, candidate))) {
            set_status(L"Bitte über eine Plugin-Oberfläche zeigen, nicht auf den Scaler.");
            return;
        }
        DWORD pid{};
        GetWindowThreadProcessId(candidate, &pid);
        if (pid == GetCurrentProcessId()) return;
        RECT bounds{};
        if (!IsWindowVisible(candidate) || !source_bounds(candidate, bounds)) continue;
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (width < 120 || height < 70) continue;
        if (width > 2600 || height > 1800) {
            set_status(L"Auswahl zu groß (vermutlich das DAW-Fenster). Auf die Plugin-GUI zeigen.");
            return;
        }
        g.source = candidate;
        g.sourceThread = GetWindowThreadProcessId(candidate, &g.sourcePid);
        g.sourceRect = bounds;
        if (IsWindow(g.viewer)) DestroyWindow(g.viewer);
        const int outputW = scaler::scaled_pixels(width, g.percent);
        const int outputH = scaler::scaled_pixels(height, g.percent);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(candidate, MONITOR_DEFAULTTONEAREST), &monitor);
        int x = bounds.right + 12;
        if (x + outputW + 60 > monitor.rcWork.right) x = monitor.rcWork.left + 24;
        int y = std::max(monitor.rcWork.top, std::min(bounds.top, monitor.rcWork.bottom - 140));
        g.viewer = CreateWindowExW(WS_EX_APPWINDOW, kViewerClass,
            L"125A PluginScaler (experimentell)",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
            x, y, outputW + 40, outputH + 65,
            nullptr, nullptr, g.instance, nullptr);
        if (!g.viewer) {
            set_status(L"Viewer konnte nicht geöffnet werden.");
            return;
        }
        apply_scale();
        refresh();
        SetWindowPos(g.viewer, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        wchar_t cls[128]{};
        GetClassNameW(candidate, cls, 128);
        set_status(L"Quelle: " + std::wstring(cls) + L" (" +
                   std::to_wstring(width) + L"x" + std::to_wstring(height) +
                   L"). Mausbedienung experimentell.");
        return;
    }
    set_status(L"Kein geeignetes Pluginfenster unter dem Mauszeiger gefunden.");
}

LRESULT CALLBACK controller_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.controller = hwnd;
        CreateWindowW(L"STATIC",
            L"1. VST3-Plugin in der DAW öffnen.\n"
            L"2. Maus über die eigentliche Plugin-Oberfläche bewegen.\n"
            L"3. Strg + Alt + F8 drücken.",
            WS_CHILD | WS_VISIBLE, 16, 16, 440, 67,
            hwnd, nullptr, g.instance, nullptr);
        CreateWindowW(L"STATIC", L"Vergrößerung:", WS_CHILD | WS_VISIBLE,
                      16, 100, 120, 20, hwnd, nullptr, g.instance, nullptr);
        g.combo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            142, 95, 110, 120, hwnd, reinterpret_cast<HMENU>(kCombo),
            g.instance, nullptr);
        const std::array<const wchar_t*, 4> labels{
            L"125 %", L"150 %", L"175 %", L"200 %"
        };
        for (auto label : labels)
            SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(g.combo, CB_SETCURSEL, 1, 0);
        g.status = CreateWindowW(L"STATIC",
            L"Bereit. Auswahl per Strg+Alt+F8.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 138, 445, 42, hwnd, reinterpret_cast<HMENU>(kStatus),
            g.instance, nullptr);
        if (!RegisterHotKey(hwnd, kHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8)) {
            set_status(L"Strg+Alt+F8 ist belegt; anderen Hotkey freigeben.");
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == kHotkey) select_source();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kCombo && HIWORD(wp) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(g.combo, CB_GETCURSEL, 0, 0);
            constexpr std::array<int, 4> values{125, 150, 175, 200};
            if (selected >= 0 && selected < static_cast<LRESULT>(values.size())) {
                g.percent = values[static_cast<size_t>(selected)];
                apply_scale();
                refresh();
            }
        }
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, kHotkey);
        if (IsWindow(g.viewer)) DestroyWindow(g.viewer);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g.instance = instance;
    // A standalone application only: does NOT modify the host DAW's DPI context.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!MagInitialize()) {
        MessageBoxW(nullptr, L"Die Windows Magnification API ist nicht verfügbar.",
                    L"125A PluginScaler", MB_OK | MB_ICONERROR);
        return 2;
    }
    WNDCLASSW control{};
    control.lpfnWndProc = controller_proc;
    control.hInstance = instance;
    control.lpszClassName = kControllerClass;
    control.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    control.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    WNDCLASSW viewer = control;
    viewer.lpfnWndProc = viewer_proc;
    viewer.lpszClassName = kViewerClass;
    viewer.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&control) || !RegisterClassW(&viewer)) {
        MagUninitialize();
        return 3;
    }
    HWND hwnd = CreateWindowExW(0, kControllerClass, L"125A PluginScaler — VST3 GUI-Prototyp",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 485, 225,
        nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MagUninitialize();
        return 4;
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    MagUninitialize();
    return static_cast<int>(message.wParam);
}
