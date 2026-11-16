#pragma once
// Independent capture-backend feasibility probe on genuine DD Reverb GUI.
// Does not modify the shipping PluginScaler. A passing result only applies
// to the tested VST2 GDI editor, not to every GPU-rendered VST.
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace scaler_printwindow_experiment {
constexpr wchar_t kCoverClass[]=L"125A.Scaler.QA.CoverForPrintWindow";
constexpr COLORREF kCoverColor=RGB(240,25,200);
LRESULT CALLBACK cover_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_PAINT){
        PAINTSTRUCT ps{};
        HDC dc=BeginPaint(w,&ps);
        RECT r{};GetClientRect(w,&r);
        HBRUSH b=CreateSolidBrush(kCoverColor);
        FillRect(dc,&r,b);
        DeleteObject(b);
        EndPaint(w,&ps);
        return 0;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
void pump(){
    MSG m{};
    while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
COLORREF desktop(int x,int y){
    HDC dc=GetDC(nullptr);
    if(!dc)return CLR_INVALID;
    const COLORREF c=GetPixel(dc,x,y);
    ReleaseDC(nullptr,dc);
    return c;
}
bool pixel_near(COLORREF a,COLORREF b,int tolerance=48){
    if(a==CLR_INVALID || b==CLR_INVALID)return false;
    return std::abs(int(GetRValue(a))-int(GetRValue(b)))<=tolerance &&
           std::abs(int(GetGValue(a))-int(GetGValue(b)))<=tolerance &&
           std::abs(int(GetBValue(a))-int(GetBValue(b)))<=tolerance;
}
struct Sample{int x,y;COLORREF color;};
struct Capture {
    HDC memory{};
    HBITMAP bitmap{};
    HBITMAP old{};
    void* bits{};
    int width{},height{};
    bool init(int w,int h) {
        width=w;height=h;
        HDC desktopDC=GetDC(nullptr);
        if(!desktopDC)return false;
        memory=CreateCompatibleDC(desktopDC);
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;
        info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;
        info.bmiHeader.biBitCount=32;
        info.bmiHeader.biCompression=BI_RGB;
        if(memory)bitmap=CreateDIBSection(desktopDC,&info,DIB_RGB_COLORS,
                                             &bits,nullptr,0);
        if(bitmap)old=static_cast<HBITMAP>(SelectObject(memory,bitmap));
        ReleaseDC(nullptr,desktopDC);
        return bitmap && memory && bits;
    }
    ~Capture(){
        if(memory && old)SelectObject(memory,old);
        if(bitmap)DeleteObject(bitmap);
        if(memory)DeleteDC(memory);
    }
    COLORREF at(int x,int y) const {
        if(!bits || x<0 || x>=width || y<0 || y>=height)return CLR_INVALID;
        const unsigned char* row=static_cast<const unsigned char*>(bits)+
                                  4*(static_cast<size_t>(y)*width+x);
        return RGB(row[2],row[1],row[0]);
    }
    bool read(HWND hwnd,UINT flags) {
        if(!bits)return false;
        std::fill_n(static_cast<unsigned char*>(bits),
                    static_cast<size_t>(width)*height*4,static_cast<unsigned char>(0));
        return PrintWindow(hwnd,memory,flags)!=FALSE;
    }
};
int run(HWND source) {
    RECT r{};
    if(!source_bounds(source,r))return 11;
    const int w=r.right-r.left,h=r.bottom-r.top;
    if(w<40||h<40)return 12;
    // Temporarily hide viewer so desktop-reference pixels are valid.
    ShowWindow(g.viewer,SW_HIDE);
    for(int n=0;n<12;++n){pump();Sleep(50);}
    std::vector<Sample> base;
    for(int row=1;row<11;++row)for(int col=1;col<15;++col){
        const int x=w*col/15,y=h*row/11;
        const COLORREF c=desktop(r.left+x,r.top+y);
        if(c!=CLR_INVALID)base.push_back({x,y,c});
    }
    if(base.size()<50) {
        std::cout<<"PRINTWINDOW_INCONCLUSIVE: source baseline unavailable\n";
        return 77;
    }
    Capture cap;
    if(!cap.init(w,h))return 13;
    auto compare=[&](const char* label,UINT flags)->int{
        const bool ok=cap.read(source,flags);
        int matching=0;
        for(const auto& point:base)
            if(pixel_near(cap.at(point.x,point.y),point.color))++matching;
        std::cout<<"PRINTWINDOW_PHASE: "<<label
                 <<" api_return="<<(ok?1:0)
                 <<" matched="<<matching
                 <<" samples="<<base.size()
                 <<" ratio="<<static_cast<double>(matching)/base.size()
                 <<" flags="<<flags<<"\n";
        return matching;
    };
    // Baseline capture should agree before any occlusion.
    const int unblocked=compare("visible",2);
    if(unblocked<static_cast<int>(base.size()*0.90)) {
        std::cout<<"PRINTWINDOW_REJECTED: cannot capture visible editor reliably\n";
        ShowWindow(g.viewer,SW_SHOW);
        return 14;
    }
    WNDCLASSW wc{};
    wc.hInstance=g.instance;
    wc.lpfnWndProc=cover_proc;
    wc.lpszClassName=kCoverClass;
    if(!RegisterClassW(&wc))return 15;
    HWND mask=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW,
        kCoverClass,L"QA only",WS_POPUP,r.left,r.top,w,h,
        nullptr,nullptr,g.instance,nullptr);
    if(!mask)return 16;
    SetWindowPos(mask,HWND_TOPMOST,r.left,r.top,w,h,
        SWP_SHOWWINDOW|SWP_NOACTIVATE);
    for(int n=0;n<8;++n){pump();Sleep(40);}
    const bool visiblyCovered=pixel_near(desktop(r.left+w/2,r.top+h/2),kCoverColor,12);
    const int occluded=compare("occluded",2);
    DestroyWindow(mask);
    // Now hide the top-level plugin host. If the child's content can
    // still be captured, this is a real window-hidden backend candidate.
    HWND root=GetAncestor(source,GA_ROOT);
    const bool rootVisible=IsWindowVisible(root)!=FALSE;
    ShowWindow(root,SW_HIDE);
    for(int n=0;n<8;++n){pump();Sleep(40);}
    const int hidden=compare("host-hidden",2);
    // A still image alone is insufficient: mutate a genuine VST2
    // knob while the parent editor remains SW_HIDE'd.
    DWORD_PTR beforeParameter{};
    const bool readOk=SendMessageTimeoutW(root,WM_APP+0x125,5,0,
         SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&beforeParameter)!=FALSE;
    const int previous=static_cast<int>(beforeParameter);
    const int changedTo=previous>700000?250000:850000;
    std::vector<COLORREF> knobBefore;
    for(int y=25;y<105;++y)for(int x=15;x<100;++x)
        knobBefore.push_back(cap.at(x,y));
    DWORD_PTR written{};
    const bool writeOk=readOk &&
        SendMessageTimeoutW(root,WM_APP+0x126,5,
            static_cast<LPARAM>(changedTo),
            SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&written)!=FALSE;
    for(int n=0;n<8;++n){pump();Sleep(60);}
    const bool repainted=cap.read(source,2);
    int changedPixels=0;
    size_t offset=0;
    for(int y=25;y<105;++y)for(int x=15;x<100;++x) {
        if(!pixel_near(cap.at(x,y),knobBefore[offset],3))++changedPixels;
        ++offset;
    }
    DWORD_PTR restoredResult{};
    const bool restoreOk=writeOk &&
        SendMessageTimeoutW(root,WM_APP+0x126,5,
            static_cast<LPARAM>(previous),
            SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&restoredResult)!=FALSE;
    std::cout<<"PRINTWINDOW_HIDDEN_DYNAMIC: read="<<(readOk?1:0)
             <<" write="<<(writeOk?1:0)
             <<" captured="<<(repainted?1:0)
             <<" changed_pixels="<<changedPixels
             <<" restore="<<(restoreOk?1:0)
             <<" value_before="<<previous
             <<" value_during="<<changedTo<<"\n";
    if(rootVisible)ShowWindow(root,SW_SHOWNA);
    ShowWindow(g.viewer,SW_SHOWNA);
    pump();
    const double total=static_cast<double>(base.size());
    std::cout<<"PRINTWINDOW_RESULT: cover_visible="<<(visiblyCovered?1:0)
             <<" baseline="<<unblocked<<"/"<<base.size()
             <<" occluded="<<occluded<<"/"<<base.size()
             <<" hidden="<<hidden<<"/"<<base.size()<<"\n";
    if(!visiblyCovered){
        std::cout<<"PRINTWINDOW_INCONCLUSIVE: cover did not actually obscure editor\n";
        return 77;
    }
    if(hidden/total>=0.90) {
        if(!readOk || !writeOk || !repainted || !restoreOk ||
           changedPixels<20) {
            std::cout<<"PRINTWINDOW_HIDDEN_STATIC_ONLY: no reliable dynamic "
                        "repaint proven after hidden knob parameter update\n";
            return 21;
        }
        std::cout<<"PRINTWINDOW_HIDDEN_LIVE_PASS: original hidden editor "
                    "repaints dynamically through PrintWindow\n";
        return 0;
    }
    if(occluded/total>=0.90) {
        std::cout<<"PRINTWINDOW_OCCLUDED_ONLY: capture survives masking but not SW_HIDE\n";
        return 19;
    }
    std::cout<<"PRINTWINDOW_NO_GO: original editor cannot be captured while "
                "visually obscured or hidden\n";
    return 20;
}
}
