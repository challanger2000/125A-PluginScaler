#pragma once
// Isolated OS-level test. NEVER enable a mask in the shipping Scaler unless
// original source pixels remain readable while desktop source is obscured.
// Uses a WS_EX_LAYERED opaque shield, per the documented Win32 composition
// method. Test catches the previous "white mirror" regression.
#include <windows.h>
#include <magnification.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

namespace scaler_mask_experiment {
constexpr wchar_t kClass[]=L"125A.Scaler.QA.LayeredShield";
constexpr COLORREF kColor=RGB(250,15,210);
LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT){
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(hwnd,&ps);
        RECT r{};GetClientRect(hwnd,&r);
        HBRUSH b=CreateSolidBrush(kColor);
        FillRect(dc,&r,b);
        DeleteObject(b);
        EndPaint(hwnd,&ps);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
bool similar(COLORREF a,COLORREF b,int tolerance=48) {
    return a!=CLR_INVALID && b!=CLR_INVALID &&
        std::abs(int(GetRValue(a))-int(GetRValue(b)))<=tolerance &&
        std::abs(int(GetGValue(a))-int(GetGValue(b)))<=tolerance &&
        std::abs(int(GetBValue(a))-int(GetBValue(b)))<=tolerance;
}
COLORREF read(POINT pos) {
    HDC dc=GetDC(nullptr);
    if(!dc)return CLR_INVALID;
    COLORREF c=GetPixel(dc,pos.x,pos.y);
    ReleaseDC(nullptr,dc);
    return c;
}
void pump() {
    MSG m{};
    while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
struct Probe {POINT dest; COLORREF source;};
int run(HWND source) {
    if(!IsWindow(source)||!IsWindow(g.viewer)||!IsWindow(g.magnifier))
        return 10;
    RECT bounds{};if(!source_bounds(source,bounds))return 11;
    const int width=bounds.right-bounds.left;
    const int height=bounds.bottom-bounds.top;
    MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromWindow(source,MONITOR_DEFAULTTONEAREST),
                       &monitor))return 12;
    const RECT screen=monitor.rcMonitor;
    // The GitHub desktop is small. Make the real source and enlarged
    // viewer non-overlapping before reading reference pixels; otherwise
    // viewer could obscure the source and yield a false mask result.
    RECT window{};
    if(!GetWindowRect(g.viewer,&window))return 18;
    const int viewerW=window.right-window.left;
    const int viewerH=window.bottom-window.top;
    int viewerX=window.left,viewerY=window.top;
    if(bounds.bottom+16+viewerH <= monitor.rcWork.bottom) {
        viewerY=bounds.bottom+16;
        viewerX=monitor.rcWork.left+8;
    } else if(bounds.left-viewerW-16 >= monitor.rcWork.left) {
        viewerX=bounds.left-viewerW-16;
        viewerY=monitor.rcWork.top+8;
    } else if(bounds.right+viewerW+16 <= monitor.rcWork.right) {
        viewerX=bounds.right+16;
        viewerY=monitor.rcWork.top+8;
    } else {
        std::cout<<"MASK_INCONCLUSIVE: source and enlarged viewer "
                    "cannot both be shown without overlap\n";
        return 77;
    }
    SetWindowPos(g.viewer,HWND_TOPMOST,viewerX,viewerY,0,0,
        SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    for(int n=0;n<10;++n){pump();Sleep(50);}
    std::vector<Probe> before;
    for(int row=1;row<11;++row)for(int col=1;col<15;++col) {
        const int x=width*col/15;
        const int y=height*row/11;
        POINT src{bounds.left+x,bounds.top+y};
        POINT dst{scaler::scaled_pixels(x,g.effectivePercent),
                  scaler::scaled_pixels(y,g.effectivePercent)};
        if(!ClientToScreen(g.magnifier,&dst) ||
           src.x<screen.left || src.x>=screen.right ||
           src.y<screen.top || src.y>=screen.bottom ||
           dst.x<screen.left || dst.x>=screen.right ||
           dst.y<screen.top || dst.y>=screen.bottom)continue;
        COLORREF c=read(src);
        if(c!=CLR_INVALID)before.push_back({dst,c});
    }
    if(before.size()<30){
        std::cout<<"MASK_INCONCLUSIVE: insufficient original GUI baseline pixels\n";
        return 77;
    }

    WNDCLASSW wc{};
    wc.hInstance=g.instance;
    wc.lpfnWndProc=proc;
    wc.lpszClassName=kClass;
    if(!RegisterClassW(&wc))return 13;
    HWND shield=CreateWindowExW(
        WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,
        kClass,L"125A layered mask test only",WS_POPUP,
        bounds.left,bounds.top,width,height,
        nullptr,nullptr,g.instance,nullptr);
    if(!shield)return 14;
    if(!SetLayeredWindowAttributes(shield,0,255,LWA_ALPHA)) {
        DestroyWindow(shield);return 15;
    }
    HWND filtered[3]{g.viewer,g.controller,shield};
    if(!MagSetWindowFilterList(g.magnifier,MW_FILTERMODE_EXCLUDE,3,filtered)){
        DestroyWindow(shield);return 16;
    }
    // First show shield above source, then maintain visible magnifier above
    // shield so its output can be sampled. Shield does not touch source HWND.
    SetWindowPos(shield,HWND_TOPMOST,bounds.left,bounds.top,width,height,
        SWP_NOACTIVATE|SWP_SHOWWINDOW);
    SetWindowPos(g.viewer,HWND_TOPMOST,0,0,0,0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
    int matched=0,valid=0;
    for(int n=0;n<20;++n){pump();Sleep(60);}
    POINT maskPoint{bounds.left+width/2,bounds.top+height/2};
    const bool visiblyMasked=similar(read(maskPoint),kColor,12);
    for(const Probe& x:before){
        const COLORREF c=read(x.dest);
        if(c==CLR_INVALID)continue;
        ++valid;
        if(similar(c,x.source))++matched;
    }
    std::cout<<"MASK_LAYERED_EXCLUDE: source_masked="<<(visiblyMasked?1:0)
             <<" original_samples="<<before.size()
             <<" viewer_samples="<<valid<<" matching="<<matched
             <<" actual_percent="<<g.effectivePercent<<"\n";
    // Remove test mask and restore standard exclusions before exiting.
    DestroyWindow(shield);
    HWND regular[2]{g.viewer,g.controller};
    MagSetWindowFilterList(g.magnifier,MW_FILTERMODE_EXCLUDE,2,regular);
    if(!visiblyMasked){
        std::cout<<"MASK_INCONCLUSIVE: shield was not above original GUI\n";
        return 77;
    }
    if(valid<30) {
        std::cout<<"MASK_INCONCLUSIVE: viewer pixels unavailable\n";
        return 77;
    }
    const double ratio=static_cast<double>(matched)/valid;
    if(ratio<0.90) {
        std::cerr<<"MASK_REJECTED: source shield contaminated magnified "
                    "image (white-mirror style failure), agreement="
                 <<ratio<<"\n";
        return 17;
    }
    std::cout<<"MASK_PASS: layered shield covers original while true "
                "plugin pixels remain in enlarged output, agreement="
             <<ratio<<"\n";
    return 0;
}
} // namespace scaler_mask_experiment
