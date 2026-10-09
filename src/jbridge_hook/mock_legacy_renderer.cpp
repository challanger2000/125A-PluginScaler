#include "native_scale_shared.h"
#include <windowsx.h>
#include <cstdlib>
#include <cwchar>

namespace {
NativeScaleState* state{};
HANDLE mapping{};
void mapStateOnce() {
    if(state)return;
    wchar_t name[192]{};
    if(!GetEnvironmentVariableW(kNativeScaleMapName,name,192))return;
    mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping)return;
    state=static_cast<NativeScaleState*>(
        MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeScaleState)));
}
}
extern "C" __declspec(dllexport)
LRESULT CALLBACK OriginalEditorProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_NCCREATE)mapStateOnce();
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(hwnd,&ps);
        if(dc) {
            HBRUSH bg=CreateSolidBrush(RGB(10,12,16));
            HBRUSH bright=CreateSolidBrush(RGB(25,190,100));
            SelectObject(dc,GetStockObject(NULL_PEN));
            SelectObject(dc,bg);
            Rectangle(dc,0,0,kLogicalWidth,kLogicalHeight);
            SelectObject(dc,bright);
            Rectangle(dc,48,30,80,66);
            DeleteObject(bg);DeleteObject(bright);
            EndPaint(hwnd,&ps);
        }
        return 0;
    }
    if(state && (msg==WM_LBUTTONDOWN||msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP)) {
        const int logicalX=GET_X_LPARAM(lp),logicalY=GET_Y_LPARAM(lp);
        const DWORD timestamped=GetMessagePos();
        POINT screen{GET_X_LPARAM(static_cast<LPARAM>(timestamped)),
                     GET_Y_LPARAM(static_cast<LPARAM>(timestamped))};
        if(ScreenToClient(hwnd,&screen)) {
            const LONG scale=InterlockedCompareExchange(&state->scale,0,0);
            if(scale>0 && (std::abs(MulDiv(screen.x,100,scale)-logicalX)>1 ||
                           std::abs(MulDiv(screen.y,100,scale)-logicalY)>1))
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
