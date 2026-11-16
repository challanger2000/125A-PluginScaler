// Standalone x64 VST2 editor host for real plugins such as DD Reverb.
// No VST2 SDK, no audio processing, and no wrapper around the plugin.
// The loaded plugin remains in this disposable process, not in the DAW.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <iostream>
#include <string>
#include "scaler/vst2_abi.hpp"

using namespace scaler::vst2;
namespace {
constexpr wchar_t kClass[] = L"125A_PluginScaler_VST2_EditorHost";
AEffect* gEffect{};
HINSTANCE gModule{};
HWND gWindow{};
bool gEditorOpened{};
Ptr __cdecl host_callback(AEffect*, Int opcode, Int, Ptr, void*, float) {
    switch(opcode) {
    case kHostVersion: return 2400;
    case kHostGetSampleRate: return 44100;
    case kHostGetBlockSize: return 512;
    default: return 0;
    }
}
LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if(msg==WM_CLOSE) {DestroyWindow(hwnd);return 0;}
    if(msg==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
void cleanup() {
    if(gEffect && gEffect->dispatcher) {
        if(gEditorOpened) gEffect->dispatcher(gEffect,kEffEditClose,0,0,nullptr,0);
        gEffect->dispatcher(gEffect,kEffClose,0,0,nullptr,0);
    }
    gEditorOpened=false;
    gEffect=nullptr;
    if(gModule){FreeLibrary(gModule);gModule=nullptr;}
}
} // namespace
int wmain(int argc,wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(argc!=2) {
        std::wcerr<<L"Aufruf: PluginScalerVST2Host.exe <VST2-x64.dll>\n";
        return 2;
    }
    gModule=LoadLibraryW(argv[1]);
    if(!gModule) {
        std::wcerr<<L"VST2_LOAD_FAIL: DLL konnte nicht geladen werden (Fehler "
                  <<GetLastError()<<L")\n";
        return 3;
    }
    auto entry=reinterpret_cast<Entry>(GetProcAddress(gModule,"VSTPluginMain"));
    if(!entry)entry=reinterpret_cast<Entry>(GetProcAddress(gModule,"main"));
    if(!entry) {
        std::wcerr<<L"VST2_ENTRY_FAIL: kein VSTPluginMain/main\n";
        cleanup();return 4;
    }
    gEffect=entry(&host_callback);
    if(!gEffect||gEffect->magic!=kMagic||!gEffect->dispatcher) {
        std::wcerr<<L"VST2_ABI_FAIL: AEffect ungültig\n";
        // A bad effect pointer must not be dereferenced on cleanup.
        gEffect=nullptr;cleanup();return 5;
    }
    if((gEffect->flags & kHasEditor)==0) {
        std::wcerr<<L"VST2_GUI_FAIL: Plugin meldet keinen Editor\n";
        // For VST2 dispatcher.Close, effOpen must be issued first.
        gEffect->dispatcher(gEffect,kEffOpen,0,0,nullptr,0);
        cleanup();return 6;
    }
    gEffect->dispatcher(gEffect,kEffOpen,0,0,nullptr,0);
    gEffect->dispatcher(gEffect,kEffSetSampleRate,0,0,nullptr,44100.f);
    gEffect->dispatcher(gEffect,kEffSetBlockSize,0,512,nullptr,0);
    ERect* rect=nullptr;
    gEffect->dispatcher(gEffect,kEffEditGetRect,0,0,&rect,0);
    int w=rect?int(rect->right)-rect->left:800;
    int h=rect?int(rect->bottom)-rect->top:500;
    if(w<120||w>4096||h<80||h>4096){w=800;h=500;}
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpfnWndProc=window_proc;
    wc.lpszClassName=kClass;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc)){cleanup();return 7;}
    RECT outer{0,0,w,h};
    AdjustWindowRectEx(&outer,WS_OVERLAPPEDWINDOW,FALSE,0);
    gWindow=CreateWindowExW(0,kClass,L"125A VST2 GUI Testhost",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,50,50,outer.right-outer.left,
        outer.bottom-outer.top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!gWindow){cleanup();return 8;}
    gEffect->dispatcher(gEffect,kEffEditOpen,0,0,gWindow,0);
    gEditorOpened=true;
    ShowWindow(gWindow,SW_SHOW);
    UpdateWindow(gWindow);
    std::wcout<<L"VST2_EDITOR_READY: "<<argv[1]<<L" "<<w<<L"x"<<h<<L"\n"<<std::flush;
    MSG msg{};
    while(IsWindow(gWindow)) {
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT)goto finished;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if(gEffect && gEffect->dispatcher)
            gEffect->dispatcher(gEffect,kEffEditIdle,0,0,nullptr,0);
        Sleep(30);
    }
finished:
    cleanup();
    return 0;
}
