#include "native_scale_shared.h"
#include <windowsx.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
NativeScaleState* state{};
void pump(int duration) {
    const DWORD until=GetTickCount()+duration;
    while(static_cast<LONG>(until-GetTickCount())>0) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}
LRESULT CALLBACK originalEditor(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(hwnd,&ps);
        if(dc) {
            HBRUSH bg=CreateSolidBrush(RGB(10,12,16));
            HBRUSH bright=CreateSolidBrush(RGB(25,190,100));
            SelectObject(dc,GetStockObject(NULL_PEN));
            SelectObject(dc,bg);Rectangle(dc,0,0,kLogicalWidth,kLogicalHeight);
            SelectObject(dc,bright);Rectangle(dc,48,30,80,66);
            DeleteObject(bg);DeleteObject(bright);
            EndPaint(hwnd,&ps);
        }
        return 0;
    }
    if(msg==WM_LBUTTONDOWN||msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP) {
        const int logicalX=GET_X_LPARAM(lp),logicalY=GET_Y_LPARAM(lp);
        const DWORD messagePosition=GetMessagePos();
        POINT physical{GET_X_LPARAM(static_cast<LPARAM>(messagePosition)),
                       GET_Y_LPARAM(static_cast<LPARAM>(messagePosition))};
        if(ScreenToClient(hwnd,&physical)) {
            const LONG scale=InterlockedCompareExchange(&state->scale,0,0);
            const int expectedX=MulDiv(physical.x,100,scale);
            const int expectedY=MulDiv(physical.y,100,scale);
            if(std::abs(expectedX-logicalX)>1 ||
               std::abs(expectedY-logicalY)>1)
                InterlockedIncrement(&state->mismatch);
        }
        InterlockedExchange(&state->originalLogicalX,logicalX);
        InterlockedExchange(&state->originalLogicalY,logicalY);
        if(msg==WM_LBUTTONDOWN) {
            InterlockedIncrement(&state->mouseDown);
            SetCapture(hwnd);
        } else if(msg==WM_MOUSEMOVE && (wp&MK_LBUTTON)) {
            InterlockedIncrement(&state->mouseMove);
        } else if(msg==WM_LBUTTONUP) {
            InterlockedIncrement(&state->mouseUp);
            if(GetCapture()==hwnd)ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int child() {
    wchar_t name[192]{};
    if(!GetEnvironmentVariableW(kNativeScaleMapName,name,192))return 11;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping)return 12;
    state=static_cast<NativeScaleState*>(
       MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeScaleState)));
    if(!state){CloseHandle(mapping);return 13;}
    MSG msg{};
    PeekMessageW(&msg,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
    InterlockedExchange(&state->ready,1);
    bool proceed=false;
    for(int i=0;i<700;++i) {
        if(InterlockedCompareExchange(&state->proceed,0,0)) {
            proceed=true;break;
        }
        Sleep(10);
    }
    if(!proceed)return 14;
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpfnWndProc=originalEditor;
    wc.lpszClassName=kNativeScaleEditorClass;
    if(!RegisterClassW(&wc))return 15;
    // The original renderer is completely unaware of any scaling.
    HWND hwnd=CreateWindowExW(0,kNativeScaleEditorClass,L"Unscaled original",
       WS_POPUP|WS_VISIBLE,GetCurrentProcessId()%2 ? 70:530,100,
       kLogicalWidth,kLogicalHeight,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd)return 16;
    ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);
    // Separate real GUI/message thread, parent injects input + examines pixels.
    for(int i=0;i<1600;++i) {
        if(InterlockedCompareExchange(&state->finish,0,0))break;
        pump(10);
    }
    DestroyWindow(hwnd);
    UnmapViewOfFile(state);CloseHandle(mapping);
    return 0;
}
struct Instance {
    HANDLE mapping{};
    NativeScaleState* state{};
    PROCESS_INFORMATION process{};
    HHOOK cbt{},mouse{};
};
void dispose(Instance& in) {
    if(in.mouse)UnhookWindowsHookEx(in.mouse);
    if(in.cbt)UnhookWindowsHookEx(in.cbt);
    if(in.process.hProcess) {
        if(WaitForSingleObject(in.process.hProcess,0)==WAIT_TIMEOUT){
            TerminateProcess(in.process.hProcess,100);
            WaitForSingleObject(in.process.hProcess,1000);
        }
        CloseHandle(in.process.hProcess);
    }
    if(in.process.hThread)CloseHandle(in.process.hThread);
    if(in.state)UnmapViewOfFile(in.state);
    if(in.mapping)CloseHandle(in.mapping);
}
bool injectInput(HWND hwnd) {
    SetForegroundWindow(hwnd);
    pump(45);
    RECT bounds{};GetClientRect(hwnd,&bounds);
    POINT start{bounds.right/2,bounds.bottom/2};
    ClientToScreen(hwnd,&start);
    if(!SetCursorPos(start.x,start.y))return false;
    pump(45);
    INPUT down{};down.type=INPUT_MOUSE;down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    INPUT move{};move.type=INPUT_MOUSE;
    move.mi.dwFlags=MOUSEEVENTF_MOVE;move.mi.dx=15;move.mi.dy=10;
    INPUT up{};up.type=INPUT_MOUSE;up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
    for(auto event:{down,move,up}) {
        if(SendInput(1,&event,sizeof(event))!=1)return false;
        pump(75);
    }
    return true;
}
bool checkPixels(HWND hwnd,int zoom) {
    RECT client{};GetClientRect(hwnd,&client);
    if(client.right!=MulDiv(kLogicalWidth,zoom,100) ||
       client.bottom!=MulDiv(kLogicalHeight,zoom,100))return false;
    HDC dc=GetDC(hwnd);
    if(!dc)return false;
    const COLORREF green=GetPixel(dc,MulDiv(68,zoom,100),
                                     MulDiv(47,zoom,100));
    const COLORREF dark=GetPixel(dc,10,10);
    ReleaseDC(hwnd,dc);
    return green==RGB(25,190,100)&&dark==RGB(10,12,16);
}
}
int wmain(int argc,wchar_t**) {
    if(argc>1)return child();
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr,exe,MAX_PATH);
    std::wstring directory=exe;
    directory.resize(directory.find_last_of(L"\\/")+1);
    const std::wstring dllFile=directory+L"PluginScalerNativeScaleHook-x86.dll";
    HMODULE dll=LoadLibraryW(dllFile.c_str());
    if(!dll){std::printf("native-hook-load=FAIL %lu\n",GetLastError());return 2;}
    auto cbt=reinterpret_cast<HOOKPROC>(GetProcAddress(dll,"NativeCBTHook"));
    if(!cbt)cbt=reinterpret_cast<HOOKPROC>(GetProcAddress(dll,"_NativeCBTHook@12"));
    auto mouse=reinterpret_cast<HOOKPROC>(GetProcAddress(dll,"NativeMouseHook"));
    if(!mouse)mouse=reinterpret_cast<HOOKPROC>(GetProcAddress(dll,"_NativeMouseHook@12"));
    if(!cbt||!mouse){std::puts("native-hook-exports=FAIL");return 3;}
    std::array<Instance,2> target{};
    bool ok=true;
    for(int i=0;i<2;++i) {
        auto& t=target[i];
        const std::wstring name=L"Local\\125A_NativeScale_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(i);
        t.mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(NativeScaleState),name.c_str());
        if(!t.mapping){ok=false;break;}
        t.state=static_cast<NativeScaleState*>(MapViewOfFile(t.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeScaleState)));
        if(!t.state){ok=false;break;}
        ZeroMemory(t.state,sizeof(NativeScaleState));
        t.state->scale=(i==0 ? 150 : 200);
        SetEnvironmentVariableW(kNativeScaleMapName,name.c_str());
        std::wstring command=L"\""+std::wstring(exe)+L"\" --child";
        std::vector<wchar_t> cmd(command.begin(),command.end());
        cmd.push_back(0);
        STARTUPINFOW si{};si.cb=sizeof(si);
        if(!CreateProcessW(exe,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&t.process)) {
            ok=false;break;
        }
    }
    SetEnvironmentVariableW(kNativeScaleMapName,nullptr);
    for(auto& t:target) {
        if(!ok)break;
        bool ready=false;
        for(int n=0;n<500;++n) {
            if(InterlockedCompareExchange(&t.state->ready,0,0)) {
                ready=true;break;
            }
            if(WaitForSingleObject(t.process.hProcess,0)==WAIT_OBJECT_0)break;
            Sleep(10);
        }
        if(!ready){ok=false;break;}
        t.cbt=SetWindowsHookExW(WH_CBT,cbt,dll,t.process.dwThreadId);
        t.mouse=SetWindowsHookExW(WH_GETMESSAGE,mouse,dll,t.process.dwThreadId);
        if(!t.cbt||!t.mouse){
            std::printf("native-hook-install=FAIL %lu\n",GetLastError());
            ok=false;break;
        }
    }
    if(ok) {
        for(auto& t:target)InterlockedExchange(&t.state->proceed,1);
        for(auto& t:target) {
            bool appeared=false;
            for(int n=0;n<400;++n) {
                if(InterlockedCompareExchange(&t.state->targetHwnd,0,0)) {
                    appeared=true;break;
                }
                Sleep(10);
            }
            if(!appeared){ok=false;break;}
            const HWND hwnd=reinterpret_cast<HWND>(static_cast<std::uintptr_t>(
                  InterlockedCompareExchange(&t.state->targetHwnd,0,0)));
            Sleep(200);
            const bool painted=checkPixels(hwnd,t.state->scale);
            const bool input=injectInput(hwnd);
            Sleep(60);
            const bool mouseVerified=input && t.state->mouseDown==1 &&
                t.state->mouseMove>0 && t.state->mouseUp==1 &&
                t.state->mismatch==0;
            const bool pass=t.state->hooked==1&&t.state->patchOK==1&&
                              painted&&mouseVerified;
            std::printf("in-process-unmodified-gdi-%ld=%s hwnd=%ld hook=%ld iat=%ld pixels=%d down=%ld move=%ld up=%ld mismatch=%ld\n",
               t.state->scale,pass?"PASS":"FAIL",t.state->targetHwnd,
               t.state->hooked,t.state->patchOK,int(painted),
               t.state->mouseDown,t.state->mouseMove,t.state->mouseUp,
               t.state->mismatch);
            ok &= pass;
        }
    }
    for(auto& t:target) {
        if(t.state)InterlockedExchange(&t.state->finish,1);
        if(t.process.hProcess)WaitForSingleObject(t.process.hProcess,2000);
    }
    for(auto& t:target)dispose(t);
    FreeLibrary(dll);
    std::printf("injected-gdi-150-200-native-mouse=%s\n",ok?"PASS":"FAIL");
    std::puts("LIMIT: Controlled Win32 GDI mock; does not establish jBridge or arbitrary VST support.");
    return ok?0:4;
}
