#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Test-only interprocess diagnostics. Never used for a production plug-in.
struct HookSmokeState {
    volatile LONG ready;
    volatile LONG proceed;
    volatile LONG created;
    volatile LONG lastPid;
    volatile LONG lastTid;
    volatile LONG lastHwnd;
    volatile LONG childHwnd;
};
constexpr wchar_t kHookSmokeEnvironment[] = L"125A_HOOK_SMOKE_MAPPING";
