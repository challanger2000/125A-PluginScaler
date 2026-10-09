#include "smoke_shared.h"
#include <cstdint>
#include <iterator>

extern "C" __declspec(dllexport)
LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_CREATEWND) {
        wchar_t name[192]{};
        const DWORD len = GetEnvironmentVariableW(
            kHookSmokeEnvironment, name, static_cast<DWORD>(std::size(name)));
        if (len && len < std::size(name)) {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
            if (mapping) {
                auto* state = static_cast<HookSmokeState*>(
                    MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0,
                                  sizeof(HookSmokeState)));
                if (state) {
                    InterlockedExchange(&state->lastPid,
                                        static_cast<LONG>(GetCurrentProcessId()));
                    InterlockedExchange(&state->lastTid,
                                        static_cast<LONG>(GetCurrentThreadId()));
                    InterlockedExchange(&state->lastHwnd,
                         static_cast<LONG>(reinterpret_cast<std::uintptr_t>(
                                                  reinterpret_cast<HWND>(wp))));
                    InterlockedIncrement(&state->created);
                    UnmapViewOfFile(state);
                }
                CloseHandle(mapping);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
