#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <string>
#include <string_view>

using namespace pluginscaler::formats::vst2abi;

namespace {
VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float) {
    return opcode == AudioMasterVersion ? 2400 : 0;
}
using EntryProc = AEffect* (__cdecl*)(AudioMasterCallback);

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd,msg,wp,lp);
}

HWND createHost() {
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
                           CW_USEDEFAULT,CW_USEDEFAULT,1300,700,
                           nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
}

BOOL CALLBACK findEditorProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd,cls,256);
    if(std::wstring_view(cls)==L"125A_MockLegacyGdiEditor"){
        *reinterpret_cast<HWND*>(lp)=hwnd;
        return FALSE;
    }
    return TRUE;
}

bool approx(COLORREF c, int r, int g, int b, int tol=45) {
    if(c==CLR_INVALID) return false;
    return std::abs((int)GetRValue(c)-r)<=tol &&
           std::abs((int)GetGValue(c)-g)<=tol &&
           std::abs((int)GetBValue(c)-b)<=tol;
}
}

int wmain(int argc, wchar_t** argv) {
    if(argc!=5) return 1;
    SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86",argv[2]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2",argv[3]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST",argv[4]);
    SetEnvironmentVariableW(L"PLUGINSCALER_SCALE_PERCENT",L"150");
    SetEnvironmentVariableW(L"PLUGINSCALER_EDITOR_MODE",L"Gdi");

    HMODULE proxy=LoadLibraryW(argv[1]);
    if(!proxy) return 2;
    auto entry=reinterpret_cast<EntryProc>(GetProcAddress(proxy,"VSTPluginMain"));
    if(!entry) return 3;
    AEffect* effect=entry(hostCallback);
    if(!effect || !effect->dispatcher) return 4;

    HWND host=createHost();
    if(!host) return 5;
    ShowWindow(host,SW_SHOW);
    UpdateWindow(host);

    VstRect* rect=nullptr;
    const auto rectOk=effect->dispatcher(effect,EffEditGetRect,0,0,&rect,0.0f);
    const int rw=rect?rect->right-rect->left:0;
    const int rh=rect?rect->bottom-rect->top:0;
    bool ok=rectOk && rw==1143 && rh==537;
    std::cout<<"gdi-rect="<<rw<<"x"<<rh<<"\n";

    if(ok) ok=effect->dispatcher(effect,EffEditOpen,0,0,host,0.0f)!=0;
    Sleep(150);

    HWND editor=nullptr;
    EnumChildWindows(host,findEditorProc,reinterpret_cast<LPARAM>(&editor));
    ok=ok && editor!=nullptr;
    std::cout<<"gdi-editor="<<(editor?1:0)<<"\n";

    if(editor){
        RECT rc{};
        GetClientRect(editor,&rc);
        const int ew=rc.right-rc.left, eh=rc.bottom-rc.top;
        std::cout<<"gdi-editor-size="<<ew<<"x"<<eh<<"\n";
        ok=ok && ew==1143 && eh==537;

        HDC dc=GetDC(editor);
        COLORREF keyBefore=GetPixel(dc, 150, 450); // native (100,300) at 150%
        ReleaseDC(editor,dc);
        const bool scaledVisual=approx(keyBefore,224,224,224,65);
        std::cout<<"gdi-scaled-visual="<<(scaledVisual?"PASS":"FAIL")<<"\n";
        ok=ok && scaledVisual;

        SendMessageW(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(150,450));
        Sleep(30);
        dc=GetDC(editor);
        COLORREF keyDown=GetPixel(dc,150,450);
        ReleaseDC(editor,dc);
        const bool clickMapped=approx(keyDown,0,204,0,70);
        std::cout<<"gdi-click-map="<<(clickMapped?"PASS":"FAIL")<<"\n";
        ok=ok && clickMapped;
        SendMessageW(editor,WM_LBUTTONUP,0,MAKELPARAM(150,450));

        // Native knob centre (425,125) appears at about (638,188).
        SendMessageW(editor,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(638,188));
        SendMessageW(editor,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(638,143));
        SendMessageW(editor,WM_LBUTTONUP,0,MAKELPARAM(638,143));
        Sleep(30);
        dc=GetDC(editor);
        COLORREF knobPixel=GetPixel(dc,638,143);
        ReleaseDC(editor,dc);
        const bool dragRedraw=knobPixel!=CLR_INVALID;
        std::cout<<"gdi-drag-redraw="<<(dragRedraw?"PASS":"FAIL")<<"\n";
        ok=ok && dragRedraw;
    }

    const auto closeOk=effect->dispatcher(effect,EffEditClose,0,0,nullptr,0.0f);
    std::cout<<"gdi-close="<<(closeOk?"PASS":"FAIL")<<"\n";
    ok=ok && closeOk!=0;

    effect->dispatcher(effect,EffClose,0,0,nullptr,0.0f);
    DestroyWindow(host);
    FreeLibrary(proxy);

    std::cout<<"gdi-harness="<<(ok?"PASS":"FAIL")<<"\n";
    return ok?0:6;
}
