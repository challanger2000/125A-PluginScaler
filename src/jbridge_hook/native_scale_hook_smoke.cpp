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
int child(bool alreadyOpen,int slot) {
    wchar_t name[192]{};
    if(!GetEnvironmentVariableW(kNativeScaleMapName,name,192))return 11;
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping)return 12;
    state=static_cast<NativeScaleState*>(
       MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeScaleState)));
    if(!state){CloseHandle(mapping);return 13;}
    MSG msg{};
    PeekMessageW(&msg,nullptr,WM_USER,WM_USER,PM_NOREMOVE);
    if(!alreadyOpen) {
        InterlockedExchange(&state->ready,1);
        bool proceed=false;
        for(int i=0;i<700;++i) {
            if(InterlockedCompareExchange(&state->proceed,0,0)) {
                proceed=true;break;
            }
            Sleep(10);
        }
        if(!proceed)return 14;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr,self,MAX_PATH);
    std::wstring folder=self;
    folder.resize(folder.find_last_of(L"\\/")+1);
    const std::wstring renderer=folder+L"PluginScalerMockLegacyRenderer-x86.dll";
    HMODULE plugin=LoadLibraryW(renderer.c_str());
    if(!plugin)return 17;
    auto pluginProc=reinterpret_cast<WNDPROC>(GetProcAddress(plugin,"OriginalEditorProc"));
    if(!pluginProc)pluginProc=reinterpret_cast<WNDPROC>(
        GetProcAddress(plugin,"_OriginalEditorProc@16"));
    if(!pluginProc)return 18;
    WNDCLASSW wc{};
    wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpfnWndProc=pluginProc;
    wc.lpszClassName=kNativeScaleEditorClass;
    if(!RegisterClassW(&wc))return 15;
    // The original renderer is completely unaware of any scaling.
    HWND hwnd=CreateWindowExW(0,kNativeScaleEditorClass,L"Unscaled original",
       WS_POPUP|WS_VISIBLE,slot==0 ? 70:530,100,
       kLogicalWidth,kLogicalHeight,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd)return 16;
    ShowWindow(hwnd,SW_SHOW);UpdateWindow(hwnd);
    if(alreadyOpen) {
        // Editor HWND and pixels already exist before the hook is installed.
        InterlockedExchange(&state->targetHwnd,
            static_cast<LONG>(reinterpret_cast<std::uintptr_t>(hwnd)));
        InterlockedExchange(&state->ready,1);
    }
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
    HANDLE attachMapping{};
    NativeAttachCommand* attach{};
};
void dispose(Instance& in) {
    if(in.attach)UnmapViewOfFile(in.attach);
    if(in.attachMapping)CloseHandle(in.attachMapping);
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
int wmain(int argc,wchar_t** argv) {
    if(argc>1 && wcscmp(argv[1],L"--child")==0)return child(false,argc>2?_wtoi(argv[2]):0);
    if(argc>1 && wcscmp(argv[1],L"--child-existing")==0)return child(true,argc>2?_wtoi(argv[2]):0);
    const bool alreadyOpen=argc>1 && wcscmp(argv[1],L"--already-open")==0;
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
        std::wstring command=L"\""+std::wstring(exe)+
            (alreadyOpen ? L"\" --child-existing " : L"\" --child ")+std::to_wstring(i);
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
        if(alreadyOpen) {
            const std::wstring attachName=L"Local\\125A_NativeAttach_"+
                std::to_wstring(t.process.dwProcessId)+L"_"+
                std::to_wstring(t.process.dwThreadId);
            t.attachMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,
                PAGE_READWRITE,0,sizeof(NativeAttachCommand),attachName.c_str());
            if(!t.attachMapping){ok=false;break;}
            t.attach=static_cast<NativeAttachCommand*>(
                MapViewOfFile(t.attachMapping,FILE_MAP_ALL_ACCESS,0,0,
                              sizeof(NativeAttachCommand)));
            if(!t.attach){ok=false;break;}
            ZeroMemory(t.attach,sizeof(NativeAttachCommand));
            t.attach->scale=t.state->scale;
            t.attach->hwnd=t.state->targetHwnd;
        } else {
            t.cbt=SetWindowsHookExW(WH_CBT,cbt,dll,t.process.dwThreadId);
        }
        t.mouse=SetWindowsHookExW(WH_GETMESSAGE,mouse,dll,t.process.dwThreadId);
        if((!alreadyOpen&&!t.cbt)||!t.mouse){
            std::printf("native-hook-install=FAIL %lu\n",GetLastError());
            ok=false;break;
        }
    }
    if(ok) {
        if(!alreadyOpen)
            for(auto& t:target)InterlockedExchange(&t.state->proceed,1);
        else
            for(auto& t:target)
                PostMessageW(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(
                    t.state->targetHwnd)),WM_NULL,0,0);
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
            if(alreadyOpen) {
                for(int n=0;n<400 && t.attach->status==0;++n)Sleep(10);
                if(t.attach->status==1)InvalidateRect(hwnd,nullptr,FALSE);
            }
            Sleep(200);
            const bool painted=checkPixels(hwnd,t.state->scale);
            const bool input=injectInput(hwnd);
            Sleep(60);
            const bool mouseVerified=input && t.state->mouseDown==1 &&
                t.state->mouseMove>0 && t.state->mouseUp==1 &&
                t.state->mismatch==0;
            const bool attached=alreadyOpen ?
                 (t.attach && t.attach->status==1 &&
                  t.attach->originalWidth==kLogicalWidth &&
                  t.attach->originalHeight==kLogicalHeight) :
                 (t.state->hooked==1 && t.state->patchOK==1);
            const bool pass=attached&&painted&&mouseVerified;
            std::printf("in-process-%s-gdi-%ld=%s hwnd=%ld attached=%ld hook=%ld iat=%ld pixels=%d down=%ld move=%ld up=%ld mismatch=%ld\n",
               alreadyOpen?"already-open":"creation",t.state->scale,
               pass?"PASS":"FAIL",t.state->targetHwnd,
               alreadyOpen?(t.attach?t.attach->status:-99):0,
               t.state->hooked,t.state->patchOK,int(painted),
               t.state->mouseDown,t.state->mouseMove,t.state->mouseUp,
               t.state->mismatch);
            ok &= pass;
            if(alreadyOpen && pass) {
                // Stop scaling while original editor is STILL running.
                // A safe detach must restore both the GDI import and HWND.
                t.attach->detach=1;
                PostMessageW(hwnd,WM_NULL,0,0);
                for(int n=0;n<400 && t.attach->status==1;++n)Sleep(10);
                Sleep(90);
                const bool restored=t.attach->status==2&&checkPixels(hwnd,100);
                InterlockedExchange(&t.state->scale,100);
                // Ignore input already queued under the preceding 150/200% mode.
                // Assert native coordinates only after the original mode settles.
                Sleep(200);
                InterlockedExchange(&t.state->mismatch,0);
                const bool clicked=restored&&injectInput(hwnd);
                Sleep(60);
                const bool inputBack=clicked&&t.state->mouseDown==2 &&
                      t.state->mouseUp==2&&t.state->mismatch==0;
                std::printf("already-open-detach-restore-%ld=%s status=%ld originalPixels=%d originalMouse=%d down=%ld up=%ld mismatch=%ld\n",
                   t.attach->scale,(restored&&inputBack)?"PASS":"FAIL",
                   t.attach->status,int(restored),int(inputBack),t.state->mouseDown,t.state->mouseUp,t.state->mismatch);
                ok &= restored&&inputBack;
            }
        }
    }
    for(auto& t:target) {
        if(t.state)InterlockedExchange(&t.state->finish,1);
        if(t.process.hProcess)WaitForSingleObject(t.process.hProcess,2000);
    }
    for(auto& t:target)dispose(t);
    FreeLibrary(dll);
    std::printf("%s-gdi-150-200-native-mouse=%s\n",
        alreadyOpen?"already-open-injected":"creation-injected",ok?"PASS":"FAIL");
    std::puts("LIMIT: Controlled Win32 GDI mock; does not establish jBridge or arbitrary VST support.");
    return ok?0:4;
}
