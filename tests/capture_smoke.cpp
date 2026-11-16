// Headless-safe visual smoke: exercises the *real* 125A viewer code with
// a deterministic GDI source window, rather than a synthetic scaling formula.
// Exit 77 = desktop capture unavailable (INCONCLUSIVE, never a PASS).
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../src/vst3_window_scaler.cpp"
#include <iostream>

namespace {
constexpr wchar_t kFixtureClass[] = L"125A.Scaler.CaptureFixture";
const COLORREF kColor = RGB(20, 180, 65);

LRESULT CALLBACK fixture_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT r{}; GetClientRect(h,&r);
        HBRUSH b = CreateSolidBrush(kColor);
        FillRect(dc,&r,b);
        DeleteObject(b);
        EndPaint(h,&ps);
        return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}

bool color_like(COLORREF c, COLORREF want) {
    if(c == CLR_INVALID) return false;
    return (std::abs(int(GetRValue(c))-int(GetRValue(want))) <= 25 &&
           std::abs(int(GetGValue(c))-int(GetGValue(want))) <= 25 &&
           std::abs(int(GetBValue(c))-int(GetBValue(want))) <= 25);
}

COLORREF pixel_at(int x,int y) {
    HDC dc = GetDC(nullptr);
    if(!dc) return CLR_INVALID;
    const COLORREF color=GetPixel(dc,x,y);
    ReleaseDC(nullptr,dc);
    return color;
}

void pump() {
    MSG m{};
    while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
}

int main() {
    g.instance = GetModuleHandleW(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!MagInitialize()) {
        std::cout << "VISUAL_INCONCLUSIVE: Magnification unavailable\n";
        return 77;
    }
    WNDCLASSW fx{};
    fx.hInstance = g.instance;
    fx.lpfnWndProc = fixture_proc;
    fx.lpszClassName = kFixtureClass;
    WNDCLASSW view = fx;
    view.lpfnWndProc = viewer_proc;
    view.lpszClassName = kViewerClass;
    if (!RegisterClassW(&fx) || !RegisterClassW(&view)) return 2;
    HWND source = CreateWindowExW(0,kFixtureClass,L"125A mock source",
        WS_POPUP|WS_VISIBLE,80,70,240,160,nullptr,nullptr,g.instance,nullptr);
    if(!source) return 3;
    ShowWindow(source,SW_SHOW);
    UpdateWindow(source);
    g.source=source;
    g.sourceThread=GetWindowThreadProcessId(source,&g.sourcePid);
    if(!source_bounds(source,g.sourceRect))return 4;
    g.percent=150;
    g.overlay=false;
    g.autoFit=true;
    create_viewer();
    if(!IsWindow(g.viewer)||!IsWindow(g.magnifier))return 5;

    POINT original{120,80};
    ClientToScreen(source,&original);
    COLORREF sourceColor=CLR_INVALID, magnifierColor=CLR_INVALID;
    bool sourcePainted=false, magPainted=false;
    for(int attempt=0;attempt<40;++attempt){
        pump();
        sourceColor=pixel_at(original.x,original.y);
        if(color_like(sourceColor,kColor)) sourcePainted=true;
        POINT displayed{scaler::scaled_pixels(120,g.effectivePercent),
                        scaler::scaled_pixels(80,g.effectivePercent)};
        ClientToScreen(g.magnifier,&displayed);
        magnifierColor=pixel_at(displayed.x,displayed.y);
        if(color_like(magnifierColor,kColor))magPainted=true;
        if(sourcePainted&&magPainted)break;
        Sleep(50);
    }
    // Exclude viewer/controller but no additional mask window may exist.
    const HWND forbiddenMask=FindWindowW(L"125A.VST3Scaler.SourceShield",nullptr);
    const bool noMask=forbiddenMask==nullptr;
    if(IsWindow(g.viewer))DestroyWindow(g.viewer);
    if(IsWindow(source))DestroyWindow(source);
    MagUninitialize();
    if(!noMask){
        std::cerr<<"VISUAL_FAIL: original source was obscured with a shield HWND\n";
        return 6;
    }
    if(!sourcePainted){
        std::cout<<"VISUAL_INCONCLUSIVE: interactive Windows desktop not available; "
                 <<"actual source pixels not readable\n";
        return 77;
    }
    if(!magPainted){
        std::cerr<<"VISUAL_FAIL: source was visible, but viewer did not display "
                 <<"the fixture color; sourcePixel="<<sourceColor
                 <<" viewerPixel="<<magnifierColor<<"\n";
        return 7;
    }
    std::cout<<"VISUAL_PASS: source pixels and scaled viewer pixels agree\n";
    return 0;
}
