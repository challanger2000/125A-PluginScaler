#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <cstdlib>
#include <tlhelp32.h>
#include <sstream>
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
DWORD sourcePid{};
DWORD sourceThread{};
RECT sourceRect{};
int percent = 150;
bool dragging = false;
HWND mouseTarget{};
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

struct Candidate { HWND hwnd{}; DWORD pid{}; RECT rect{}; std::wstring title; };
struct Scan { std::vector<Candidate> candidates; };
void consider(HWND hwnd, Scan& scan) {
    if (!IsWindowVisible(hwnd) || !IsWindowEnabled(hwnd)) return;
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (!jbridgePid(pid)) return;
    RECT r{}, client{};
    if (!GetWindowRect(hwnd, &r) || !GetClientRect(hwnd, &client)) return;
    const long width = r.right-r.left, height = r.bottom-r.top;
    if (width < 180 || height < 100 || width > 3000 || height > 2000 ||
        client.right < 150 || client.bottom < 80) return;
    if (std::find_if(scan.candidates.begin(), scan.candidates.end(),
        [hwnd](const Candidate& c) { return c.hwnd == hwnd; }) != scan.candidates.end())
        return;
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    scan.candidates.push_back({hwnd,pid,r,title});
}
BOOL CALLBACK enumChild(HWND hwnd, LPARAM p) {
    consider(hwnd,*reinterpret_cast<Scan*>(p));
    return TRUE;
}
BOOL CALLBACK enumTop(HWND hwnd, LPARAM p) {
    auto& scan=*reinterpret_cast<Scan*>(p);
    consider(hwnd,scan);
    EnumChildWindows(hwnd,enumChild,p);
    return TRUE;
}
HWND locateSource() {
    Scan scan{};
    EnumWindows(enumTop,reinterpret_cast<LPARAM>(&scan));
    if (scan.candidates.empty()) return nullptr;
    const HWND foreground=GetForegroundWindow();
    if (foreground) {
        auto it=std::find_if(scan.candidates.begin(),scan.candidates.end(),
            [foreground](const Candidate& c) {
                return c.hwnd==foreground || IsChild(foreground,c.hwnd);
            });
        if (it!=scan.candidates.end())
            std::rotate(scan.candidates.begin(),it,it+1);
    }
    if (scan.candidates.size()==1) return scan.candidates.front().hwnd;

    // Do not guess which editor is meant when several jBridge windows exist.
    std::vector<std::wstring> labels;
    for (auto& c:scan.candidates) {
        std::wstringstream ss;
        ss << (c.title.empty()?L"(ohne Titel)":c.title)
           << L" (PID " << c.pid << L", "
           << c.rect.right-c.rect.left << L"x"
           << c.rect.bottom-c.rect.top << L")";
        labels.push_back(ss.str());
    }
    // Standard USER32 dialog: compatible with Windows systems that do not
    // export TaskDialogIndirect from ComCtl32 (ordinal 345).
    for (std::size_t i = 0; i < labels.size(); ++i) {
        std::wstring question = L"Dieses Plugin skalieren?\n\n";
        question += labels[i];
        question += L"\n\nJa = auswaehlen, Nein = naechstes, Abbrechen = beenden.";
        const int answer = MessageBoxW(nullptr, question.c_str(),
            L"125A PluginScaler - Pluginauswahl",
            MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST);
        if (answer == IDYES) return scan.candidates[i].hwnd;
        if (answer == IDCANCEL) return nullptr;
    }
    return nullptr;
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
    DWORD currentPid{};
    const DWORD currentThread = IsWindow(source) ?
        GetWindowThreadProcessId(source, &currentPid) : 0;
    if (!currentThread || currentPid != sourcePid ||
        currentThread != sourceThread) {
        KillTimer(viewer, refreshTimer);
        MessageBoxW(viewer, L"Das jBridge-Plugin-Fenster wurde geschlossen.",
                    L"125A PluginScaler", MB_OK | MB_ICONINFORMATION);
        DestroyWindow(viewer);
        return;
    }
    RECT client{};
    if (!GetClientRect(source, &client)) return;
    POINT topLeft{client.left,client.top}, bottomRight{client.right,client.bottom};
    if (!ClientToScreen(source,&topLeft) || !ClientToScreen(source,&bottomRight)) return;
    sourceRect={topLeft.x,topLeft.y,bottomRight.x,bottomRight.y};
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

// Route to the deepest enabled editor child that really occupies the point.
// Keep the same recipient throughout a drag even when the pointer moves.
HWND hitTestEditor(POINT screen) {
    HWND current = source;
    for (int depth = 0; depth < 32; ++depth) {
        POINT local = screen;
        if (!ScreenToClient(current, &local)) break;
        HWND child = RealChildWindowFromPoint(current, local);
        if (!child || child == current || !IsWindow(child)) break;
        current = child;
    }
    return current;
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
    case WM_SIZE: {
        if (!magnifier) return 0;
        const int clientWidth = LOWORD(lp);
        const int clientHeight = HIWORD(lp);
        // A resized viewer must not map clicks against an obsolete fixed
        // 150% transform: preserve the source aspect ratio and rescale the
        // magnifier's presentation rather than stretching arbitrarily.
        const int naturalWidth = sourceRect.right-sourceRect.left;
        const int naturalHeight = sourceRect.bottom-sourceRect.top;
        if (naturalWidth <= 0 || naturalHeight <= 0) return 0;
        const int fitW = MulDiv(naturalWidth,percent,100);
        const int fitH = MulDiv(naturalHeight,percent,100);
        const int x = (clientWidth-fitW)/2;
        const int y = (clientHeight-fitH)/2;
        MoveWindow(magnifier,x,y,fitW,fitH,TRUE);
        return 0;
    }
    case WM_TIMER:
        refresh();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        if (!IsWindow(source)) return 0;
        POINT origin{0,0};
        if (!magnifier || !IsWindow(magnifier) ||
            !ClientToScreen(magnifier, &origin) ||
            !ScreenToClient(hwnd, &origin)) return 0;
        const int x = GET_X_LPARAM(lp)-origin.x;
        const int y = GET_Y_LPARAM(lp)-origin.y;
        RECT surface{};
        GetClientRect(magnifier,&surface);
        if (!dragging && (x < 0 || y < 0 ||
             x >= surface.right || y >= surface.bottom)) return 0;
        POINT screen{sourceRect.left + MulDiv(x,100,percent),
                     sourceRect.top + MulDiv(y,100,percent)};
        if (msg == WM_LBUTTONDOWN) {
            mouseTarget = hitTestEditor(screen);
            SetCapture(hwnd);
            dragging = true;
        }
        HWND target = dragging && IsWindow(mouseTarget) ? mouseTarget
                        : hitTestEditor(screen);
        if (!IsWindow(target)) return 0;
        POINT mapped = screen;
        if (!ScreenToClient(target, &mapped)) return 0;
        const LPARAM coordinates = MAKELPARAM(static_cast<short>(mapped.x),
                                               static_cast<short>(mapped.y));
        WPARAM buttons = wp;
        if (dragging && msg == WM_MOUSEMOVE) buttons |= MK_LBUTTON;
        // Avoid blocking the GUI if an old plugin stops responding.
        PostMessageW(target, msg, buttons, coordinates);
        if (msg == WM_LBUTTONUP) {
            dragging = false;
            mouseTarget = nullptr;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        dragging = false;
        mouseTarget = nullptr;
        return 0;
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
    // Test-only explicit HWND binding, restricted to the bundled mock class.
    // Production execution always discovers jBridge editors normally.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && wcscmp(argv[1], L"--mock-hwnd") == 0) {
        wchar_t* end = nullptr;
        const unsigned long long number = wcstoull(argv[2], &end, 10);
        HWND candidate = reinterpret_cast<HWND>(static_cast<UINT_PTR>(number));
        wchar_t cls[128]{};
        if (end && !*end && IsWindow(candidate) &&
            GetClassNameW(candidate, cls, 128) &&
            wcscmp(cls, L"125A.MockLegacyEditor") == 0)
            source = candidate;
    } else {
        source = locateSource();
    }
    if (argv) LocalFree(argv);
    if (source)
        sourceThread = GetWindowThreadProcessId(source, &sourcePid);
    if (!source || !sourceThread) {
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
    RECT client{};
    GetClientRect(source, &client);
    POINT topLeft{client.left,client.top}, bottomRight{client.right,client.bottom};
    ClientToScreen(source,&topLeft);
    ClientToScreen(source,&bottomRight);
    sourceRect={topLeft.x,topLeft.y,bottomRight.x,bottomRight.y};
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const int width = MulDiv(sourceRect.right-sourceRect.left, percent, 100);
    const int height = MulDiv(sourceRect.bottom-sourceRect.top, percent, 100);
    RECT outside{0,0,width,height};
    AdjustWindowRectEx(&outside, WS_OVERLAPPEDWINDOW, FALSE, 0);
    viewer = CreateWindowW(kClass, L"125A PluginScaler - jBridge 150%",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           outside.right-outside.left, outside.bottom-outside.top,
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
