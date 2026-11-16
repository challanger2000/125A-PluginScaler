// Real 125A VST3 GUI and actual 125A PluginScaler Windows pixel check.
// This intentionally uses the SAME renderer/source mapping as the user EXE.
// Exit 77=INCONCLUSIVE, no false PASS when desktop pixels are unreadable.
#include <windows.h>
#include <windowsx.h>
#include <magnification.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <iostream>
#include <set>
#include <vector>
#include <fstream>
#include "../src/vst3_window_scaler.cpp"
#include "vst2_input_probe.hpp"
#include "vst2_layered_mask_probe.hpp"
#include "vst2_printwindow_probe.hpp"

namespace {
struct PixelProbe { int x{},y{}; COLORREF color{}; };
void pump() {
    MSG msg{};
    while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
COLORREF desktop_pixel(POINT p) {
    HDC dc=GetDC(nullptr);
    if(!dc)return CLR_INVALID;
    const COLORREF c=GetPixel(dc,p.x,p.y);
    ReleaseDC(nullptr,dc);
    return c;
}
bool similar(COLORREF a,COLORREF b) {
    if(a==CLR_INVALID||b==CLR_INVALID)return false;
    constexpr int kMaxChannelDiff=46;
    return std::abs(int(GetRValue(a))-int(GetRValue(b)))<=kMaxChannelDiff &&
           std::abs(int(GetGValue(a))-int(GetGValue(b)))<=kMaxChannelDiff &&
           std::abs(int(GetBValue(a))-int(GetBValue(b)))<=kMaxChannelDiff;
}
bool capture_desktop_bitmap(const char* filename, const RECT& screen) {
    const int width=screen.right-screen.left;
    const int height=screen.bottom-screen.top;
    if(width<=0||height<=0||width>8192||height>8192)return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=width;
    bi.bmiHeader.biHeight=-height; // top down
    bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    HDC desktop=GetDC(nullptr);
    if(!desktop)return false;
    HDC memory=CreateCompatibleDC(desktop);
    void* pixels=nullptr;
    HBITMAP bitmap=memory?CreateDIBSection(desktop,&bi,DIB_RGB_COLORS,
                              &pixels,nullptr,0):nullptr;
    bool ok=false;
    if(bitmap && pixels){
        HGDIOBJ old=SelectObject(memory,bitmap);
        if(BitBlt(memory,0,0,width,height,desktop,screen.left,screen.top,
                  SRCCOPY|CAPTUREBLT)){
            BITMAPFILEHEADER header{};
            const auto len=static_cast<DWORD>(width*height*4);
            header.bfType=0x4D42;
            header.bfOffBits=sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER);
            header.bfSize=header.bfOffBits+len;
            std::ofstream file(filename,std::ios::binary);
            if(file){
                file.write(reinterpret_cast<const char*>(&header),
                           sizeof(header));
                file.write(reinterpret_cast<const char*>(&bi.bmiHeader),
                           sizeof(bi.bmiHeader));
                file.write(static_cast<const char*>(pixels),
                           static_cast<std::streamsize>(len));
                ok=file.good();
            }
        }
        SelectObject(memory,old);
    }
    if(bitmap)DeleteObject(bitmap);
    if(memory)DeleteDC(memory);
    ReleaseDC(nullptr,desktop);
    return ok;
}
bool within(POINT p, const RECT& r) {
    return p.x>=r.left&&p.y>=r.top&&p.x<r.right&&p.y<r.bottom;
}
}

