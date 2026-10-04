#define NOMINMAX
#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>
#include <windowsx.h>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {
VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float) {
    return opcode == AudioMasterVersion ? 2400 : 0;
}
using EntryProc = AEffect* (__cdecl*)(AudioMasterCallback);

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd,msg,wp,lp);
}

HWND createHost(int width, int height) {
    static ATOM atom=0;
    static const wchar_t* cls=L"125A_GdiHarnessHost";
    if(!atom){
        WNDCLASSW wc{};
        wc.lpfnWndProc=hostProc;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=cls;
        atom=RegisterClassW(&wc);
        if(!atom && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    }
    return CreateWindowExW(0,cls,L"GDI Harness",WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT,CW_USEDEFAULT,width+80,height+120,
                           nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
}

BOOL CALLBACK findSurfaceProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd,cls,256);
    if(std::wstring_view(cls)==L"125A_PluginScaler_ScaledSurface"){
        *reinterpret_cast<HWND*>(lp)=hwnd;
        return FALSE;
    }
    return TRUE;
}

int childCount(HWND parent) {
    int count=0;
    EnumChildWindows(parent,
        [](HWND, LPARAM lp)->BOOL {
            ++*reinterpret_cast<int*>(lp);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

bool approx(COLORREF c, int r, int g, int b, int tol=45) {
    if(c==CLR_INVALID) return false;
    return std::abs((int)GetRValue(c)-r)<=tol &&
           std::abs((int)GetGValue(c)-g)<=tol &&
           std::abs((int)GetBValue(c)-b)<=tol;
}

int scaledCoord(int native, int scale) {
    return native * scale / 100;
}

void pumpMessagesFor(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool runScale(EntryProc entry, int scale) {
    const std::wstring scaleText=std::to_wstring(scale);
    SetEnvironmentVariableW(L"PLUGINSCALER_SCALE_PERCENT",scaleText.c_str());

    AEffect* effect=entry(hostCallback);
    if(!effect || !effect->dispatcher) {
        std::cout<<"gdi-scale-"<<scale<<"=FAIL\n";
        return false;
    }

    constexpr int nativeWidth=762;
    constexpr int nativeHeight=358;
    const int expectedWidth=nativeWidth*scale/100;
    const int expectedHeight=nativeHeight*scale/100;

    bool ok=true;
    VstRect* rect=nullptr;
    const auto rectOk=effect->dispatcher(effect,EffEditGetRect,0,0,&rect,0.0f);
    const int rw=rect?rect->right-rect->left:0;
    const int rh=rect?rect->bottom-rect->top:0;
    ok=ok && rectOk && rw==expectedWidth && rh==expectedHeight;
    std::cout<<"gdi-rect-"<<scale<<"="<<rw<<"x"<<rh<<"\n";

    for(int cycle=1; cycle<=3 && ok; ++cycle) {
        HWND host=createHost(expectedWidth,expectedHeight);
        if(!host) { ok=false; break; }
        ShowWindow(host,SW_SHOW);
        UpdateWindow(host);

        const auto openOk=effect->dispatcher(effect,EffEditOpen,0,0,host,0.0f);
        // The legacy GDI path intentionally gives old editors a quiet
        // startup grace period before capture. Keep pumping the host message
        // queue so the surface timer can begin capture after that grace.
        pumpMessagesFor(2300);

        HWND surface=nullptr;
        EnumChildWindows(host,findSurfaceProc,reinterpret_cast<LPARAM>(&surface));
        ok=ok && openOk!=0 && surface!=nullptr;

        if(surface) {
            RECT rc{};
            GetClientRect(surface,&rc);
            const int sw=rc.right-rc.left;
            const int sh=rc.bottom-rc.top;
            ok=ok && sw==expectedWidth && sh==expectedHeight;

            const int keyX=scaledCoord(100,scale);
            const int keyY=scaledCoord(300,scale);

            HDC dc=GetDC(surface);
            const COLORREF keyBefore=GetPixel(dc,keyX,keyY);
            ReleaseDC(surface,dc);
            const bool scaledVisual=approx(keyBefore,224,224,224,65);
            ok=ok && scaledVisual;

            SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(keyX,keyY));
            Sleep(20);
            dc=GetDC(surface);
            const COLORREF keyDown=GetPixel(dc,keyX,keyY);
            ReleaseDC(surface,dc);
            const bool clickMapped=approx(keyDown,0,204,0,70);
            ok=ok && clickMapped;
            SendMessageW(surface,WM_LBUTTONUP,0,MAKELPARAM(keyX,keyY));

            // Native knob centre (425,125), drag 30 native pixels upward.
            const int knobX=scaledCoord(425,scale);
            const int knobStartY=scaledCoord(125,scale);
            const int knobEndY=scaledCoord(95,scale);
            SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,
                         MAKELPARAM(knobX,knobStartY));
            SendMessageW(surface,WM_MOUSEMOVE,MK_LBUTTON,
                         MAKELPARAM(knobX,knobEndY));
            SendMessageW(surface,WM_LBUTTONUP,0,
                         MAKELPARAM(knobX,knobEndY));
            Sleep(20);

            dc=GetDC(surface);
            const COLORREF knobPixel=GetPixel(dc,knobX,knobEndY);
            ReleaseDC(surface,dc);
            const bool dragRedraw=knobPixel!=CLR_INVALID;
            ok=ok && dragRedraw;

            std::cout<<"gdi-cycle-"<<scale<<"-"<<cycle
                     <<"="<<(ok?"PASS":"FAIL")<<"\n";
        }

        const auto closeOk=effect->dispatcher(
            effect,EffEditClose,0,0,nullptr,0.0f);
        Sleep(30);
        const bool closed=closeOk!=0 && childCount(host)==0;
        ok=ok && closed;
        DestroyWindow(host);
    }

    effect->dispatcher(effect,EffClose,0,0,nullptr,0.0f);
    std::cout<<"gdi-scale-"<<scale<<"="<<(ok?"PASS":"FAIL")<<"\n";
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    if(argc!=5) return 1;
    SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86",argv[2]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2",argv[3]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST",argv[4]);
    SetEnvironmentVariableW(L"PLUGINSCALER_EDITOR_MODE",L"Gdi");

    HMODULE proxy=LoadLibraryW(argv[1]);
    if(!proxy) return 2;
    auto entry=reinterpret_cast<EntryProc>(GetProcAddress(proxy,"VSTPluginMain"));
    if(!entry) {
        FreeLibrary(proxy);
        return 3;
    }

    bool ok=true;
    for(const int scale : std::vector<int>{125,150,200})
        ok=runScale(entry,scale) && ok;

    FreeLibrary(proxy);

    std::cout<<"gdi-scaled-visual="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-click-map="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-drag-redraw="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-close="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-harness="<<(ok?"PASS":"FAIL")<<"\n";
    return ok?0:6;
}
