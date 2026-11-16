// CI-only synthetic VST2 DLL. This is a test fixture, NOT the user DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "scaler/vst2_abi.hpp"
using namespace scaler::vst2;
namespace {
AEffect effect{};
HWND editor{};
ERect rect{0,0,280,480};
LRESULT CALLBACK paint(HWND hwnd,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_PAINT){
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(hwnd,&ps);
        for(int row=0;row<7;++row)for(int col=0;col<12;++col){
            RECT r{col*40,row*40,(col+1)*40,(row+1)*40};
            HBRUSH brush=CreateSolidBrush(RGB((col*41+row*13)%256,
                                                  (col*17+row*57)%256,
                                                  (col*83+row*29)%256));
            FillRect(dc,&r,brush);
            DeleteObject(brush);
        }
        EndPaint(hwnd,&ps);return 0;
    }
    return DefWindowProcW(hwnd,m,w,l);
}
Ptr __cdecl dispatch(AEffect*,Int op,Int,Ptr,void* ptr,float){
    if(op==kEffEditGetRect){*static_cast<ERect**>(ptr)=&rect;return 1;}
    if(op==kEffEditOpen){
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpfnWndProc=paint;wc.lpszClassName=L"125A_Synthetic_VST2_GUI";
        RegisterClassW(&wc);
        editor=CreateWindowExW(0,wc.lpszClassName,L"",WS_CHILD|WS_VISIBLE,
            0,0,480,280,static_cast<HWND>(ptr),nullptr,wc.hInstance,nullptr);
        return editor?1:0;
    }
    if(op==kEffEditClose){if(IsWindow(editor))DestroyWindow(editor);editor=nullptr;return 1;}
    return 1;
}
void __cdecl process(AEffect*,float**,float**,Int){}
void __cdecl setParam(AEffect*,Int,float){}
float __cdecl getParam(AEffect*,Int){return 0.f;}
}
extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(Host) {
    effect.magic=kMagic;
    effect.dispatcher=dispatch;
    effect.process=process;
    effect.setParameter=setParam;
    effect.getParameter=getParam;
    effect.flags=kHasEditor;
    effect.numInputs=2;effect.numOutputs=2;
    effect.version=1;
    return &effect;
}
