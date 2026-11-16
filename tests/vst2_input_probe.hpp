#pragma once
// Test-only VST2 parameter oracle with genuine OS mouse input through
// the actual 125A PluginScaler magnified window.
// Include after src/vst3_window_scaler.cpp in the integration checker.
#include <cstdint>
#include <cmath>
#include <vector>
#include <iostream>

namespace scaler_vst2_interaction {
constexpr UINT kParamRead = WM_APP + 0x125;

inline void pump_input() {
    MSG event{};
    while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&event);
        DispatchMessageW(&event);
    }
}
inline bool get_parameter(HWND host, int index, int& result) {
    DWORD_PTR v{};
    if (!SendMessageTimeoutW(host,kParamRead,
        static_cast<WPARAM>(static_cast<std::intptr_t>(index)),
        0,SMTO_ABORTIFHUNG | SMTO_BLOCK,1500,&v)) return false;
    result = static_cast<int>(static_cast<std::intptr_t>(v));
    return true;
}
inline std::vector<int> snapshot(HWND host, int count) {
    std::vector<int> values;
    values.reserve(static_cast<size_t>(count));
    for(int i=0;i<count;++i) {
        int v{};
        if(!get_parameter(host,i,v) || v<0) return {};
        values.push_back(v);
    }
    return values;
}
inline void button(bool down) {
    INPUT input{};
    input.type=INPUT_MOUSE;
    input.mi.dwFlags=down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    (void)SendInput(1,&input,sizeof(input));
    pump_input();
}
inline bool viewer_point(int x,int y, POINT& point) {
    if(!IsWindow(g.viewer)||!IsWindow(g.magnifier))return false;
    point={scaler::scaled_pixels(x,g.effectivePercent),
           scaler::scaled_pixels(y,g.effectivePercent)};
    if(!ClientToScreen(g.magnifier,&point)) return false;
    RECT visible{};GetWindowRect(g.magnifier,&visible);
    return point.x>=visible.left && point.x<visible.right &&
           point.y>=visible.top && point.y<visible.bottom;
}
inline int run(HWND host,int sourceW,int sourceH) {
    int count{};
    if(!get_parameter(host,-1,count)){
        std::cerr<<"VST2_INPUT_FAIL: parameter IPC unavailable\n";
        return 11;
    }
    if(count<=0||count>16384){
        std::cerr<<"VST2_INPUT_INCONCLUSIVE: invalid or absent parameters="
                 <<count<<"\n";
        return 77;
    }
    const int checked=(std::min)(count,256);
    const auto baseline=snapshot(host,checked);
    if(baseline.size()!=static_cast<size_t>(checked)){
        std::cerr<<"VST2_INPUT_FAIL: initial parameter snapshot unreadable\n";
        return 12;
    }
    std::cout<<"VST2_INPUT_BEGIN: parameters="<<count
             <<" checked="<<checked<<" zoom="<<g.effectivePercent
             <<" source="<<sourceW<<"x"<<sourceH<<"\n";
    SetWindowPos(g.viewer,HWND_TOPMOST,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetForegroundWindow(g.viewer);
    SetFocus(g.magnifier);
    RECT magRect{};GetWindowRect(g.magnifier,&magRect);
    int gestures=0,changedCount=0,clickChanges=0,dragChanges=0;
    std::vector<bool> observed(static_cast<size_t>(checked),false);
    // Scanning interior points, not the host's title bar. During a drag,
    // x and y are mapped through the actual fitted scale (not the selected
    // combo percentage). The same callbacks a human uses are exercised.
    for(int mode=0;mode<2 && changedCount<3;++mode) {
        const bool drag=mode==1;
        for(int row=1;row<=5 && changedCount<3;++row) {
            for(int col=1;col<=9 && changedCount<3;++col) {
                const int sx=sourceW*col/10;
                const int sy=sourceH*row/6;
                POINT p{};
                if(!viewer_point(sx,sy,p) || !SetCursorPos(p.x,p.y))continue;
                pump_input();Sleep(25);
                const auto before=snapshot(host,checked);
                if(before.size()!=static_cast<size_t>(checked))return 13;
                button(true);
                if(drag) {
                    const int wanted=(row%2) ? -70 : 70;
                    const int endY=(std::max)(magRect.top+8,
                        (std::min)(magRect.bottom-8,p.y+wanted));
                    for(int step=1;step<=8;++step) {
                        SetCursorPos(p.x,p.y+(endY-p.y)*step/8);
                        pump_input();Sleep(15);
                    }
                }
                button(false);
                pump_input();Sleep(90);
                ++gestures;
                const auto after=snapshot(host,checked);
                if(after.size()!=static_cast<size_t>(checked))return 14;
                for(int i=0;i<checked;++i) {
                    if(std::abs(after[i]-before[i])>1000) {
                        if(!observed[static_cast<size_t>(i)]) {
                            observed[static_cast<size_t>(i)]=true;
                            ++changedCount;
                        }
                        if(drag)++dragChanges;else ++clickChanges;
                        std::cout<<"VST2_INPUT_PARAMETER_CHANGED: mode="
                                 <<(drag?"drag":"click")<<" point="<<sx<<","<<sy
                                 <<" index="<<i<<" before="<<before[i]
                                 <<" after="<<after[i]<<"\n";
                    }
                }
            }
        }
    }
    std::cout<<"VST2_INPUT_RESULT: gestures="<<gestures
             <<" distinct_parameters="<<changedCount
             <<" click_changes="<<clickChanges
             <<" drag_changes="<<dragChanges<<"\n";
    if(changedCount==0) {
        std::cerr<<"VST2_INPUT_FAIL: no parameter responded to magnified-window mouse actions\n";
        return 15;
    }
    std::cout<<"VST2_INPUT_PASS: genuine mouse actions in scaled viewer "
                "changed plugin VST2 parameter values\n";
    return 0;
}
}  // namespace scaler_vst2_interaction