int main(int argc, char** argv) {
    const bool printWindowTest = argc == 2 && std::string(argv[1]) == "--vst2-printwindow";
    const bool maskTest = argc == 2 && std::string(argv[1]) == "--vst2-mask";
    const bool targeted = argc == 2 && std::string(argv[1]) == "--vst2-controls";
    const bool interaction = targeted || (argc == 2 && std::string(argv[1]) == "--vst2-input");
    const bool vst2 = printWindowTest || maskTest || interaction || (argc == 2 && std::string(argv[1]) == "--vst2");
    const wchar_t* hostClass = vst2 ?
        L"125A_PluginScaler_VST2_EditorHost" :
        L"125A_Test_Lab_P04_EditorHost";
    g.instance=GetModuleHandleW(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!MagInitialize()) {
        std::cout<<"REAL_PLUGIN_INCONCLUSIVE: Magnification API unavailable\n";
        return 77;
    }
    // The *actual* Windows HWND created by 125A Plugin Test Lab.
    HWND host=nullptr;
    for(int n=0;n<200;++n) {
        host=FindWindowW(hostClass,nullptr);
        if(IsWindow(host)&&IsWindowVisible(host)&&GetWindow(host,GW_CHILD))break;
        Sleep(50);
    }
    if(!IsWindow(host)||!GetWindow(host,GW_CHILD)) {
        std::cerr<<"REAL_PLUGIN_FAIL: 125A Test Lab did not expose its real plugin editor\n";
        MagUninitialize();return 2;
    }
    HWND editor=GetWindow(host,GW_CHILD);
    if(!IsWindowVisible(editor)) {
        std::cerr<<"REAL_PLUGIN_FAIL: real VST3 child HWND is not visible\n";
        MagUninitialize();return 3;
    }
    RECT editorBounds{};
    if(!source_bounds(editor,editorBounds)) {
        std::cerr<<"REAL_PLUGIN_FAIL: real VST3 editor has no usable bounds\n";
        MagUninitialize();return 4;
    }
    MONITORINFO mi{};mi.cbSize=sizeof(mi);
    if(!GetMonitorInfoW(MonitorFromWindow(editor,MONITOR_DEFAULTTONEAREST),&mi)) {
        MagUninitialize();return 5;
    }
    RECT display{};
    if(!IntersectRect(&display,&editorBounds,&mi.rcMonitor)) {
        std::cout<<"REAL_PLUGIN_INCONCLUSIVE: plugin editor is entirely off screen\n";
        MagUninitialize();return 77;
    }
    const int width=editorBounds.right-editorBounds.left;
    const int height=editorBounds.bottom-editorBounds.top;
    // Read original composited desktop pixels before opening our viewer.
    // For large plugins, only use source coordinates visible on the monitor.
    std::vector<PixelProbe> samples;
    std::set<COLORREF> distinct;
    for(int row=1;row<=13;++row)for(int col=1;col<=19;++col) {
        const int sx=width*col/20;
        const int sy=height*row/14;
        POINT p{editorBounds.left+sx,editorBounds.top+sy};
        if(!within(p,mi.rcMonitor))continue;
        const COLORREF c=desktop_pixel(p);
        if(c!=CLR_INVALID) {
            samples.push_back({sx,sy,c});
            distinct.insert(c);
        }
    }
    if(interaction) {
        if(capture_desktop_bitmap("vst2-source-before.bmp",editorBounds))
            std::cout<<"VST2_SOURCE_BITMAP_WRITTEN\\n";
    }
    if(samples.size()<24 || distinct.size()<6) {
        std::cout<<"REAL_PLUGIN_INCONCLUSIVE: real editor pixels are not readable/diverse "
                 <<"samples="<<samples.size()<<" distinct="<<distinct.size()<<"\n";
        MagUninitialize();return 77;
    }

    WNDCLASSW wc{};
    wc.hInstance=g.instance;
    wc.lpfnWndProc=viewer_proc;
    wc.lpszClassName=kViewerClass;
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&wc)) {
        std::cerr<<"REAL_PLUGIN_FAIL: scaler viewer class registration\n";
        MagUninitialize();return 6;
    }
    g.source=editor;
    g.sourceThread=GetWindowThreadProcessId(editor,&g.sourcePid);
    g.sourceRect=editorBounds;
    g.percent=150;
    g.autoFit=true;
    g.overlay=false;
    create_viewer();
    if(!IsWindow(g.viewer)||!IsWindow(g.magnifier)) {
        std::cerr<<"REAL_PLUGIN_FAIL: actual scaler viewer creation\n";
        MagUninitialize();return 7;
    }
    // Allow Direct2D/VSTGUI + Magnification refresh to settle.
    for(int i=0;i<25;++i){pump();Sleep(40);}
    int matched=0,valid=0;
    const int zoom=g.effectivePercent;
    if (zoom <= 100) {
        std::cout << "REAL_PLUGIN_INCONCLUSIVE: product was reduced to "
                  << zoom << "% by Auto-Fit, not enlarged; smaller GUI or "
                     "larger desktop required\n";
        if(IsWindow(g.viewer))DestroyWindow(g.viewer);
        MagUninitialize();
        return 77;
    }
    if(printWindowTest) {
        const int code=scaler_printwindow_experiment::run(editor);
        if(IsWindow(g.viewer))DestroyWindow(g.viewer);
        MagUninitialize();
        return code;
    }
    if(maskTest) {
        // The source and enlarged viewer cannot share the same physical
        // pixels during our mask experiment. The isolated experiment
        // repositions the viewer, then takes its own clean baseline.
        const int code=scaler_mask_experiment::run(editor);
        if(IsWindow(g.viewer))DestroyWindow(g.viewer);
        MagUninitialize();
        return code;
    }
    RECT viewerRect{};GetWindowRect(g.magnifier,&viewerRect);
    if(interaction) {
        if(capture_desktop_bitmap("vst2-magnified-before.bmp",viewerRect))
            std::cout<<"VST2_MAGNIFIED_BITMAP_WRITTEN\\n";
    }
    for(const auto& s:samples) {
        POINT mapped{scaler::scaled_pixels(s.x,zoom),
                     scaler::scaled_pixels(s.y,zoom)};
        ClientToScreen(g.magnifier,&mapped);
        if(!within(mapped,viewerRect)||!within(mapped,mi.rcMonitor))continue;
        const COLORREF c=desktop_pixel(mapped);
        if(c==CLR_INVALID)continue;
        ++valid;
        if(similar(c,s.color))++matched;
    }
    const double agreement=valid?double(matched)/valid:0.0;
    std::cout<<"REAL_PLUGIN_CAPTURE: source="<<width<<"x"<<height
             <<" requested=150 actual="<<zoom
             <<" samples="<<samples.size()<<" valid="<<valid
             <<" matched="<<matched<<" agreement="<<agreement<<"\n";
    if(valid<24) {
        std::cout<<"REAL_PLUGIN_INCONCLUSIVE: insufficient visible magnified pixels\n";
        return 77;
    }
    if(agreement<0.65) {
        std::cerr<<"REAL_PLUGIN_FAIL: real editor diverged from its magnified pixels\n";
        return 8;
    }
    std::cout<<"REAL_PLUGIN_PASS: actual plugin editor and enlarged pixels agree\n";
    const int code = targeted ?
        scaler_vst2_interaction::run_targeted(host,width,height) :
        interaction ? scaler_vst2_interaction::run(host,width,height) : 0;
    if(IsWindow(g.viewer))DestroyWindow(g.viewer);
    MagUninitialize();
    return code;
}
