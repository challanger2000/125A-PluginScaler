#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ProcessInfo {
    DWORD pid{};
    std::wstring image;
};

struct WindowInfo {
    HWND hwnd{};
    DWORD pid{};
    DWORD tid{};
    HWND parent{};
    HWND owner{};
    HWND root{};
    DWORD rootPid{};
    std::wstring className;
    std::wstring title;
    RECT windowRect{};
    RECT clientRect{};
    LONG_PTR style{};
    LONG_PTR exStyle{};
    bool visible{};
    bool enabled{};
    int depth{};
};

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring windowText(HWND hwnd) {
    std::array<wchar_t, 1024> buffer{};
    const int len = GetWindowTextW(hwnd, buffer.data(),
                                   static_cast<int>(buffer.size()));
    return len > 0 ? std::wstring(buffer.data(), static_cast<std::size_t>(len))
                   : std::wstring{};
}

std::wstring windowClass(HWND hwnd) {
    std::array<wchar_t, 512> buffer{};
    const int len = GetClassNameW(hwnd, buffer.data(),
                                  static_cast<int>(buffer.size()));
    return len > 0 ? std::wstring(buffer.data(), static_cast<std::size_t>(len))
                   : std::wstring{};
}

std::vector<ProcessInfo> findJBridgeProcesses() {
    std::vector<ProcessInfo> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring image = entry.szExeFile;
            const auto normalized = lower(image);
            if (normalized == L"auxhost.exe" ||
                normalized == L"auxhost64.exe") {
                result.push_back({entry.th32ProcessID, image});
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

struct EnumContext {
    const std::set<DWORD>* targetPids{};
    std::vector<WindowInfo>* windows{};
};

int windowDepth(HWND hwnd) {
    int depth = 0;
    HWND current = hwnd;
    while ((current = GetParent(current)) != nullptr && depth < 64)
        ++depth;
    return depth;
}

void collectWindow(HWND hwnd, EnumContext& ctx) {
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || !ctx.targetPids->contains(pid))
        return;

    WindowInfo info{};
    info.hwnd = hwnd;
    info.pid = pid;
    info.tid = tid;
    info.parent = GetParent(hwnd);
    info.owner = GetWindow(hwnd, GW_OWNER);
    info.root = GetAncestor(hwnd, GA_ROOT);
    if (info.root)
        GetWindowThreadProcessId(info.root, &info.rootPid);
    info.className = windowClass(hwnd);
    info.title = windowText(hwnd);
    GetWindowRect(hwnd, &info.windowRect);
    GetClientRect(hwnd, &info.clientRect);
    info.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    info.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    info.visible = IsWindowVisible(hwnd) != FALSE;
    info.enabled = IsWindowEnabled(hwnd) != FALSE;
    info.depth = windowDepth(hwnd);
    ctx.windows->push_back(std::move(info));
}

BOOL CALLBACK enumChildProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!ctx)
        return FALSE;
    collectWindow(hwnd, *ctx);
    return TRUE;
}

BOOL CALLBACK enumTopProc(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (!ctx)
        return FALSE;

    collectWindow(hwnd, *ctx);
    EnumChildWindows(hwnd, enumChildProc, param);
    return TRUE;
}

std::filesystem::path executableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD len = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!len || len >= path.size())
        return std::filesystem::current_path();
    return std::filesystem::path(
        std::wstring(path.data(), static_cast<std::size_t>(len))).parent_path();
}

std::wstring hwndHex(HWND hwnd) {
    std::wstringstream out;
    out << L"0x" << std::hex << std::uppercase
        << reinterpret_cast<std::uintptr_t>(hwnd);
    return out.str();
}

long rectWidth(const RECT& r) { return r.right - r.left; }
long rectHeight(const RECT& r) { return r.bottom - r.top; }

std::wstring processNameFor(DWORD pid, const std::vector<ProcessInfo>& processes) {
    for (const auto& p : processes)
        if (p.pid == pid)
            return p.image;
    return L"";
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto processes = findJBridgeProcesses();
    std::set<DWORD> targetPids;
    for (const auto& p : processes)
        targetPids.insert(p.pid);

    std::vector<WindowInfo> windows;
    EnumContext ctx{&targetPids, &windows};

    if (!targetPids.empty())
        EnumWindows(enumTopProc, reinterpret_cast<LPARAM>(&ctx));

    std::sort(windows.begin(), windows.end(),
              [](const WindowInfo& a, const WindowInfo& b) {
                  if (a.pid != b.pid) return a.pid < b.pid;
                  if (a.depth != b.depth) return a.depth < b.depth;
                  return reinterpret_cast<std::uintptr_t>(a.hwnd) <
                         reinterpret_cast<std::uintptr_t>(b.hwnd);
              });

    const auto logPath =
        executableDirectory() / L"125A-jBridge-WindowProbe.txt";
    std::wofstream out(logPath, std::ios::trunc);

    if (out) {
        out << L"125A jBridge GUI Window Probe\n";
        out << L"purpose=measure window ownership/topology before scaler implementation\n";
        out << L"jbridgeProcessCount=" << processes.size() << L"\n";
        out << L"matchingWindowCount=" << windows.size() << L"\n\n";

        for (const auto& p : processes)
            out << L"PROCESS pid=" << p.pid << L" image=" << p.image << L"\n";

        out << L"\n";
        for (const auto& w : windows) {
            const bool topLevel = w.parent == nullptr;
            const bool rootSameProcess = w.rootPid == w.pid;
            const bool hasArea =
                rectWidth(w.clientRect) > 0 && rectHeight(w.clientRect) > 0;

            out << L"WINDOW"
                << L" hwnd=" << hwndHex(w.hwnd)
                << L" pid=" << w.pid
                << L" process=" << processNameFor(w.pid, processes)
                << L" tid=" << w.tid
                << L" depth=" << w.depth
                << L" parent=" << hwndHex(w.parent)
                << L" owner=" << hwndHex(w.owner)
                << L" root=" << hwndHex(w.root)
                << L" rootPid=" << w.rootPid
                << L" topLevel=" << (topLevel ? 1 : 0)
                << L" rootSameProcess=" << (rootSameProcess ? 1 : 0)
                << L" visible=" << (w.visible ? 1 : 0)
                << L" enabled=" << (w.enabled ? 1 : 0)
                << L" window=" << rectWidth(w.windowRect)
                << L"x" << rectHeight(w.windowRect)
                << L" client=" << rectWidth(w.clientRect)
                << L"x" << rectHeight(w.clientRect)
                << L" style=0x" << std::hex
                << static_cast<std::uintptr_t>(w.style)
                << L" exStyle=0x"
                << static_cast<std::uintptr_t>(w.exStyle)
                << std::dec
                << L" candidate=" << ((w.visible && hasArea) ? 1 : 0)
                << L" class=\"" << w.className << L"\""
                << L" title=\"" << w.title << L"\""
                << L"\n";
        }
    }

    std::wstringstream message;
    if (processes.empty()) {
        message
            << L"Kein laufender jBridge auxhost gefunden.\n\n"
            << L"FM7 oder Pro-53 ueber jBridge oeffnen und dieses EXE erneut starten.";
    } else {
        message
            << L"jBridge-Prozesse: " << processes.size() << L"\n"
            << L"Gefundene Fenster: " << windows.size() << L"\n\n"
            << L"Diagnose gespeichert:\n"
            << logPath.wstring();
    }

    MessageBoxW(nullptr, message.str().c_str(),
                L"125A jBridge Window Probe",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);

    return 0;
}
