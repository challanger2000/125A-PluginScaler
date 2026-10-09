#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClass[] = L"125A.JBridgeScaler.Viewer";
HWND viewer{}, magnifier{}, source{};
RECT sourceRect{};
int percent = 150;
bool dragging = false;
UINT_PTR refreshTimer = 1;

bool jbridgePid(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W info{};
    info.dwSize = sizeof(info);
    bool found = false;
    if (Process32FirstW(snapshot, &info)) {
        do {
            if (info.th32ProcessID != pid) continue;
            std::wstring s = info.szExeFile;
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t x) {
                return static_cast<wchar_t>(towlower(x)); });
            found = s == L"auxhost.exe" || s == L"auxhost64.exe" ||
                    s == L"gauxhost.exe" || s == L"gauxhost64.exe";
            break;
        } while (Process32NextW(snapshot, &info));
    }
    CloseHandle(snapshot);
    return found;
}

struct Candidate { HWND hwnd{}; long area{}; };
void inspectCandidate(HWND hwnd, Candidate& c) {
    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (!jbridgePid(pid)) return;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    const long w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 120 || h < 90 || w > 3000 || h > 2000) return;
    if (w * h > c.area) c = {hwnd, w * h};
}

BOOL CALLBACK inspectChild(HWND hwnd, LPARAM param) {
    inspectCandidate(hwnd, *reinterpret_cast<Candidate*>(param));
    return TRUE;
}
BOOL CALLBACK chooseWindow(HWND hwnd, LPARAM param) {
    auto& candidate = *reinterpret_cast<Candidate*>(param);
    inspectCandidate(hwnd, candidate);
    EnumChildWindows(hwnd, inspectChild, param);
    return TRUE;
}

HWND locateSource() {
    // First examine the active plugin tree: an embedded x86 editor can be
    // a child of a DAW-owned top-level window. Do not pick a random large
    // auxhost window elsewhere on the desktop.
    const HWND foreground = GetForegroundWindow();
    if (foreground) {
        Candidate active{};
        inspectCandidate(foreground, active);
        EnumChildWindows(foreground, inspectChild,
                         reinterpret_cast<LPARAM>(&active));
        if (active.hwnd) return active.hwnd;
    }
    Candidate candidate{};
    EnumWindows(chooseWindow, reinterpret_cast<LPARAM>(&candidate));
    return candidate.hwnd;
}

// The magnification control fills the whole viewer. It is the actual mouse
// hit target, so route its input to our owning window in this process.
WNDPROC originalMagnifierProc = nullptr;

LRESULT CALLBACK magnifierInputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEMOVE:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        MapWindowPoints(hwnd, viewer, &point, 1);
        return SendMessageW(viewer, msg, wp, MAKELPARAM(point.x, point.y));
    }
    default:
        break;
    }
    return CallWindowProcW(originalMagnifierProc, hwnd, msg, wp, lp);
}

void refresh() {
    if (!IsWindow(source)) {
        KillTimer(viewer, refreshTimer);
        MessageBoxW(viewer, L"Das jBridge-Plugin-Fenster wurde geschlossen.",
                    L"125A PluginScaler", MB_OK | MB_ICONINFORMATION);
        DestroyWindow(viewer);
        return;
    }
    if (!GetWindowRect(source, &sourceRect)) return;
    // Magnification mirrors a screen rectangle, NOT an HWND. If another
    // top-level window overlaps that rectangle, it is captured instead.
    // Exclude unrelated top-level windows; preserve the editor's root
    // (which may be the DAW itself for embedded jBridge editors).
    const HWND editorRoot = GetAncestor(source, GA_ROOT);
    struct Filter {
        HWND editorRoot{};
        HWND viewerRoot{};
        RECT sourceBounds{};
        std::vector<HWND> windows;
    } filter{editorRoot, GetAncestor(viewer, GA_ROOT), sourceRect, {}};
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto& f = *reinterpret_cast<Filter*>(value);
        if (!IsWindowVisible(hwnd) || hwnd == f.editorRoot)
            return TRUE;
        RECT bounds{}, overlap{};
        if (GetWindowRect(hwnd, &bounds) &&
            IntersectRect(&overlap, &bounds, &f.sourceBounds))
            f.windows.push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&filter));
    // Include our own viewer explicitly even if the window manager reports
    // it outside the queried source rectangle at initialization.
    if (std::find(filter.windows.begin(), filter.windows.end(),
                  filter.viewerRoot) == filter.windows.end())
        filter.windows.push_back(filter.viewerRoot);
    MagSetWindowFilterList(magnifier, MW_FILTERMODE_EXCLUDE,
                           static_cast<int>(filter.windows.size()),
                           filter.windows.data());
    MagSetWindowSource(magnifier, sourceRect);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        magnifier = CreateWindowW(WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 100, 100, hwnd, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (!magnifier) return -1;
        SetLastError(0);
        originalMagnifierProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(magnifier, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(&magnifierInputProc)));
        if (!originalMagnifierProc && GetLastError() != 0)
            return -1;
        MAGTRANSFORM transform{};
        transform.v[0][0] = transform.v[1][1] = percent / 100.0f;
        transform.v[2][2] = 1.0f;
        if (!MagSetWindowTransform(magnifier, &transform)) return -1;
        HWND exclude[] = { hwnd };
        MagSetWindowFilterList(magnifier, MW_FILTERMODE_EXCLUDE, 1, exclude);
        SetTimer(hwnd, refreshTimer, 30, nullptr);
        return 0;
    }
    case WM_SIZE:
        if (magnifier) MoveWindow(magnifier, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_TIMER:
        refresh();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        dragging = true;
        [[fallthrough]];
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        if (!IsWindow(source)) return 0;
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        POINT mapped{sourceRect.left + MulDiv(x,100,percent),
                     sourceRect.top + MulDiv(y,100,percent)};
        ScreenToClient(source, &mapped);
        const LPARAM coordinates = MAKELPARAM(static_cast<short>(mapped.x),
                                               static_cast<short>(mapped.y));
        WPARAM buttons = wp;
        if (dragging && msg == WM_MOUSEMOVE) buttons |= MK_LBUTTON;
        // Post rather than Send: an unresponsive legacy window must never
        // block this viewer's own UI thread.
        PostMessageW(source, msg, buttons, coordinates);
        if (msg == WM_LBUTTONUP) {
            dragging = false;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, refreshTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    source = locateSource();
    if (!source) {
        MessageBoxW(nullptr, L"Kein sichtbares jBridge-Plugin gefunden. "
                    L"Plugin in Studio One oeffnen und erneut starten.",
                    L"125A PluginScaler", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    if (!MagInitialize()) {
        MessageBoxW(nullptr, L"Windows Magnification API ist nicht verfuegbar.",
                    L"125A PluginScaler", MB_OK | MB_ICONERROR);
        return 3;
    }
    GetWindowRect(source, &sourceRect);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const int width = MulDiv(sourceRect.right-sourceRect.left, percent, 100);
    const int height = MulDiv(sourceRect.bottom-sourceRect.top, percent, 100);
    viewer = CreateWindowW(kClass, L"125A PluginScaler - jBridge 150%",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, width+16, height+39,
                           nullptr, nullptr, instance, nullptr);
    if (!viewer) { MagUninitialize(); return 4; }
    refresh();
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    MagUninitialize();
    return static_cast<int>(m.wParam);
}
