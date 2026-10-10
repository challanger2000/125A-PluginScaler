#include "native_scale_shared.h"
#include <windowsx.h>
#include <cstdlib>
#include <cwchar>
#include <vector>
#include <cstdint>

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
        if(state && state->rendererMode==4 && state->partialToggle) {
            RECT update{};
            if(GetUpdateRect(hwnd,&update,FALSE)) {
                InterlockedExchange(&state->partialUpdateLeft,update.left);
                InterlockedExchange(&state->partialUpdateRight,update.right);
            }
        }
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(hwnd,&ps);
        if(state && state->rendererMode==4 && state->partialToggle) {
            InterlockedExchange(&state->partialPaintLeft,ps.rcPaint.left);
            InterlockedExchange(&state->partialPaintRight,ps.rcPaint.right);
        }
        if(dc) {
            if(state && state->rendererMode>=1) {
                // A realistic unscaled VST-style bitmap renderer.
                // No StretchDIBits or zoom code belongs to this DLL.
                BITMAPINFO bitmap{};
                bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                bitmap.bmiHeader.biWidth=kLogicalWidth;
                bitmap.bmiHeader.biHeight=-kLogicalHeight; // top-down
                bitmap.bmiHeader.biPlanes=1;
                bitmap.bmiHeader.biBitCount=32;
                bitmap.bmiHeader.biCompression=BI_RGB;
                std::vector<std::uint32_t> pixels(kLogicalWidth*kLogicalHeight,
                                                   0x000A0C10);
                const std::uint32_t fill=(state->rendererMode==4 &&
                    state->partialToggle)?0x00B030D0:0x0019BE64;
                for(int y=30;y<66;++y)
                    for(int x=48;x<80;++x)
                        pixels[y*kLogicalWidth+x]=fill;
                if(state->rendererMode==3) {
                    // Reproduce actual Pro-53 input after the plugin HWND
                    // grows: SetDIBitsToDevice asks for physical client
                    // size but its backing pixels remain at 100%.
                    RECT client{};
                    GetClientRect(hwnd,&client);
                    const int w=client.right-client.left;
                    const int h=client.bottom-client.top;
                    SetDIBitsToDevice(dc,0,0,w,h,0,
                        kLogicalHeight-h,0,kLogicalHeight,
                        pixels.data(),&bitmap,DIB_RGB_COLORS);
                } else if(state->rendererMode==2) {
                    // First fill with background only; then paint the
                    // green rectangle as a CLIPPED source-DIB region.
                    // This test was impossible to pass with the former
                    // full-frame-only SetDIBits hooking condition.
                    std::vector<std::uint32_t> background(
                        kLogicalWidth*kLogicalHeight,0x000A0C10);
                    SetDIBitsToDevice(dc,0,0,kLogicalWidth,kLogicalHeight,
                        0,0,0,kLogicalHeight,background.data(),&bitmap,
                        DIB_RGB_COLORS);
                    // Nonzero source coordinates and a source rectangle
                    // smaller than the BITMAPINFO header dimensions.
                    InterlockedIncrement(&state->clippedDibCalls);
                    SetDIBitsToDevice(dc,48,30,32,36,48,kLogicalHeight-30-36,0,
                        kLogicalHeight,pixels.data(),&bitmap,DIB_RGB_COLORS);
                } else {
                    SetDIBitsToDevice(dc,0,0,kLogicalWidth,kLogicalHeight,
                        0,0,0,kLogicalHeight,pixels.data(),&bitmap,DIB_RGB_COLORS);
                }
            } else {
                HBRUSH bg=CreateSolidBrush(RGB(10,12,16));
                HBRUSH bright=CreateSolidBrush(RGB(25,190,100));
                SelectObject(dc,GetStockObject(NULL_PEN));
                SelectObject(dc,bg);
                Rectangle(dc,0,0,kLogicalWidth,kLogicalHeight);
                SelectObject(dc,bright);
                Rectangle(dc,48,30,80,66);
                DeleteObject(bg);DeleteObject(bright);
            }
            EndPaint(hwnd,&ps);
        }
        return 0;
    }
    if(state && (msg==WM_LBUTTONDOWN||msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP)) {
        const int logicalX=GET_X_LPARAM(lp),logicalY=GET_Y_LPARAM(lp);
        const bool direct=InSendMessage()!=FALSE;
        if(direct && (msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP)) {
            InterlockedIncrement(&state->directSentMouse);
            InterlockedExchange(&state->directSentX,logicalX);
            InterlockedExchange(&state->directSentY,logicalY);
        }
        const DWORD timestamped=GetMessagePos();
        POINT screen{GET_X_LPARAM(static_cast<LPARAM>(timestamped)),
                     GET_Y_LPARAM(static_cast<LPARAM>(timestamped))};
        if(ScreenToClient(hwnd,&screen)) {
            const LONG scale=InterlockedCompareExchange(&state->scale,0,0);
            const bool tracked=msg==WM_LBUTTONDOWN || msg==WM_LBUTTONUP ||
                               (msg==WM_MOUSEMOVE && (wp&MK_LBUTTON));
            if(tracked && !direct && scale>0 &&
               (std::abs(MulDiv(screen.x,100,scale)-logicalX)>1 ||
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
            if(state->rendererMode==4) {
                // The original renderer invalidates a SMALL logical
                // control rectangle, not the entire blown-up editor.
                InterlockedExchange(&state->partialToggle,1);
                const RECT redraw{48,30,80,66};
                InvalidateRect(hwnd,&redraw,FALSE);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
