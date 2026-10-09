#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <magnification.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <set>
#include <string>

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
BOOL CALLBACK chooseWindow(HWND hwnd, LPARAM param) {
    auto& c = *reinterpret_cast<Candidate*>(param);
    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return TRUE;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (!jbridgePid(pid)) return TRUE;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    const long w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 120 || h < 90 || w > 3000 || h > 2000) return TRUE;
    if (w * h > c.area) c = {hwnd, w * h};
    return TRUE;
}

HWND locateSource() {
    HWND foreground = GetForegroundWindow();
    for (HWND w = foreground; w; w = GetParent(w)) {
        DWORD pid{};
        GetWindowThreadProcessId(w, &pid);
        if (jbridgePid(pid) && IsWindowVisible(w)) return w;
    }
    Candidate candidate{};
    EnumWindows(chooseWindow, reinterpret_cast<LPARAM>(&candidate));
    return candidate.hwnd;
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
    // Keep the mirror separate from the native editor: do not reparent it,
    // change its DPI awareness or rewrite third-party code.
    MagSetWindowSource(magnifier, sourceRect);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        magnifier = CreateWindowW(WC_MAGNIFIER, L"", WS_CHILD | WS_VISIBLE,
                                  0, 0, 100, 100, hwnd, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        if (!magnifier) return -1;
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
