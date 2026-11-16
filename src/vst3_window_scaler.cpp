// 125A PluginScaler: intentionally small, external Windows GUI experiment.
// Does not load VST2/VST3 modules, touch audio or inject code into the host.
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
constexpr int kCloseHotkey = 2;
constexpr int kCombo = 101;
constexpr int kStatus = 102;
constexpr int kOverlay = 103;
constexpr int kFit = 105;
constexpr UINT_PTR kRefreshTimer = 10;

struct App {
    HINSTANCE instance{};
    HWND controller{};
    HWND status{};
    HWND combo{};
    HWND overlayCheck{};
    HWND viewer{};
    HWND magnifier{};
    HWND source{};
    HWND mouseTarget{};
    WNDPROC originalMagnifierProc{};
    RECT sourceRect{};
    DWORD sourcePid{};
    DWORD sourceThread{};
    int percent{150};
    int effectivePercent{150};
    bool autoFit{true};
    HWND autoFitCheck{};
    bool overlay{true};
    // Preserve manually dragged window location across zoom/source changes.
    bool manualViewerPosition{};
    POINT draggedViewerTopLeft{};
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
    const int srcW = g.sourceRect.right-g.sourceRect.left;
    const int srcH = g.sourceRect.bottom-g.sourceRect.top;
    if(srcW<=0 || srcH<=0) return;
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g.viewer,GWL_STYLE));
    const DWORD exstyle = static_cast<DWORD>(GetWindowLongPtrW(g.viewer,GWL_EXSTYLE));
    const UINT dpi = GetDpiForWindow(g.viewer);
    RECT chrome{0,0,0,0};
    if(!AdjustWindowRectExForDpi(&chrome,style,FALSE,exstyle,dpi)) return;
    MONITORINFO mi{}; mi.cbSize=sizeof(mi);
    HMONITOR mon = g.manualViewerPosition ?
        MonitorFromPoint(g.draggedViewerTopLeft,MONITOR_DEFAULTTONEAREST) :
        MonitorFromWindow(g.source,MONITOR_DEFAULTTONEAREST);
    if(!GetMonitorInfoW(mon,&mi)) return;
    const int maxW=(std::max)(1,static_cast<int>(mi.rcWork.right-mi.rcWork.left-
                              (chrome.right-chrome.left)-16));
    const int maxH=(std::max)(1,static_cast<int>(mi.rcWork.bottom-mi.rcWork.top-
                              (chrome.bottom-chrome.top)-16));
    g.effectivePercent=g.autoFit ?
        scaler::fit_percent(g.percent,srcW,srcH,maxW,maxH) : g.percent;
    if(g.effectivePercent<=0) return;
    MAGTRANSFORM transform{};
    transform.v[0][0]=transform.v[1][1]=g.effectivePercent/100.f;
    transform.v[2][2]=1.f;
    if(!MagSetWindowTransform(g.magnifier,&transform)){
        set_status(L"Vergrößerung nicht möglich.");
        return;
    }
    const int width=scaler::scaled_pixels(srcW,g.effectivePercent);
    const int height=scaler::scaled_pixels(srcH,g.effectivePercent);
    RECT outer{0,0,width,height};
    if(!AdjustWindowRectExForDpi(&outer,style,FALSE,exstyle,dpi))return;
    // Align the actual viewer CLIENT region with the source client rectangle.
    // In overlay mode the unscaled editor remains visible to Magnification,
    // while the enlarged presentation covers it from the user's perspective.
    int x = g.sourceRect.left + outer.left;
    int y = g.sourceRect.top + outer.top;
    if (!g.overlay) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(g.source, MONITOR_DEFAULTTONEAREST), &monitor)) {
            x = g.sourceRect.right + 12;
            if (x + outer.right - outer.left > monitor.rcWork.right)
                x = monitor.rcWork.left + 24;
            y = std::max(monitor.rcWork.top,
                         std::min(g.sourceRect.top, monitor.rcWork.bottom - 140));
        }
    }
    if (g.manualViewerPosition) {
        x = g.draggedViewerTopLeft.x;
        y = g.draggedViewerTopLeft.y;
    }
    const int fullW=outer.right-outer.left,fullH=outer.bottom-outer.top;
    if(g.autoFit){
        x=(std::max)(static_cast<int>(mi.rcWork.left),
            (std::min)(x,static_cast<int>(mi.rcWork.right)-fullW));
        y=(std::max)(static_cast<int>(mi.rcWork.top),
            (std::min)(y,static_cast<int>(mi.rcWork.bottom)-fullH));
    }
    SetWindowPos(g.viewer,nullptr,x,y,fullW,fullH,SWP_NOZORDER|SWP_NOACTIVATE);
    MoveWindow(g.magnifier, 0, 0, width, height, TRUE);
    std::wstring caption=L"125A PluginScaler — "+std::to_wstring(g.percent)+L"%";
    if(g.percent!=g.effectivePercent)
        caption+=L" (angepasst: "+std::to_wstring(g.effectivePercent)+L"%)";
    SetWindowTextW(g.viewer,caption.c_str());
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
        g.sourceRect.left + scaler::source_pixel(p.x, g.effectivePercent),
        g.sourceRect.top + scaler::source_pixel(p.y, g.effectivePercent)
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

