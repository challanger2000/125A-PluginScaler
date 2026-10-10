#include "smoke_shared.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int childProcess() {
    wchar_t name[192]{};
    if (!GetEnvironmentVariableW(kHookSmokeEnvironment, name, 192))
        return 11;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 12;
    auto* state = static_cast<HookSmokeState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookSmokeState)));
    if (!state) { CloseHandle(mapping); return 13; }
    // Establish this target thread's Windows message queue before hooking.
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    InterlockedExchange(&state->ready, 1);
    bool signaled = false;
    for (int i = 0; i < 800; ++i) {
        if (InterlockedCompareExchange(&state->proceed, 0, 0)) {
            signaled = true;
            break;
        }
        Sleep(10);
    }
    if (!signaled) {
        UnmapViewOfFile(state); CloseHandle(mapping);
        return 14;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpfnWndProc = editorProc;
    wc.lpszClassName = L"125A.HookNativeMockEditor";
    if (!RegisterClassW(&wc)) {
        UnmapViewOfFile(state); CloseHandle(mapping);
        return 15;
    }
    HWND editor = CreateWindowExW(0, wc.lpszClassName, L"Legacy Editor",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 410, 280,
        nullptr, nullptr, instance, nullptr);
    if (!editor) {
        UnmapViewOfFile(state); CloseHandle(mapping);
        return 16;
    }
    InterlockedExchange(&state->childHwnd,
         static_cast<LONG>(reinterpret_cast<std::uintptr_t>(editor)));
    ShowWindow(editor, SW_SHOWNOACTIVATE);
    UpdateWindow(editor);
    for (int i = 0; i < 40; ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    DestroyWindow(editor);
    UnmapViewOfFile(state); CloseHandle(mapping);
    return 0;
}
struct Instance {
    HANDLE mapping{};
    HookSmokeState* state{};
    PROCESS_INFORMATION proc{};
    HHOOK hook{};
    std::wstring name;
};
void cleanup(Instance& x) {
    if (x.hook) UnhookWindowsHookEx(x.hook);
    if (x.proc.hThread) CloseHandle(x.proc.hThread);
    if (x.proc.hProcess) {
        if (WaitForSingleObject(x.proc.hProcess, 0) == WAIT_TIMEOUT) {
            TerminateProcess(x.proc.hProcess, 101);
            WaitForSingleObject(x.proc.hProcess, 5000);
        }
        CloseHandle(x.proc.hProcess);
    }
    if (x.state) UnmapViewOfFile(x.state);
    if (x.mapping) CloseHandle(x.mapping);
}
int controllerProcess() {
    wchar_t file[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, file, MAX_PATH)) return 2;
    std::wstring dir = file;
    dir.erase(dir.find_last_of(L"\\/") + 1);
    const std::wstring library = dir + L"PluginScalerJBridgeHook-x86.dll";
    HMODULE dll = LoadLibraryW(library.c_str());
    if (!dll) { std::printf("hook-dll-load=FAIL (%lu)\n", GetLastError()); return 3; }
    auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "HookProc"));
    if (!procedure) procedure =
       reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "_HookProc@12"));
    if (!procedure) {
        std::printf("hook-export=FAIL (%lu)\n", GetLastError());
        FreeLibrary(dll); return 4;
    }
    std::array<Instance, 2> instances{};
    bool okay = true;
    for (std::size_t i = 0; i < instances.size(); ++i) {
        auto& x = instances[i];
        wchar_t name[192]{};
        swprintf_s(name, L"Local\\125A_HookSmoke_%lu_%u",
                   GetCurrentProcessId(), static_cast<unsigned>(i));
        x.name = name;
        x.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                           PAGE_READWRITE, 0, sizeof(HookSmokeState), name);
        if (!x.mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
            okay = false; break;
        }
        x.state = static_cast<HookSmokeState*>(MapViewOfFile(
            x.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookSmokeState)));
        if (!x.state) { okay=false; break; }
        ZeroMemory(x.state, sizeof(HookSmokeState));
        SetEnvironmentVariableW(kHookSmokeEnvironment, name);
        std::wstring line = L"\"" + std::wstring(file) + L"\" --child";
        std::vector<wchar_t> args(line.begin(), line.end());
        args.push_back(0);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        if (!CreateProcessW(file, args.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &x.proc)) {
            std::printf("hook-child-create=FAIL (%lu)\n", GetLastError());
            okay=false; break;
        }
    }
    SetEnvironmentVariableW(kHookSmokeEnvironment, nullptr);
    if (okay) {
        for (auto& x : instances) {
            bool ready = false;
            for (int attempt=0; attempt<500; ++attempt) {
                if (InterlockedCompareExchange(&x.state->ready, 0, 0) != 0) {
                    ready=true; break;
                }
                if (WaitForSingleObject(x.proc.hProcess, 0) == WAIT_OBJECT_0)
                    break;
                Sleep(10);
            }
            if (!ready) { std::puts("hook-child-ready=FAIL"); okay=false; break; }
            x.hook = SetWindowsHookExW(WH_CBT, procedure, dll,
                                      x.proc.dwThreadId);
            if (!x.hook) {
                std::printf("hook-install=FAIL (%lu)\n", GetLastError());
                okay=false; break;
            }
        }
    }
    if (okay) {
        for (auto& x:instances)
            InterlockedExchange(&x.state->proceed,1);
        for (auto& x:instances) {
            DWORD wait = WaitForSingleObject(x.proc.hProcess, 12000);
            DWORD code=100;
            if (wait==WAIT_OBJECT_0) GetExitCodeProcess(x.proc.hProcess,&code);
            bool reached = wait==WAIT_OBJECT_0 && code==0 &&
                x.state->created>0 &&
                x.state->matchedCount>0 &&
                static_cast<DWORD>(x.state->matchedPid)==x.proc.dwProcessId &&
                static_cast<DWORD>(x.state->matchedTid)==x.proc.dwThreadId &&
                x.state->matchedHwnd==x.state->childHwnd;
            std::printf("hook-matched-editor=%s pid=%lu callbackPid=%ld events=%ld matched=%ld hwnd=%ld expected=%ld\n",
                reached?"PASS":"FAIL",x.proc.dwProcessId,
                x.state->matchedPid,x.state->created,x.state->matchedCount,
                x.state->matchedHwnd,x.state->childHwnd);
            okay &= reached;
        }
    }
    for (auto& x:instances) cleanup(x);
    FreeLibrary(dll);
    std::printf("two-independent-32bit-hooked-editors=%s\n",
                 okay?"PASS":"FAIL");
    std::puts("note=Hook proof only; no plugin content scaling is claimed.");
    return okay?0:5;
}
int wmain(int argc, wchar_t**) {
    if (argc==2) return childProcess();
    return controllerProcess();
}
