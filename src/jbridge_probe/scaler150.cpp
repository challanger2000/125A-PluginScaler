#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kScalePercent = 150;
constexpr int kWrapperTolerancePx = 8;
constexpr int kMaxAncestorPropagation = 4;

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring className(HWND hwnd) {
    std::array<wchar_t, 512> b{};
    const int n = GetClassNameW(hwnd, b.data(), static_cast<int>(b.size()));
    return n > 0 ? std::wstring(b.data(), static_cast<std::size_t>(n)) : L"";
}

std::wstring titleText(HWND hwnd) {
    std::array<wchar_t, 1024> b{};
    const int n = GetWindowTextW(hwnd, b.data(), static_cast<int>(b.size()));
    return n > 0 ? std::wstring(b.data(), static_cast<std::size_t>(n)) : L"";
}

std::wstring hwndHex(HWND hwnd) {
    std::wstringstream s;
    s << L"0x" << std::hex << std::uppercase
      << reinterpret_cast<std::uintptr_t>(hwnd);
    return s.str();
}

std::filesystem::path executableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                       static_cast<DWORD>(path.size()));
    if (!n || n >= path.size())
        return std::filesystem::current_path();
    return std::filesystem::path(
        std::wstring(path.data(), static_cast<std::size_t>(n))).parent_path();
}

std::set<DWORD> auxhostPids() {
    std::set<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return pids;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const auto image = lower(pe.szExeFile);
            if (image == L"auxhost.exe" || image == L"auxhost64.exe")
                pids.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

struct Candidate {
    HWND hwnd{};
    DWORD pid{};
    RECT window{};
    RECT client{};
    std::wstring cls;
    std::wstring title;
};

struct EnumContext {
    const std::set<DWORD>* pids{};
    std::vector<Candidate>* candidates{};
};

void maybeCollect(HWND hwnd, EnumContext& ctx) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!ctx.pids->contains(pid) || !IsWindowVisible(hwnd))
        return;

    const auto cls = className(hwnd);
    if (cls.rfind(L"NIVSTChildWindow", 0) != 0)
        return;

    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr))
        return;

    if ((cr.right - cr.left) < 100 || (cr.bottom - cr.top) < 100)
        return;

    ctx.candidates->push_back(
        {hwnd, pid, wr, cr, cls, titleText(hwnd)});
}

BOOL CALLBACK enumChild(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumContext*>(lp);
    if (!ctx)
        return FALSE;
    maybeCollect(hwnd, *ctx);
    return TRUE;
}

BOOL CALLBACK enumTop(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumContext*>(lp);
    if (!ctx)
        return FALSE;
    maybeCollect(hwnd, *ctx);
    EnumChildWindows(hwnd, enumChild, lp);
    return TRUE;
}

long width(const RECT& r) { return r.right - r.left; }
long height(const RECT& r) { return r.bottom - r.top; }

struct WindowSnapshot {
    HWND hwnd{};
    HWND parent{};
    DWORD pid{};
    RECT window{};
    RECT client{};
    POINT childOriginInParent{};
    std::wstring cls;
    std::wstring title;
};

WindowSnapshot snapshot(HWND hwnd) {
    WindowSnapshot s{};
    s.hwnd = hwnd;
    s.parent = GetParent(hwnd);
    GetWindowThreadProcessId(hwnd, &s.pid);
    GetWindowRect(hwnd, &s.window);
    GetClientRect(hwnd, &s.client);
    s.cls = className(hwnd);
    s.title = titleText(hwnd);

    if (s.parent) {
        POINT p{0, 0};
        ClientToScreen(hwnd, &p);
        ScreenToClient(s.parent, &p);
        s.childOriginInParent = p;
    }
    return s;
}

bool isTightWrapper(const WindowSnapshot& child,
                    const WindowSnapshot& parent) {
    const long cw = width(child.window);
    const long ch = height(child.window);
    const long pw = width(parent.client);
    const long ph = height(parent.client);

    const bool nearOrigin =
        std::abs(child.childOriginInParent.x) <= kWrapperTolerancePx &&
        std::abs(child.childOriginInParent.y) <= kWrapperTolerancePx;

    const bool nearSize =
        std::abs(pw - cw) <= kWrapperTolerancePx &&
        std::abs(ph - ch) <= kWrapperTolerancePx;

    return nearOrigin && nearSize;
}

