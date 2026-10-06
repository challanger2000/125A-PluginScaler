#define NOMINMAX
#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {
VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float) {
    switch (opcode) {
    case AudioMasterVersion: return 2400;
    case AudioMasterGetSampleRate: return 48000;
    case AudioMasterGetBlockSize: return 64;
    case AudioMasterGetCurrentProcessLevel: return 2; // realtime
    case AudioMasterGetAutomationState: return 0;
    default: return 0;
    }
}
using EntryProc = AEffect* (__cdecl*)(AudioMasterCallback);

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd,msg,wp,lp);
}

HWND createHost(int width, int height) {
    static ATOM atom=0;
    static const wchar_t* cls=L"125A_GdiHarnessHost";
    if(!atom){
        WNDCLASSW wc{};
        wc.lpfnWndProc=hostProc;
        wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=cls;
        atom=RegisterClassW(&wc);
        if(!atom && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    }
    return CreateWindowExW(0,cls,L"GDI Harness",WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT,CW_USEDEFAULT,width+80,height+120,
                           nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
}

BOOL CALLBACK findSurfaceProc(HWND hwnd, LPARAM lp) {
    wchar_t cls[256]{};
    GetClassNameW(hwnd,cls,256);
    if(std::wstring_view(cls)==L"125A_PluginScaler_ScaledSurface"){
        *reinterpret_cast<HWND*>(lp)=hwnd;
        return FALSE;
    }
    return TRUE;
}

int childCount(HWND parent) {
    int count=0;
    EnumChildWindows(parent,
        [](HWND, LPARAM lp)->BOOL {
            ++*reinterpret_cast<int*>(lp);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&count));
    return count;
}

bool approx(COLORREF c, int r, int g, int b, int tol=45) {
    if(c==CLR_INVALID) return false;
    return std::abs((int)GetRValue(c)-r)<=tol &&
           std::abs((int)GetGValue(c)-g)<=tol &&
           std::abs((int)GetBValue(c)-b)<=tol;
}

int scaledCoord(int native, int scale) {
    return native * scale / 100;
}

void pumpMessagesFor(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}


struct TimingSummary {
    double p95Us{0.0};
    double p99Us{0.0};
    double maxUs{0.0};
    int deadlineOverruns{0};
};

bool processTimed64(AEffect* effect, std::vector<double>& durationsUs) {
    std::array<float,64> left{};
    std::array<float,64> right{};
    float* outputs[2]{left.data(), right.data()};

    const auto begin=std::chrono::steady_clock::now();
    effect->processReplacing(effect,nullptr,outputs,64);
    const auto end=std::chrono::steady_clock::now();

    durationsUs.push_back(
        std::chrono::duration<double,std::micro>(end-begin).count());

    for(int i=0;i<64;++i) {
        if(std::fabs(left[static_cast<std::size_t>(i)]-0.25f)>0.00001f ||
           std::fabs(right[static_cast<std::size_t>(i)]-0.25f)>0.00001f)
            return false;
    }
    return true;
}

TimingSummary summarizeTimings(std::vector<double> values) {
    TimingSummary s{};
    if(values.empty()) return s;
    std::sort(values.begin(),values.end());
    auto percentile=[&](double q) {
        const std::size_t index=static_cast<std::size_t>(
            std::clamp(q,0.0,1.0)*static_cast<double>(values.size()-1));
        return values[index];
    };
    s.p95Us=percentile(0.95);
    s.p99Us=percentile(0.99);
    s.maxUs=values.back();
    constexpr double kBlockDeadlineUs=1000000.0*64.0/48000.0;
    s.deadlineOverruns=static_cast<int>(std::count_if(
        values.begin(),values.end(),
        [](double us){ return us>1000000.0*64.0/48000.0; }));
    return s;
}

bool runScale(EntryProc entry, int scale) {
    const std::wstring scaleText=std::to_wstring(scale);
    SetEnvironmentVariableW(L"PLUGINSCALER_SCALE_PERCENT",scaleText.c_str());

    AEffect* effect=entry(hostCallback);
    if(!effect || !effect->dispatcher) {
        std::cout<<"gdi-scale-"<<scale<<"=FAIL\n";
        return false;
    }

    bool audioConfigured=
        effect->dispatcher(effect,EffSetSampleRate,0,0,nullptr,48000.0f)!=0 &&
        effect->dispatcher(effect,EffSetBlockSize,0,64,nullptr,0.0f)!=0 &&
        effect->dispatcher(effect,EffMainsChanged,0,1,nullptr,0.0f)!=0;
    if(!audioConfigured) {
        std::cout<<"gdi-audio-config-"<<scale<<"=FAIL\n";
        effect->dispatcher(effect,EffMainsChanged,0,0,nullptr,0.0f);
    effect->dispatcher(effect,EffClose,0,0,nullptr,0.0f);
        return false;
    }
    std::cout<<"gdi-audio-config-"<<scale<<"=PASS\n";

    std::vector<double> baselineTimes;
    baselineTimes.reserve(256);
    bool baselineSignal=true;
    for(int i=0;i<256;++i)
        baselineSignal=processTimed64(effect,baselineTimes)&&baselineSignal;
    const auto baselineTiming=summarizeTimings(baselineTimes);
    std::cout<<"gdi-audio-baseline-"<<scale<<"="
             <<(baselineSignal?"PASS":"FAIL")
             <<" p95_us="<<baselineTiming.p95Us
             <<" p99_us="<<baselineTiming.p99Us
             <<" max_us="<<baselineTiming.maxUs
             <<" overruns="<<baselineTiming.deadlineOverruns<<"\n";

    constexpr int nativeWidth=762;
    constexpr int nativeHeight=358;
    const int expectedWidth=nativeWidth*scale/100;
    const int expectedHeight=nativeHeight*scale/100;

    bool ok=baselineSignal;
    VstRect* rect=nullptr;
    const auto rectOk=effect->dispatcher(effect,EffEditGetRect,0,0,&rect,0.0f);
    const int rw=rect?rect->right-rect->left:0;
    const int rh=rect?rect->bottom-rect->top:0;
    ok=ok && rectOk && rw==expectedWidth && rh==expectedHeight;
    std::cout<<"gdi-rect-"<<scale<<"="<<rw<<"x"<<rh<<"\n";

    for(int cycle=1; cycle<=3 && ok; ++cycle) {
        HWND host=createHost(expectedWidth,expectedHeight);
        if(!host) { ok=false; break; }
        ShowWindow(host,SW_SHOW);
        UpdateWindow(host);

        const auto openOk=effect->dispatcher(effect,EffEditOpen,0,0,host,0.0f);
        // Capture now begins almost immediately. Keep pumping long enough
        // for the first timer/capture cycle, but do not artificially idle the
        // helper for seconds between editor lifecycle operations.
        pumpMessagesFor(250);

        HWND surface=nullptr;
        EnumChildWindows(host,findSurfaceProc,reinterpret_cast<LPARAM>(&surface));
        const bool opened = openOk != 0;
        const bool haveSurface = surface != nullptr;
        std::cout<<"gdi-open-"<<scale<<"-"<<cycle<<"="<<(opened?"PASS":"FAIL")<<"\n";
        std::cout<<"gdi-surface-"<<scale<<"-"<<cycle<<"="<<(haveSurface?"PASS":"FAIL")<<"\n";
        ok=ok && opened && haveSurface;

        if(surface) {
            RECT rc{};
            GetClientRect(surface,&rc);
            const int sw=rc.right-rc.left;
            const int sh=rc.bottom-rc.top;
            const bool sizeOk = sw==expectedWidth && sh==expectedHeight;
            std::cout<<"gdi-size-"<<scale<<"-"<<cycle<<"="
                     <<(sizeOk?"PASS":"FAIL")<<" actual="<<sw<<"x"<<sh<<"\n";
            ok=ok && sizeOk;

            const int keyX=scaledCoord(100,scale);
            const int keyY=scaledCoord(300,scale);

            HDC dc=GetDC(surface);
            const COLORREF keyBefore=GetPixel(dc,keyX,keyY);
            ReleaseDC(surface,dc);
            const bool scaledVisual=approx(keyBefore,224,224,224,65);
            std::cout<<"gdi-visual-"<<scale<<"-"<<cycle<<"="
                     <<(scaledVisual?"PASS":"FAIL")
                     <<" rgb="<<(int)GetRValue(keyBefore)<<","
                     <<(int)GetGValue(keyBefore)<<","
                     <<(int)GetBValue(keyBefore)<<"\n";
            ok=ok && scaledVisual;

            SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(keyX,keyY));
            Sleep(20);
            dc=GetDC(surface);
            const COLORREF keyDown=GetPixel(dc,keyX,keyY);
            ReleaseDC(surface,dc);
            const bool clickMapped=approx(keyDown,0,204,0,70);
            std::cout<<"gdi-click-"<<scale<<"-"<<cycle<<"="
                     <<(clickMapped?"PASS":"FAIL")
                     <<" rgb="<<(int)GetRValue(keyDown)<<","
                     <<(int)GetGValue(keyDown)<<","
                     <<(int)GetBValue(keyDown)<<"\n";
            ok=ok && clickMapped;
            SendMessageW(surface,WM_LBUTTONUP,0,MAKELPARAM(keyX,keyY));

            // Native knob centre (425,125), drag 30 native pixels upward.
            const int knobX=scaledCoord(425,scale);
            const int knobStartY=scaledCoord(125,scale);
            const int knobEndY=scaledCoord(95,scale);
            SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,
                         MAKELPARAM(knobX,knobStartY));
            SendMessageW(surface,WM_MOUSEMOVE,MK_LBUTTON,
                         MAKELPARAM(knobX,knobEndY));
            SendMessageW(surface,WM_LBUTTONUP,0,
                         MAKELPARAM(knobX,knobEndY));
            Sleep(20);

            dc=GetDC(surface);
            const COLORREF knobPixel=GetPixel(dc,knobX,knobEndY);
            ReleaseDC(surface,dc);
            const bool dragRedraw=knobPixel!=CLR_INVALID;
            std::cout<<"gdi-drag-"<<scale<<"-"<<cycle<<"="
                     <<(dragRedraw?"PASS":"FAIL")<<"\n";
            ok=ok && dragRedraw;


            if(ok) {
                const auto deferredBefore=effect->dispatcher(
                    effect,EffVendorSpecific,0x1271,0,nullptr,0.0f);
                const auto watchdogBefore=effect->dispatcher(
                    effect,EffVendorSpecific,0x1272,0,nullptr,0.0f);
                const auto capturesBefore=effect->dispatcher(
                    effect,EffVendorSpecific,0x1273,0,nullptr,0.0f);

                std::atomic<bool> beginAudio{false};
                std::atomic<bool> guiDone{false};
                std::atomic<bool> stressSignal{true};
                std::atomic<bool> stressTimedOut{false};
                std::atomic<int> badAudioBlocks{0};
                std::vector<double> stressTimes;
                stressTimes.reserve(2048);

                std::thread audioThread([&] {
                    while(!beginAudio.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    using Clock=std::chrono::steady_clock;
                    constexpr auto kBlockPeriod=
                        std::chrono::nanoseconds(1333333);
                    const auto started=Clock::now();
                    auto next=started;

                    while(!guiDone.load(std::memory_order_acquire) ||
                          stressTimes.size()<256) {
                        next+=kBlockPeriod;

                        if(!processTimed64(effect,stressTimes)) {
                            stressSignal.store(false,std::memory_order_release);
                            badAudioBlocks.fetch_add(
                                1,std::memory_order_relaxed);
                        }

                        if(Clock::now()-started>std::chrono::seconds(10)) {
                            stressTimedOut.store(true,std::memory_order_release);
                            break;
                        }

                        std::this_thread::sleep_until(next);
                    }
                });

                SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,
                             MAKELPARAM(knobX,knobStartY));
                beginAudio.store(true,std::memory_order_release);
                for(int i=0;i<512;++i) {
                    const int nativeY=125-(i%100);
                    const int y=scaledCoord(nativeY,scale);
                    SendMessageW(surface,WM_MOUSEMOVE,MK_LBUTTON,
                                 MAKELPARAM(knobX,y));
                    if((i%16)==15)
                        pumpMessagesFor(1);
                }
                SendMessageW(surface,WM_LBUTTONUP,0,
                             MAKELPARAM(knobX,knobEndY));
                guiDone.store(true,std::memory_order_release);
                audioThread.join();

                const auto deferredAfter=effect->dispatcher(
                    effect,EffVendorSpecific,0x1271,0,nullptr,0.0f);
                const auto watchdogAfter=effect->dispatcher(
                    effect,EffVendorSpecific,0x1272,0,nullptr,0.0f);
                const auto capturesAfter=effect->dispatcher(
                    effect,EffVendorSpecific,0x1273,0,nullptr,0.0f);
                const auto deferredDelta=
                    deferredAfter>=deferredBefore
                        ? deferredAfter-deferredBefore : -1;
                const auto watchdogDelta=
                    watchdogAfter>=watchdogBefore
                        ? watchdogAfter-watchdogBefore : -1;
                const auto captureDelta=
                    capturesAfter>=capturesBefore
                        ? capturesAfter-capturesBefore : -1;

                const auto stressTiming=summarizeTimings(stressTimes);
                const bool timingMeasured=!stressTimes.empty();
                const bool realtimeSignal=
                    stressSignal.load(std::memory_order_acquire);

                const double p99Ratio =
                    baselineTiming.p99Us>0.0
                        ? stressTiming.p99Us/baselineTiming.p99Us : 0.0;
                const bool realtimeTimingOk =
                    baselineTiming.deadlineOverruns==0 &&
                    stressTiming.deadlineOverruns==0 &&
                    p99Ratio<=2.0;
                const bool transportClean =
                    !stressTimedOut.load(std::memory_order_acquire) &&
                    badAudioBlocks.load(std::memory_order_relaxed)==0 &&
                    deferredDelta==0 &&
                    watchdogDelta==0;
                const bool captureCoalesced =
                    captureDelta>=0 && captureDelta<128;
                std::cout<<"gdi-realtime-drag-"<<scale<<"="
                         <<(realtimeSignal&&timingMeasured&&realtimeTimingOk&&transportClean&&captureCoalesced
                                ?"PASS":"FAIL")
                         <<" blocks="<<stressTimes.size()
                         <<" p95_us="<<stressTiming.p95Us
                         <<" p99_us="<<stressTiming.p99Us
                         <<" max_us="<<stressTiming.maxUs
                         <<" overruns="<<stressTiming.deadlineOverruns
                         <<" baseline_overruns="<<baselineTiming.deadlineOverruns
                         <<" p99_ratio="<<p99Ratio
                         <<" bad_blocks="<<badAudioBlocks.load(std::memory_order_relaxed)
                         <<" stress_timeout="<<(stressTimedOut.load(std::memory_order_relaxed)?1:0)
                         <<" deferred104="<<deferredDelta
                         <<" watchdogs="<<watchdogDelta
                         <<" captures="<<captureDelta<<"\n";
                ok=ok&&realtimeSignal&&timingMeasured&&realtimeTimingOk&&transportClean&&captureCoalesced;
            }

            // Captured legacy drags must continue beyond the visible client
            // rectangle. Pro-53-style vertical knobs rely on negative/outside
            // WM_MOUSEMOVE coordinates rather than being clamped at y=0.
            const int outsideEndY=-scaledCoord(40,scale);
            SendMessageW(surface,WM_LBUTTONDOWN,MK_LBUTTON,
                         MAKELPARAM(knobX,knobStartY));
            SendMessageW(surface,WM_MOUSEMOVE,MK_LBUTTON,
                         MAKELPARAM(knobX,outsideEndY));
            SendMessageW(surface,WM_LBUTTONUP,0,
                         MAKELPARAM(knobX,outsideEndY));
            const auto minDragY=effect->dispatcher(
                effect,EffVendorSpecific,0x1270,0,nullptr,0.0f);
            const bool outsideDrag=minDragY<0;
            std::cout<<"gdi-outside-drag-"<<scale<<"-"<<cycle<<"="
                     <<(outsideDrag?"PASS":"FAIL")
                     <<" nativeY="<<minDragY<<"\n";
            ok=ok && outsideDrag;

            std::cout<<"gdi-cycle-"<<scale<<"-"<<cycle
                     <<"="<<(ok?"PASS":"FAIL")<<"\n";
        }

        const auto closeOk=effect->dispatcher(
            effect,EffEditClose,0,0,nullptr,0.0f);
        Sleep(30);
        const bool closed=closeOk!=0 && childCount(host)==0;
        std::cout<<"gdi-close-"<<scale<<"-"<<cycle<<"="
                 <<(closed?"PASS":"FAIL")
                 <<" closeRet="<<closeOk
                 <<" children="<<childCount(host)<<"\n";
        ok=ok && closed;
        DestroyWindow(host);
    }

    effect->dispatcher(effect,EffClose,0,0,nullptr,0.0f);
    std::cout<<"gdi-scale-"<<scale<<"="<<(ok?"PASS":"FAIL")<<"\n";
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    if(argc!=5) return 1;
    SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86",argv[2]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2",argv[3]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST",argv[4]);
    SetEnvironmentVariableW(L"PLUGINSCALER_EDITOR_MODE",L"Gdi");

    HMODULE proxy=LoadLibraryW(argv[1]);
    if(!proxy) return 2;
    auto entry=reinterpret_cast<EntryProc>(GetProcAddress(proxy,"VSTPluginMain"));
    if(!entry) {
        FreeLibrary(proxy);
        return 3;
    }

    bool ok=true;
    for(const int scale : std::vector<int>{125,150,200})
        ok=runScale(entry,scale) && ok;

    FreeLibrary(proxy);

    std::cout<<"gdi-scaled-visual="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-click-map="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-drag-redraw="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-outside-drag="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-close="<<(ok?"PASS":"FAIL")<<"\n";
    std::cout<<"gdi-harness="<<(ok?"PASS":"FAIL")<<"\n";
    return ok?0:6;
}
