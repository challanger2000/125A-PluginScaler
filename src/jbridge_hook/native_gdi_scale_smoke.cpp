#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace {
constexpr int logicalW=200, logicalH=140;
struct Editor {
    HWND hwnd{};
    int scale{};
    int down{}, moves{}, up{};
    bool drag{};
    bool coordsAgreed{true};
    POINT lastLogical{};
};
LRESULT CALLBACK editorWnd(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    Editor* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        e=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        e->hwnd=hwnd;
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e));
        return TRUE;
    }
    if(!e) return DefWindowProcW(hwnd,msg,wp,lp);
    if(msg==WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(hwnd,&paint);
        if(dc) {
            RECT area{};GetClientRect(hwnd,&area);
            HBRUSH bg=CreateSolidBrush(RGB(10,12,16));
            FillRect(dc,&area,bg);
            DeleteObject(bg);
            const int saved=SaveDC(dc);
            SetMapMode(dc,MM_ANISOTROPIC);
            SetWindowExtEx(dc,logicalW,logicalH,nullptr);
            SetViewportExtEx(dc,MulDiv(logicalW,e->scale,100),
                                MulDiv(logicalH,e->scale,100),nullptr);
            HBRUSH knob=CreateSolidBrush(RGB(25,190,100));
            SelectObject(dc,knob);
            SelectObject(dc,GetStockObject(NULL_PEN));
            Rectangle(dc,48,30,80,66);
            RestoreDC(dc,saved);
            DeleteObject(knob);
            EndPaint(hwnd,&paint);
        }
        return 0;
    }
    if(msg==WM_LBUTTONDOWN||msg==WM_MOUSEMOVE||msg==WM_LBUTTONUP) {
        const DWORD messagePosition=GetMessagePos();
        POINT screen{GET_X_LPARAM(static_cast<LPARAM>(messagePosition)),
                     GET_Y_LPARAM(static_cast<LPARAM>(messagePosition))};
        if(ScreenToClient(hwnd,&screen)) {
            const POINT cursorLogical{MulDiv(screen.x,100,e->scale),
                                      MulDiv(screen.y,100,e->scale)};
            const POINT messageLogical{MulDiv(GET_X_LPARAM(lp),100,e->scale),
                                       MulDiv(GET_Y_LPARAM(lp),100,e->scale)};
            if(abs(cursorLogical.x-messageLogical.x)>1||
               abs(cursorLogical.y-messageLogical.y)>1) e->coordsAgreed=false;
            e->lastLogical=messageLogical;
        }
        if(msg==WM_LBUTTONDOWN) {
            ++e->down;
            e->drag=true;
            SetCapture(hwnd);
        }
        if(msg==WM_MOUSEMOVE&&e->drag) ++e->moves;
        if(msg==WM_LBUTTONUP) {
            ++e->up;
            e->drag=false;
            if(GetCapture()==hwnd) ReleaseCapture();
        }
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
void pump(int milliseconds) {
    const DWORD start=GetTickCount();
    while(GetTickCount()-start<static_cast<DWORD>(milliseconds)) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}
bool sendInput(INPUT event) {
    return SendInput(1,&event,sizeof(event))==1;
}
bool dragNative(Editor& e) {
    SetForegroundWindow(e.hwnd);
    SetFocus(e.hwnd);
    pump(45);
    POINT start{MulDiv(65,e.scale,100),MulDiv(45,e.scale,100)};
    ClientToScreen(e.hwnd,&start);
    if(!SetCursorPos(start.x,start.y)) return false;
    pump(40);
    INPUT down{};down.type=INPUT_MOUSE;down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    if(!sendInput(down)) return false;
    pump(45);
    INPUT move{};move.type=INPUT_MOUSE;
    move.mi.dwFlags=MOUSEEVENTF_MOVE;
    move.mi.dx=18;move.mi.dy=12;
    if(!sendInput(move)) return false;
    pump(45);
    INPUT up{};up.type=INPUT_MOUSE;up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(!sendInput(up)) return false;
    pump(45);
    return e.down==1&&e.moves>0&&e.up==1&&!e.drag&&e.coordsAgreed;
}
bool pixels(Editor& e) {
    HDC dc=GetDC(e.hwnd);
    if(!dc) return false;
    // A pixel inside the transformed GDI rectangle, but outside the
    // unscaled rectangle; a false "PASS" for unscaled paint is impossible.
    const COLORREF inside=GetPixel(dc,MulDiv(68,e.scale,100),
                                       MulDiv(47,e.scale,100));
    const COLORREF outside=GetPixel(dc,20,20);
    ReleaseDC(e.hwnd,dc);
    return inside==RGB(25,190,100)&&outside==RGB(10,12,16);
}
}
int main() {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW klass{};klass.hInstance=instance;
    klass.lpfnWndProc=editorWnd;
    klass.lpszClassName=L"125A.NativeGdiScaleSmoke";
    klass.hCursor=LoadCursorA(nullptr,IDC_ARROW);
    if(!RegisterClassW(&klass)) return 1;
    Editor a{};a.scale=150;
    Editor b{};b.scale=200;
    const DWORD style=WS_OVERLAPPEDWINDOW;
    auto spawn=[&](Editor& e,int x,int y) {
        RECT win{0,0,MulDiv(logicalW,e.scale,100),
                       MulDiv(logicalH,e.scale,100)};
        AdjustWindowRectEx(&win,style,FALSE,0);
        return CreateWindowExW(0,klass.lpszClassName,L"Native GDI original",
            style|WS_VISIBLE,x,y,win.right-win.left,win.bottom-win.top,
            nullptr,nullptr,instance,&e);
    };
    HWND ha=spawn(a,60,60);
    HWND hb=spawn(b,470,60);
    if(!ha||!hb) return 2;
    ShowWindow(ha,SW_SHOW);ShowWindow(hb,SW_SHOW);
    UpdateWindow(ha);UpdateWindow(hb);pump(100);
    RECT ra{},rb{};GetClientRect(ha,&ra);GetClientRect(hb,&rb);
    const bool dimensions=ra.right==300&&ra.bottom==210&&
                          rb.right==400&&rb.bottom==280;
    const bool drawn=pixels(a)&&pixels(b);
    std::printf("native-150-200-gdi-client=%s\n",dimensions?"PASS":"FAIL");
    std::printf("native-150-200-scaled-pixels=%s\n",drawn?"PASS":"FAIL");
    const bool inputA=dragNative(a);
    const bool inputB=dragNative(b);
    std::printf("debug-150 down=%d move=%d up=%d drag=%d coord=%d logical=%ld,%ld\n",
                a.down,a.moves,a.up,int(a.drag),int(a.coordsAgreed),a.lastLogical.x,a.lastLogical.y);
    std::printf("debug-200 down=%d move=%d up=%d drag=%d coord=%d logical=%ld,%ld\n",
                b.down,b.moves,b.up,int(b.drag),int(b.coordsAgreed),b.lastLogical.x,b.lastLogical.y);
    const bool independent=a.down==1&&b.down==1&&a.up==1&&b.up==1
                            &&ha!=hb&&a.coordsAgreed&&b.coordsAgreed;
    std::printf("native-150-mouse-capture=%s\n",inputA?"PASS":"FAIL");
    std::printf("native-200-mouse-capture=%s\n",inputB?"PASS":"FAIL");
    std::printf("native-two-window-isolation=%s\n",independent?"PASS":"FAIL");
    DestroyWindow(ha);DestroyWindow(hb);
    const bool passed=dimensions&&drawn&&inputA&&inputB&&independent;
    std::printf("native-gdi-render-input-150-200=%s\n",passed?"PASS":"FAIL");
    std::puts("NOTE: Instrumented GDI mock, not a proof of unmodified VST editor support.");
    return passed?0:3;
}