bool resizeWindow(HWND hwnd, int clientTargetW, int clientTargetH) {
    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr))
        return false;

    const int nonClientW = static_cast<int>(width(wr) - width(cr));
    const int nonClientH = static_cast<int>(height(wr) - height(cr));
    const int targetW = (std::max)(1, clientTargetW + nonClientW);
    const int targetH = (std::max)(1, clientTargetH + nonClientH);

    return SetWindowPos(hwnd, nullptr, 0, 0, targetW, targetH,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                        SWP_FRAMECHANGED) != FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto pids = auxhostPids();
    std::vector<Candidate> candidates;
    EnumContext ctx{&pids, &candidates};
    if (!pids.empty())
        EnumWindows(enumTop, reinterpret_cast<LPARAM>(&ctx));

    const auto logPath =
        executableDirectory() / L"125A-jBridge-150-Geometry.txt";
    std::wofstream log(logPath, std::ios::trunc);

    if (log) {
        log << L"125A jBridge 150% Geometry Test\n";
        log << L"scalePercent=" << kScalePercent << L"\n";
        log << L"auxhostCount=" << pids.size() << L"\n";
        log << L"candidateCount=" << candidates.size() << L"\n";
    }

    if (candidates.empty()) {
        MessageBoxW(nullptr,
            L"Kein sichtbares NI-VST-Fenster in jBridge gefunden.\n\n"
            L"FM7 oder Pro-53 ueber jBridge oeffnen und erneut starten.",
            L"125A jBridge 150% Geometry Test",
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return 2;
    }

    auto best = std::max_element(
        candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return width(a.client) * height(a.client) <
                   width(b.client) * height(b.client);
        });

    const HWND plugin = best->hwnd;
    const auto beforePlugin = snapshot(plugin);
    const int nativeW = static_cast<int>(width(beforePlugin.client));
    const int nativeH = static_cast<int>(height(beforePlugin.client));
    const int targetW = MulDiv(nativeW, kScalePercent, 100);
    const int targetH = MulDiv(nativeH, kScalePercent, 100);

    if (log) {
        log << L"plugin hwnd=" << hwndHex(plugin)
            << L" pid=" << beforePlugin.pid
            << L" class=\"" << beforePlugin.cls << L"\""
            << L" nativeClient=" << nativeW << L"x" << nativeH
            << L" targetClient=" << targetW << L"x" << targetH
            << L"\n";
    }

    HWND parent = GetParent(plugin);
    bool parentResized = false;
    if (parent) {
        const auto p = snapshot(parent);
        const int chromeW = static_cast<int>(width(p.client) - nativeW);
        const int chromeH = static_cast<int>(height(p.client) - nativeH);
        const int parentClientW = (std::max)(1, targetW + chromeW);
        const int parentClientH = (std::max)(1, targetH + chromeH);
        parentResized = resizeWindow(parent, parentClientW, parentClientH);

        if (log) {
            log << L"container hwnd=" << hwndHex(parent)
                << L" pid=" << p.pid
                << L" class=\"" << p.cls << L"\""
                << L" client=" << width(p.client) << L"x" << height(p.client)
                << L" chromeDelta=" << chromeW << L"x" << chromeH
                << L" requestedClient=" << parentClientW << L"x" << parentClientH
                << L" resized=" << (parentResized ? 1 : 0)
                << L"\n";
        }
    }

    const bool pluginResized = resizeWindow(plugin, targetW, targetH);

    HWND child = parent ? parent : plugin;
    for (int i = 0; child && i < kMaxAncestorPropagation; ++i) {
        HWND ancestor = GetParent(child);
        if (!ancestor)
            break;

        const auto childBefore = snapshot(child);
        const auto ancestorBefore = snapshot(ancestor);

        if (!isTightWrapper(childBefore, ancestorBefore)) {
            if (log) {
                log << L"ancestor-stop child=" << hwndHex(child)
                    << L" ancestor=" << hwndHex(ancestor)
                    << L" reason=not-tight-wrapper"
                    << L" childWindow=" << width(childBefore.window)
                    << L"x" << height(childBefore.window)
                    << L" ancestorClient=" << width(ancestorBefore.client)
                    << L"x" << height(ancestorBefore.client)
                    << L" childOrigin=(" << childBefore.childOriginInParent.x
                    << L"," << childBefore.childOriginInParent.y << L")\n";
            }
            break;
        }

        RECT childNow{};
        GetWindowRect(child, &childNow);
        const int desiredClientW = static_cast<int>(width(childNow));
        const int desiredClientH = static_cast<int>(height(childNow));
        const bool ok = resizeWindow(ancestor, desiredClientW, desiredClientH);

        if (log) {
            log << L"ancestor-resize hwnd=" << hwndHex(ancestor)
                << L" pid=" << ancestorBefore.pid
                << L" class=\"" << ancestorBefore.cls << L"\""
                << L" requestedClient=" << desiredClientW
                << L"x" << desiredClientH
                << L" resized=" << (ok ? 1 : 0) << L"\n";
        }

        if (!ok)
            break;
        child = ancestor;
    }

    InvalidateRect(plugin, nullptr, FALSE);
    UpdateWindow(plugin);

    const auto afterPlugin = snapshot(plugin);
    const auto afterParent = parent ? snapshot(parent) : WindowSnapshot{};

    if (log) {
        log << L"pluginResized=" << (pluginResized ? 1 : 0) << L"\n";
        log << L"parentResized=" << (parentResized ? 1 : 0) << L"\n";
        log << L"pluginAfterClient=" << width(afterPlugin.client)
            << L"x" << height(afterPlugin.client) << L"\n";
        if (parent) {
            log << L"containerAfterClient=" << width(afterParent.client)
                << L"x" << height(afterParent.client) << L"\n";
        }
    }

    std::wstringstream msg;
    msg << L"Geometrie-Test ausgefuehrt.\n\n"
        << L"Plugin: " << nativeW << L"x" << nativeH
        << L" -> " << targetW << L"x" << targetH << L"\n"
        << L"Ergebnis: " << width(afterPlugin.client)
        << L"x" << height(afterPlugin.client) << L"\n\n"
        << L"Bitte nur ansehen: Ist das jBridge/Plugin-Fenster jetzt groesser?\n"
        << L"Zum Ruecksetzen Plugin-Fenster schliessen und neu oeffnen.\n\n"
        << L"Log: " << logPath.wstring();

    MessageBoxW(nullptr, msg.str().c_str(),
                L"125A jBridge 150% Geometry Test",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return pluginResized ? 0 : 3;
}
