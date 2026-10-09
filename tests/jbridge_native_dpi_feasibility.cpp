#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellscalingapi.h>
#include <cstdio>
#pragma comment(lib,"user32.lib")

LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
    return DefWindowProcW(h,m,w,l);
}
int main() {
    HMODULE user=GetModuleHandleW(L"user32.dll");
    auto setThread=reinterpret_cast<DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(user,"SetThreadDpiAwarenessContext"));
    auto getWindow=reinterpret_cast<DPI_AWARENESS_CONTEXT(WINAPI*)(HWND)>(
        GetProcAddress(user,"GetWindowDpiAwarenessContext"));
    auto getDpi=reinterpret_cast<UINT(WINAPI*)(HWND)>(
        GetProcAddress(user,"GetDpiForWindow"));
    if(!setThread || !getWindow || !getDpi) {
        std::puts("native-dpi-apis=UNAVAILABLE"); return 2;
    }
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=proc;
    wc.lpszClassName=L"125A.DpiFeasibility";
    if(!RegisterClassW(&wc)) return 3;
    struct Mode { DPI_AWARENESS_CONTEXT context; const char* label; };
    Mode modes[]={{DPI_AWARENESS_CONTEXT_UNAWARE,"unaware"},
                  {DPI_AWARENESS_CONTEXT_SYSTEM_AWARE,"system"},
                  {DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2,"per-monitor-v2"}};
    int ok=0;
    for(const auto& mode:modes) {
        auto previous=setThread(mode.context);
        if(!previous) {std::printf("%s=UNAVAILABLE\n",mode.label);continue;}
        HWND h=CreateWindowExW(0,wc.lpszClassName,L"DPI case",
            WS_OVERLAPPEDWINDOW,100,100,400,250,nullptr,nullptr,instance,nullptr);
        if(h) {
            const UINT dpi=getDpi(h);
            const auto actual=getWindow(h);
            BOOL equal=AreDpiAwarenessContextsEqual(actual,mode.context);
            std::printf("%s=PASS dpi=%u context=%d\n",mode.label,dpi,int(equal));
            if(equal)++ok;
            DestroyWindow(h);
        }
        setThread(previous);
    }
    std::printf("native-dpi-precreation=%s\n",ok==3?"PASS":"FAIL");
    std::puts("note=No per-HWND arbitrary DPI value is set by these APIs.");
    return ok==3?0:4;
}
