#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <cstdio>

struct Editor {
    HWND window{};
    int down{},move{},up{};
    bool drag{};
    int lastX{},lastY{};
};
LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) {
        e=reinterpret_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e));
        e->window=hwnd;
    }
    if(e) {
        if(msg==WM_LBUTTONDOWN) {++e->down;e->drag=true;SetCapture(hwnd);}
        if(msg==WM_MOUSEMOVE && e->drag) {++e->move;e->lastX=GET_X_LPARAM(l);e->lastY=GET_Y_LPARAM(l);}
        if(msg==WM_LBUTTONUP) {++e->up;e->drag=false;if(GetCapture()==hwnd)ReleaseCapture();}
        if(msg==WM_PAINT) {
            PAINTSTRUCT ps{};HDC dc=BeginPaint(hwnd,&ps);
            RECT rect{};GetClientRect(hwnd,&rect);
            HBRUSH bg=CreateSolidBrush(RGB(33,39,46));
            FillRect(dc,&rect,bg);DeleteObject(bg);
            HBRUSH knob=CreateSolidBrush(RGB(46,160,210));
            RECT r{90,65,155,130};FillRect(dc,&r,knob);DeleteObject(knob);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
            TextOutW(dc,15,15,L"125A MOCK 32-bit VST2 EDITOR",29);
            EndPaint(hwnd,&ps);return 0;
        }
    }
    return DefWindowProcW(hwnd,msg,w,l);
}
LRESULT CALLBACK occluderProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{}; GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(230, 15, 90));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int main() {
    HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=editorProc;
    wc.lpszClassName=L"125A.MockLegacyEditor";RegisterClassW(&wc);
    WNDCLASSW blocker{};
    blocker.hInstance=instance;
    blocker.lpfnWndProc=occluderProc;
    blocker.lpszClassName=L"125A.MockOccluder";
    RegisterClassW(&blocker);
    Editor first{},second{};
    HWND a=CreateWindowW(wc.lpszClassName,L"Mock Pro53-style",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,100,100,440,290,nullptr,nullptr,instance,&first);
    HWND b=CreateWindowW(wc.lpszClassName,L"Mock FM7-style",
        WS_OVERLAPPEDWINDOW|WS_VISIBLE,600,100,500,320,nullptr,nullptr,instance,&second);
    if(!a||!b)return 1;
    // Exercise real mouse input (no PostMessage shortcut).
    SetForegroundWindow(a);
    POINT start{120,110};ClientToScreen(a,&start);
    SetCursorPos(start.x,start.y);
    INPUT inputs[3]{};
    inputs[0].type=INPUT_MOUSE;inputs[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
    inputs[1].type=INPUT_MOUSE;inputs[1].mi.dwFlags=MOUSEEVENTF_MOVE;
    inputs[1].mi.dx=15;inputs[1].mi.dy=18;
    inputs[2].type=INPUT_MOUSE;inputs[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    for(const auto& input: inputs) {
        INPUT current=input;
        SendInput(1,&current,sizeof(INPUT));
        for(int n=0;n<12;++n) {
            MSG msg{};
            while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&msg);DispatchMessageW(&msg);
            }
            Sleep(10);
        }
    }
    bool passed=first.down>0&&first.move>0&&first.up>0 &&
        second.down==0 &&second.up==0;
    std::printf("mock-real-mouse=%s\n",passed?"PASS":"FAIL");
    std::printf("mock-independent-windows=%s\n",
        first.window!=second.window?"PASS":"FAIL");
    // Launch the viewer bound to this exact mock HWND. Exercise viewer -> editor,
    // not merely direct mouse gestures to an unscaled dummy.
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exe=exePath;
    auto slash=exe.find_last_of(L"\\/");
    exe.resize(slash+1);
    exe+=L"PluginScalerJBridgeViewer.exe";
    std::wstring command=L"\""+exe+L"\" --mock-hwnd "+
        std::to_wstring(reinterpret_cast<UINT_PTR>(a));
    std::vector<wchar_t> commandBuffer(command.begin(),command.end());
    commandBuffer.push_back(0);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION proc{};
    bool viewerPassed=false;
    if(CreateProcessW(exe.c_str(),commandBuffer.data(),nullptr,nullptr,
                      FALSE,0,nullptr,nullptr,&startup,&proc)) {
        HWND scaled=nullptr;
        for(int i=0;i<120 && !scaled;++i) {
            EnumWindows([](HWND w,LPARAM p)->BOOL {
                wchar_t cls[128]{};
                GetClassNameW(w,cls,128);
                if(wcscmp(cls,L"125A.JBridgeScaler.Viewer")==0)
                    *reinterpret_cast<HWND*>(p)=w;
                return TRUE;
            },reinterpret_cast<LPARAM>(&scaled));
            if(!scaled) Sleep(50);
        }
        if(scaled) {
            SetForegroundWindow(scaled);
            // Verify actual rendered pixels, not only a successful process start.
            // The mock editor has a known opaque background. Its top-left
            // client sample must remain that color in the 150% viewer.
            Sleep(300);
            bool imagePassed=false;
            POINT sample{45,160};
            ClientToScreen(scaled,&sample);
            HDC desktop=GetDC(nullptr);
            const COLORREF pixel=GetPixel(desktop,sample.x,sample.y);
            ReleaseDC(nullptr,desktop);
            imagePassed = (pixel==RGB(33,39,46));
            // Regression: unrelated windows overlapping the original editor
            // must never replace the editor's pixels in the scaled viewer.
            HWND occluder = CreateWindowW(blocker.lpszClassName,
                L"Unrelated overlay", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                110, 115, 300, 230, nullptr, nullptr, instance, &second);
            if (occluder) {
                // Ensure the occluder actually covers the source pixel; an
                // occlusion test that never overlaps is a false PASS.
                RECT occlusion{}, original{};
                GetWindowRect(occluder,&occlusion);
                GetWindowRect(a,&original);
                RECT intersection{};
                const bool overlaps = IntersectRect(&intersection,
                    &occlusion,&original) != FALSE;
                if (!overlaps) imagePassed = false;
                SetWindowPos(occluder, HWND_TOP, 110,115,300,230,
                             SWP_SHOWWINDOW);
                SetWindowPos(scaled, HWND_TOP, 0,0,0,0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                for (int n=0;n<30;++n) {
                    MSG pending{};
                    while (PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)) {
                        TranslateMessage(&pending);DispatchMessageW(&pending);
                    }
                    Sleep(20);
                }
                HDC current=GetDC(nullptr);
                COLORREF pixelOccluded=GetPixel(current,sample.x,sample.y);
                ReleaseDC(nullptr,current);
                const bool occlusionPassed=(pixelOccluded==RGB(33,39,46));
                std::printf("viewer-window-occlusion=%s\\n",
                            occlusionPassed?"PASS":"FAIL");
                imagePassed = imagePassed && occlusionPassed;
                DestroyWindow(occluder);
                SetForegroundWindow(scaled);
            } else imagePassed=false;
            std::printf("viewer-150pct-image=%s (pixel=%lu)\\n",
                        imagePassed?"PASS":"FAIL",static_cast<unsigned long>(pixel));
            RECT client{};GetClientRect(scaled,&client);
            POINT mouse{client.left+120,client.top+110};
            ClientToScreen(scaled,&mouse);
            SetCursorPos(mouse.x,mouse.y);
            INPUT down{};down.type=INPUT_MOUSE;down.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
            INPUT move{};move.type=INPUT_MOUSE;move.mi.dwFlags=MOUSEEVENTF_MOVE;
            move.mi.dx=12;move.mi.dy=15;
            INPUT up{};up.type=INPUT_MOUSE;up.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            const int before=first.down;
            const int beforeUp=first.up;
            const int beforeMove=first.move;
            for(const INPUT input:{down,move,up}) {
                INPUT copy=input;
                SendInput(1,&copy,sizeof(copy));
                for(int n=0;n<15;++n) {
                    MSG msg{};
                    while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
                        TranslateMessage(&msg);DispatchMessageW(&msg);
                    }
                    Sleep(10);
                }
            }
            viewerPassed=imagePassed && first.down>before &&
                first.up>beforeUp && first.move>beforeMove &&
                !first.drag && second.down==0 && second.up==0;
        }
        TerminateProcess(proc.hProcess,0);
        WaitForSingleObject(proc.hProcess,1000);
        CloseHandle(proc.hThread);
        CloseHandle(proc.hProcess);
    }
    std::printf("viewer-through-mouse=%s\n",viewerPassed?"PASS":"FAIL");
    DestroyWindow(a);DestroyWindow(b);
    return passed && viewerPassed ?0:3;
}
