#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>
#include <cstring>

namespace fs = std::filesystem;

void notice(const std::wstring& msg, UINT type = MB_ICONERROR) {
    MessageBoxW(nullptr, msg.c_str(), L"125A PluginScaler Setup", MB_OK | type);
}

std::wstring chooseDll() {
    wchar_t path[32768] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    ofn.lpstrFilter = L"VST2-DLL (*.dll)\0*.dll\0\0";
    ofn.lpstrTitle = L"Originale 32-Bit-VST2-DLL auswaehlen";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? path : L"";
}

std::wstring chooseFolder() {
    BROWSEINFOW browse{};
    browse.lpszTitle = L"64-Bit-VST2-Pluginordner waehlen";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&browse);
    if (!id) return L"";
    wchar_t output[MAX_PATH] = {};
    const BOOL success = SHGetPathFromIDListW(id, output);
    CoTaskMemFree(id);
    return success ? output : L"";
}

std::wstring quote(const fs::path& p) {
    return L"\"" + p.wstring() + L"\"";
}

bool generateManifest(const fs::path& helper, const fs::path& target,
                      const fs::path& manifest) {
    const std::wstring command = quote(helper) + L" --write-vst2-manifest " +
                                 quote(target) + L" " + quote(manifest);
    std::vector<wchar_t> args(command.begin(), command.end());
    args.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helper.c_str(), args.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr,
                        helper.parent_path().c_str(), &si, &pi))
        return false;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 30000);
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    const bool ok = wait == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(pi.hProcess, &code) &&
                    code == 0 && fs::exists(manifest);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

bool saveIni(const fs::path& path, const std::wstring& ini) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) != 0 &&
              written == sizeof(bom);
    if (ok) {
        const DWORD bytes = static_cast<DWORD>(ini.size() * sizeof(wchar_t));
        ok = WriteFile(h, ini.data(), bytes, &written, nullptr) != 0 &&
             written == bytes;
    }
    CloseHandle(h);
    if (!ok) fs::remove(path);
    return ok;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        wchar_t binaryPath[32768] = {};
        if (!GetModuleFileNameW(nullptr, binaryPath,
                                static_cast<DWORD>(sizeof(binaryPath) / sizeof(binaryPath[0])))) {
            notice(L"Setup-Pfad konnte nicht gelesen werden.");
            return 1;
        }
        const fs::path self = binaryPath;
        // Packed executable layout: EXE | x64 proxy | x86 helper |
        // 2x uint64 little-endian lengths | 16-byte magic identifier.
        constexpr char marker[16] = {'1','2','5','A','_','S','C','A','L','E','R','_','P','K','G','1'};
        HANDLE input = CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE) {
            notice(L"Die Setup-EXE konnte nicht gelesen werden.");
            return 1;
        }
        LARGE_INTEGER total{};
        GetFileSizeEx(input, &total);
        struct Footer { unsigned long long proxyBytes; unsigned long long helperBytes; char magic[16]; } footer{};
        LARGE_INTEGER pos{};
        pos.QuadPart = total.QuadPart - static_cast<LONGLONG>(sizeof(Footer));
        DWORD read = 0;
        const bool validFooter = pos.QuadPart > 0 &&
            SetFilePointerEx(input, pos, nullptr, FILE_BEGIN) &&
            ReadFile(input, &footer, sizeof(footer), &read, nullptr) &&
            read == sizeof(footer) &&
            memcmp(footer.magic, marker, sizeof(marker)) == 0 &&
            footer.proxyBytes > 0 && footer.helperBytes > 0 &&
            footer.proxyBytes + footer.helperBytes < static_cast<unsigned long long>(pos.QuadPart);
        if (!validFooter) {
            CloseHandle(input);
            notice(L"Setup-Datei enthaelt keine gueltigen eingebetteten Komponenten.");
            return 1;
        }
        auto extract = [&](const fs::path& dest, unsigned long long offset,
                           unsigned long long bytes) -> bool {
            if (bytes > 100ULL * 1024 * 1024) return false;
            LARGE_INTEGER start{};
            start.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(input, start, nullptr, FILE_BEGIN)) return false;
            HANDLE out = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) return false;
            bool ok = true;
            char buffer[65536];
            while (bytes > 0 && ok) {
                const DWORD count = static_cast<DWORD>((std::min<unsigned long long>)(bytes, sizeof(buffer)));
                DWORD got = 0, wrote = 0;
                ok = ReadFile(input, buffer, count, &got, nullptr) && got == count &&
                     WriteFile(out, buffer, got, &wrote, nullptr) && wrote == got;
                bytes -= count;
            }
            CloseHandle(out);
            if (!ok) DeleteFileW(dest.c_str());
            return ok;
        };
        const std::wstring selected = chooseDll();
        if (selected.empty()) return 0;
        const fs::path target = fs::absolute(selected);
        std::wstring extension = target.extension().wstring();
        for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
        if (extension != L".dll") {
            notice(L"Bitte eine DLL auswaehlen.");
            return 1;
        }
        const std::wstring folder = chooseFolder();
        if (folder.empty()) return 0;
        const std::wstring name = target.stem().wstring() + L"-125A-150";
        const fs::path dir = fs::path(folder) / name;
        if (fs::exists(dir)) {
            notice(L"Der Testordner existiert schon. Es wurde nichts ueberschrieben.");
            return 1;
        }
        fs::create_directories(dir);
        const fs::path localHelper = dir / L"PluginScalerHelper-x86.exe";
        const fs::path dll = dir / (name + L".dll");
        const fs::path manifest = dir / (name + L".pluginscaler.txt");
        const fs::path iniPath = dir / (name + L".pluginscaler.ini");
        const unsigned long long payloadStart = static_cast<unsigned long long>(pos.QuadPart) - footer.helperBytes - footer.proxyBytes;
        const bool unpacked = extract(dll, payloadStart, footer.proxyBytes) &&
                              extract(localHelper, payloadStart + footer.proxyBytes, footer.helperBytes);
        CloseHandle(input);
        if (!unpacked) {
            fs::remove_all(dir);
            notice(L"Die eingebetteten Komponenten konnten nicht entpackt werden.");
            return 1;
        }
        if (!generateManifest(localHelper, target, manifest)) {
            fs::remove_all(dir);
            notice(L"Manifest-Erstellung gescheitert. Pruefe, ob das Ziel ein 32-Bit-VST2-Plugin ist.");
            return 1;
        }
        const std::wstring ini = L"[PluginScaler]\r\nhelper=PluginScalerHelper-x86.exe\r\ntarget=" +
            target.wstring() + L"\r\nmanifest=" + manifest.filename().wstring() +
            L"\r\nscale=150\r\neditor=gdi\r\n";
        if (!saveIni(iniPath, ini)) {
            fs::remove_all(dir);
            notice(L"Konfiguration konnte nicht gespeichert werden.");
            return 1;
        }
        notice(L"150%-Testwrapper wurde erstellt:\n" + dll.wstring() +
               L"\n\nJetzt Plugins in Studio One neu scannen.", MB_ICONINFORMATION);
        return 0;
    } catch (const fs::filesystem_error&) {
        notice(L"Dateifehler: Pruefe die Berechtigungen und den Zielordner.");
        return 1;
    }
}
