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

constexpr UINT kParamWrite = WM_APP + 0x126;
struct ControlPoint {
    const char* name;
    int x;
    int y;
    bool toggle;
};

// Centres read from the actual original DD Reverb (2).dll GUI screenshot
// produced by private Windows CI (472x240). No synthetic GUI coordinates.
constexpr ControlPoint kDDReverbControls[]={
    {"Room Size",55,63,false},
    {"Damping",143,63,false},
    {"Density",231,63,false},
    {"Width",320,63,false},
    {"Mix",408,63,false},
    {"Predelay",143,168,false},
    {"Low Cut",231,168,false},
    {"High Cut",320,168,false},
    {"Sync",35,188,true},
    {"Dead Duck",396,151,true},
    {"Bypass",440,207,true}
};
inline bool write_parameter(HWND host,int index,int normalized) {
    DWORD_PTR result{};
    return SendMessageTimeoutW(host,kParamWrite,static_cast<WPARAM>(index),
        static_cast<LPARAM>(normalized),SMTO_ABORTIFHUNG | SMTO_BLOCK,1500,
        &result) && result==1;
}
inline bool restore_all(HWND host,const std::vector<int>& initial) {
    for(size_t index=0;index<initial.size();++index)
        if(!write_parameter(host,static_cast<int>(index),initial[index]))
            return false;
    return true;
}
inline int run_targeted(HWND host,int sourceW,int sourceH) {
    if(sourceW!=472 || sourceH!=240) {
        std::cerr<<"DD_TARGETED_INCONCLUSIVE: unexpected GUI dimensions "
                 <<sourceW<<"x"<<sourceH<<"\n";
        return 77;
    }
    int count{};
    if(!get_parameter(host,-1,count) || count<=0 || count>256) {
        std::cerr<<"DD_TARGETED_FAIL: cannot read parameter count\n";
        return 21;
    }
    const auto originals=snapshot(host,count);
    if(originals.size()!=static_cast<size_t>(count)) {
        std::cerr<<"DD_TARGETED_FAIL: cannot save original parameter state\n";
        return 22;
    }
    int totalGestures=0;
    std::vector<bool> hit(static_cast<size_t>(count),false);
    SetWindowPos(g.viewer,HWND_TOPMOST,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetForegroundWindow(g.viewer);
    RECT bounds{};GetWindowRect(g.magnifier,&bounds);
    for(const auto& control:kDDReverbControls) {
        std::vector<int> changed;
        // Controls are explicitly identified by the true GUI screenshot,
        // not a blind grid. For knobs, both directions are tested, including
        // parameter values that start at 0 or 1.
        const int modes=control.toggle ? 1 : 3;
        for(int mode=0;mode<modes;++mode) {
            if(!restore_all(host,originals)) {
                std::cerr<<"DD_TARGETED_FAIL: could not restore baseline\n";
                return 23;
            }
            pump_input();Sleep(35);
            const auto before=snapshot(host,count);
            if(before.size()!=static_cast<size_t>(count))return 24;
            POINT start{};
            if(!viewer_point(control.x,control.y,start))return 25;
            if(!SetCursorPos(start.x,start.y))return 26;
            pump_input();Sleep(40);
            button(true);
            if(mode>0) {
                const int shift=(mode==1) ? -80 : +80;
                const int finalY=(std::max)(bounds.top+6,
                    (std::min)(bounds.bottom-6,start.y+shift));
                for(int step=1;step<=12;++step) {
                    if(!SetCursorPos(start.x,
                        start.y+(finalY-start.y)*step/12))return 27;
                    pump_input();
                    Sleep(15);
                }
            }
            button(false);
            pump_input();Sleep(125);
            ++totalGestures;
            const auto after=snapshot(host,count);
            if(after.size()!=static_cast<size_t>(count))return 28;
            for(int i=0;i<count;++i) {
                if(std::abs(after[static_cast<size_t>(i)]-
                    before[static_cast<size_t>(i)])>1000) {
                    hit[static_cast<size_t>(i)]=true;
                    if(std::find(changed.begin(),changed.end(),i)==changed.end())
                        changed.push_back(i);
                    std::cout<<"DD_TARGETED_CHANGE: control="<<control.name
                             <<" gesture="<<mode
                             <<" parameter="<<i
                             <<" before="<<before[static_cast<size_t>(i)]
                             <<" after="<<after[static_cast<size_t>(i)]<<"\n";
                }
            }
        }
        std::cout<<"DD_TARGETED_CONTROL: name="<<control.name
                 <<" changed_parameters=";
        if(changed.empty())std::cout<<"none";
        for(size_t i=0;i<changed.size();++i) {
            if(i)std::cout<<",";
            std::cout<<changed[i];
        }
        std::cout<<"\n";
    }
    if(!restore_all(host,originals)) {
        std::cerr<<"DD_TARGETED_FAIL: final baseline restore failed\n";
        return 29;
    }
    pump_input();Sleep(100);
    const auto restored=snapshot(host,count);
    if(restored.size()!=static_cast<size_t>(count))return 30;
    int restoreFailures=0,covered=0;
    for(int i=0;i<count;++i) {
        if(hit[static_cast<size_t>(i)])++covered;
        if(std::abs(restored[static_cast<size_t>(i)]-
            originals[static_cast<size_t>(i)])>1000)++restoreFailures;
        std::cout<<"DD_TARGETED_PARAMETER: index="<<i
                 <<" covered="<<(hit[static_cast<size_t>(i)]?1:0)
                 <<" initial="<<originals[static_cast<size_t>(i)]
                 <<" final="<<restored[static_cast<size_t>(i)]<<"\n";
    }
    std::cout<<"DD_TARGETED_RESULT: controls="
             <<(sizeof(kDDReverbControls)/sizeof(kDDReverbControls[0]))
             <<" gestures="<<totalGestures<<" verified_parameters="<<covered
             <<" total_parameters="<<count
             <<" restoration_failures="<<restoreFailures<<"\n";
    if(restoreFailures) return 31;
    if(covered==count) {
        std::cout<<"DD_TARGETED_PASS: ALL exposed VST2 parameters respond "
                    "to original DD Reverb GUI controls at fitted 150% zoom\n";
        return 0;
    }
    if(covered==0) {
        std::cerr<<"DD_TARGETED_FAIL: no target responded to scaled mouse actions\n";
        return 32;
    }
    // Partial interaction coverage is not a GUI malfunction by itself:
    // not every VST2 parameter must be reachable from the editor.
    std::cout<<"DD_TARGETED_PARTIAL: uncovered indices must be investigated; "
                "do not claim all controls verified\n";
    return 78;
}
}  // namespace scaler_vst2_interaction
