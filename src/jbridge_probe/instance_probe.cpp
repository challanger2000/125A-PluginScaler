#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct WindowRow {
    HWND hwnd{};
    DWORD pid{};
    DWORD tid{};
    HWND parent{};
    HWND owner{};
    HWND root{};
    HWND rootOwner{};
    std::wstring className;
    std::wstring title;
    RECT windowRect{};
    RECT clientRect{};
    LONG_PTR style{};
    LONG_PTR exStyle{};
    bool visible{};
    bool enabled{};
    int depth{};
};

std::wstring lower(std::wstring v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
    return v;
}

std::set<DWORD> jbridgePids() {
    std::set<DWORD> result;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W e{}; e.dwSize=sizeof(e);
    if(Process32FirstW(snap,&e)) {
        do {
            auto n=lower(e.szExeFile);
            if(n==L"auxhost.exe" || n==L"auxhost64.exe")
                result.insert(e.th32ProcessID);
        } while(Process32NextW(snap,&e));
    }
    CloseHandle(snap);
    return result;
}

std::wstring textOf(HWND h) {
    std::array<wchar_t,1024> b{};
    int n=GetWindowTextW(h,b.data(),static_cast<int>(b.size()));
    return n>0?std::wstring(b.data(),static_cast<std::size_t>(n)):L"";
}

std::wstring classOf(HWND h) {
    std::array<wchar_t,512> b{};
    int n=GetClassNameW(h,b.data(),static_cast<int>(b.size()));
    return n>0?std::wstring(b.data(),static_cast<std::size_t>(n)):L"";
}

std::wstring hexHwnd(HWND h) {
    std::wstringstream s;
    s<<L"0x"<<std::hex<<std::uppercase
     <<reinterpret_cast<std::uintptr_t>(h);
    return s.str();
}

int depth(HWND h) {
    int d=0;
    while((h=GetParent(h))!=nullptr && d<64) ++d;
    return d;
}

long width(const RECT& r){return r.right-r.left;}
long height(const RECT& r){return r.bottom-r.top;}

struct Ctx {
    const std::set<DWORD>* pids{};
    std::vector<WindowRow>* rows{};
};

void add(HWND h,Ctx& c) {
    DWORD pid=0;
    DWORD tid=GetWindowThreadProcessId(h,&pid);
    if(!pid || !c.pids->contains(pid)) return;

    WindowRow r{};
    r.hwnd=h;
    r.pid=pid;
    r.tid=tid;
    r.parent=GetParent(h);
    r.owner=GetWindow(h,GW_OWNER);
    r.root=GetAncestor(h,GA_ROOT);
    r.rootOwner=GetAncestor(h,GA_ROOTOWNER);
    r.className=classOf(h);
    r.title=textOf(h);
    GetWindowRect(h,&r.windowRect);
    GetClientRect(h,&r.clientRect);
    r.style=GetWindowLongPtrW(h,GWL_STYLE);
    r.exStyle=GetWindowLongPtrW(h,GWL_EXSTYLE);
    r.visible=IsWindowVisible(h)!=FALSE;
    r.enabled=IsWindowEnabled(h)!=FALSE;
    r.depth=depth(h);
    c.rows->push_back(std::move(r));
}

BOOL CALLBACK childProc(HWND h,LPARAM p){
    auto* c=reinterpret_cast<Ctx*>(p);
    if(!c) return FALSE;
    add(h,*c);
    return TRUE;
}

BOOL CALLBACK topProc(HWND h,LPARAM p){
    auto* c=reinterpret_cast<Ctx*>(p);
    if(!c) return FALSE;
    add(h,*c);
    EnumChildWindows(h,childProc,p);
    return TRUE;
}

std::filesystem::path exeDir(){
    std::array<wchar_t,32768> b{};
    DWORD n=GetModuleFileNameW(nullptr,b.data(),static_cast<DWORD>(b.size()));
    if(!n || n>=b.size()) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(b.data(),n)).parent_path();
}

} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    const auto pids=jbridgePids();
    std::vector<WindowRow> rows;
    Ctx c{&pids,&rows};
    if(!pids.empty())
        EnumWindows(topProc,reinterpret_cast<LPARAM>(&c));

    std::sort(rows.begin(),rows.end(),[](const WindowRow&a,const WindowRow&b){
        if(a.pid!=b.pid) return a.pid<b.pid;
        if(a.depth!=b.depth) return a.depth<b.depth;
        return reinterpret_cast<std::uintptr_t>(a.hwnd)<
               reinterpret_cast<std::uintptr_t>(b.hwnd);
    });

    std::map<DWORD,int> perPid;
    for(const auto& r:rows) ++perPid[r.pid];

    const auto path=exeDir()/L"125A-jBridge-MultiInstanceProbe.txt";
    std::wofstream out(path,std::ios::trunc);
    if(out){
        out<<L"125A jBridge Multi-Instance Probe\n";
        out<<L"purpose=vendor-neutral multi-instance window topology measurement\n";
        out<<L"jbridgePidCount="<<pids.size()<<L"\n";
        out<<L"windowCount="<<rows.size()<<L"\n\n";

        for(DWORD pid:pids)
            out<<L"PROCESS pid="<<pid
               <<L" windowCount="<<perPid[pid]<<L"\n";

        out<<L"\n";
        for(const auto&r:rows){
            const bool hasArea=width(r.clientRect)>0 && height(r.clientRect)>0;
            out<<L"WINDOW"
               <<L" hwnd="<<hexHwnd(r.hwnd)
               <<L" pid="<<r.pid
               <<L" tid="<<r.tid
               <<L" depth="<<r.depth
               <<L" parent="<<hexHwnd(r.parent)
               <<L" owner="<<hexHwnd(r.owner)
               <<L" root="<<hexHwnd(r.root)
               <<L" rootOwner="<<hexHwnd(r.rootOwner)
               <<L" visible="<<(r.visible?1:0)
               <<L" enabled="<<(r.enabled?1:0)
               <<L" window="<<width(r.windowRect)<<L"x"<<height(r.windowRect)
               <<L" client="<<width(r.clientRect)<<L"x"<<height(r.clientRect)
               <<L" style=0x"<<std::hex<<static_cast<std::uintptr_t>(r.style)
               <<L" exStyle=0x"<<static_cast<std::uintptr_t>(r.exStyle)
               <<std::dec
               <<L" hasArea="<<(hasArea?1:0)
               <<L" class=\""<<r.className<<L"\""
               <<L" title=\""<<r.title<<L"\""
               <<L"\n";
        }
    }

    std::wstringstream msg;
    if(pids.empty()){
        msg<<L"Kein jBridge auxhost gefunden.\n\n"
           <<L"Bitte zwei gebridgte Plugins gleichzeitig oeffnen und erneut starten.";
    } else {
        msg<<L"Messung fertig.\n\n"
           <<L"jBridge-Prozesse: "<<pids.size()<<L"\n"
           <<L"Fenster: "<<rows.size()<<L"\n\n"
           <<L"Log:\n"<<path.wstring();
    }

    MessageBoxW(nullptr,msg.str().c_str(),
                L"125A jBridge Multi-Instance Probe",
                MB_OK|MB_ICONINFORMATION|MB_SETFOREGROUND);
    return 0;
}