// The Magnification API needs the original editor visible. A cover window
// can replace the editor with its own pixels (observed in Studio Pro).
void configure_capture_exclusions() {
    if(!IsWindow(g.magnifier)) return;
    HWND excluded[]{g.viewer,g.controller};
    MagSetWindowFilterList(g.magnifier,MW_FILTERMODE_EXCLUDE,2,excluded);
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
    const bool movedOrResized = (bounds.left != g.sourceRect.left) ||
        (bounds.top != g.sourceRect.top) ||
        (bounds.right != g.sourceRect.right) ||
        (bounds.bottom != g.sourceRect.bottom);
    g.sourceRect = bounds;
    if (movedOrResized && g.overlay) apply_scale();
    if (movedOrResized && !g.overlay &&
        (bounds.right - bounds.left != 0)) {
        // Recalculate when the separate source changes size, but don't chase
        // mere source-window movement in separate-viewer mode.
        RECT viewerClient{};
        GetClientRect(g.magnifier, &viewerClient);
        if (viewerClient.right != scaler::scaled_pixels(
                bounds.right - bounds.left, g.effectivePercent) ||
            viewerClient.bottom != scaler::scaled_pixels(
                bounds.bottom - bounds.top, g.effectivePercent)) apply_scale();
    }
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
        configure_capture_exclusions();
        SetTimer(hwnd, kRefreshTimer, 33, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == kRefreshTimer) refresh();
        return 0;
    case WM_EXITSIZEMOVE: {
        // Triggered after the user drags the native Windows title bar. Do not
        // snap the viewer back on a later zoom or source-window movement.
        RECT moved{};
        if (GetWindowRect(hwnd, &moved)) {
            g.draggedViewerTopLeft = {moved.left, moved.top};
            g.manualViewerPosition = true;
        }
        return 0;
    }
    case WM_SIZE:
        // The viewer uses a fixed-size client area: zoom is controlled by the
        // percentage selector, not freeform stretching of a magnified bitmap.
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

// Recreate only the presentation window on a mode switch; never recreate
// or modify the foreign plugin HWND.
void create_viewer() {
    if (!IsWindow(g.source)) return;
    if (IsWindow(g.viewer)) DestroyWindow(g.viewer);
    // Never use a borderless WS_POPUP for the overlay: it had no draggable
    // caption, making the enlarged GUI impractical to reposition.
    // Both modes get a native full-height title bar, close and minimize.
    // No thickframe: resizing this bitmap-backed surface would clip content.
    g.manualViewerPosition = false;
    constexpr DWORD exstyle = WS_EX_APPWINDOW;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                            WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_VISIBLE;
    g.viewer = CreateWindowExW(exstyle, kViewerClass,
        L"125A PluginScaler (experimentell)", style,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 400,
        nullptr, nullptr, g.instance, nullptr);
    if (!g.viewer) {
        set_status(L"Viewer konnte nicht geöffnet werden.");
        return;
    }
    apply_scale();
    refresh();
    SetWindowPos(g.viewer, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        create_viewer();
        if (!IsWindow(g.viewer)) return;
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
            L"1. VST2-/VST3-Plugin in der DAW öffnen.\n"
            L"2. Maus über die eigentliche Plugin-Oberfläche bewegen.\n"
            L"3. Strg + Alt + F8 drücken.",
            WS_CHILD | WS_VISIBLE, 16, 16, 440, 67,
            hwnd, nullptr, g.instance, nullptr);
        CreateWindowW(L"STATIC", L"Vergrößerung:", WS_CHILD | WS_VISIBLE,
                      16, 100, 120, 20, hwnd, nullptr, g.instance, nullptr);
        g.combo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            142, 95, 110, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCombo)),
            g.instance, nullptr);
        const std::array<const wchar_t*, 4> labels{
            L"125 %", L"150 %", L"175 %", L"200 %"
        };
        for (auto label : labels)
            SendMessageW(g.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(g.combo, CB_SETCURSEL, 1, 0);
        g.overlayCheck = CreateWindowW(L"BUTTON", L"Über Original anzeigen (Overlay)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            16, 133, 320, 22, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOverlay)),
            g.instance, nullptr);
        SendMessageW(g.overlayCheck, BM_SETCHECK, BST_CHECKED, 0);
        g.autoFitCheck=CreateWindowW(L"BUTTON",L"An Bildschirm anpassen (Auto-Fit)",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            16,158,380,22,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFit)),
            g.instance,nullptr);
        SendMessageW(g.autoFitCheck,BM_SETCHECK,BST_CHECKED,0);
        g.status = CreateWindowW(L"STATIC",
            L"Bereit. Auswahl per Strg+Alt+F8.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 190, 445, 50, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatus)),
            g.instance, nullptr);
        if (!RegisterHotKey(hwnd, kCloseHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F9)) {
            set_status(L"Strg+Alt+F9 ist belegt; Viewer über Fenster schließen.");
        }
        if (!RegisterHotKey(hwnd, kHotkey,
                            MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8)) {
            set_status(L"Strg+Alt+F8 ist belegt; anderen Hotkey freigeben.");
        }
        return 0;
    }
    case WM_HOTKEY:
        if (wp == kHotkey) select_source();
        else if (wp == kCloseHotkey && IsWindow(g.viewer)) DestroyWindow(g.viewer);
        return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==kFit && HIWORD(wp)==BN_CLICKED){
            g.autoFit=SendMessageW(g.autoFitCheck,BM_GETCHECK,0,0)==BST_CHECKED;
            apply_scale();
        }
        if (LOWORD(wp) == kOverlay && HIWORD(wp) == BN_CLICKED) {
            const bool wantOverlay = SendMessageW(g.overlayCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (wantOverlay != g.overlay) {
                g.overlay = wantOverlay;
                if (IsWindow(g.source) && IsWindow(g.viewer)) create_viewer();
            }
        }
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
        UnregisterHotKey(hwnd, kCloseHotkey);
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
    HWND hwnd = CreateWindowExW(0, kControllerClass, L"125A PluginScaler — VST2/VST3 GUI-Prototyp",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 485, 295,
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
