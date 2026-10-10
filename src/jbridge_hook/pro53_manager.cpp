#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>
#include "native_scale_shared.h"
static_assert(sizeof(void*)==4,"Pro-53 manager must be built as x86");
namespace {
HINSTANCE instance{}; HWND view{},message{};
HMODULE hookDll{}; HOOKPROC callback{};
std::wstring tempDll,tempDir;
struct Session {
    HWND hwnd{};DWORD pid{},tid{};HANDLE mapping{};
    NativeAttachCommand* state{};HHOOK hook{};
};
std::vector<Session> active;
std::wstring userStatus;
void updateView() {
    if(!message)return;
    std::wstring s=userStatus;
    if(!active.empty()&&active[0].state&&active[0].state->status==1) {
        const auto* v=active[0].state;
        s+=L"\n\nMAUS: "+std::to_wstring(v->nativeMouseEvents)+
           L" (Down "+std::to_wstring(v->nativeMouseDown)+
           L", Move "+std::to_wstring(v->nativeMouseMove)+
           L", Up "+std::to_wstring(v->nativeMouseUp)+L")";
        s+=L"\nPlugin-Capture: "+std::to_wstring(v->diagCaptureCalls)+
           L" (aktiv "+std::to_wstring(v->diagCaptureSuccessful)+
           L", Release "+std::to_wstring(v->diagReleaseCaptureCalls)+
           L", Import "+std::to_wstring(v->diagCaptureImported)+L")";
        s+=L"\nPlugin-Neuzeichnung: "+std::to_wstring(v->diagInvalidateCalls);
        s+=L"\nPosition physisch: "+std::to_wstring(v->nativeLastPhysicalX)+
           L","+std::to_wstring(v->nativeLastPhysicalY)+
           L" -> logisch: "+std::to_wstring(v->nativeLastLogicalX)+
           L","+std::to_wstring(v->nativeLastLogicalY);
        s+=L"\n\nGRAFIK-DIAGNOSE (echter Pro-53-Prozess):";
        s+=L"\nBeginPaint: "+std::to_wstring(v->diagPaint)+
           L"  GetDC: "+std::to_wstring(v->diagGetDC)+
           L"  DIB: "+std::to_wstring(v->diagDibCalls);
        s+=L"\nDIB-Skalierung: "+std::to_wstring(v->diagDibConverted)+
           L"  Anderer DC: "+std::to_wstring(v->diagDibOtherDC)+
           L"  Ungeeignet: "+std::to_wstring(v->diagDibSkipped);
        s+=L"\nTatsaechlich gezeichnet: "+std::to_wstring(v->diagStretchSuccess)+
           L"  GDI-Fehler/0: "+std::to_wstring(v->diagStretchFailure)+
           L"  Letzter Rueckgabewert: "+std::to_wstring(v->diagLastStretchReturn);
        s+=L"\n100 logische Pixel -> "+std::to_wstring(v->diagMappedDx)+
           L" / "+std::to_wstring(v->diagMappedDy)+L" physische Pixel";
        s+=L"\nABGELEHNTER Aufruf: Grund "+std::to_wstring(v->diagRejectReason)+
           L"  Rechteck "+std::to_wstring(v->diagRejectedWidth)+
           L"x"+std::to_wstring(v->diagRejectedHeight);
        s+=L"\nQuelle "+std::to_wstring(v->diagRejectedX)+L","+
           std::to_wstring(v->diagRejectedY)+
           L"  Bitmap "+std::to_wstring(v->diagRejectedBitmapWidth)+L"x"+
           std::to_wstring(v->diagRejectedBitmapHeight);
        s+=L"\nScanlines "+std::to_wstring(v->diagRejectedStart)+L","+
           std::to_wstring(v->diagRejectedLines)+
           L"  Tiefe "+std::to_wstring(v->diagRejectedBits)+
           L"  Kompression "+std::to_wstring(v->diagRejectedCompression);
    }
    SetWindowTextW(message,s.c_str());
}
void output(const std::wstring& s){userStatus=s;updateView();}
void release(Session& s){
    if(s.hook)UnhookWindowsHookEx(s.hook);
    if(s.state)UnmapViewOfFile(s.state);
    if(s.mapping)CloseHandle(s.mapping);
    s={};
}
bool waitChange(volatile LONG* v,LONG initial,DWORD maxMs){
    for(DWORD t=0;t<maxMs;t+=10){if(*v!=initial)return true;Sleep(10);}
    return *v!=initial;
}
bool hasPro53(DWORD pid) {
    HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(h==INVALID_HANDLE_VALUE)return false;
    MODULEENTRY32W m{};m.dwSize=sizeof(m);bool found=false;
    if(Module32FirstW(h,&m))do{
        if(_wcsicmp(m.szModule,L"Pro-53.dll")==0){found=true;break;}
    }while(Module32NextW(h,&m));
    CloseHandle(h);return found;
}
std::vector<DWORD> processes() {
    std::vector<DWORD> result;
    HANDLE h=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(h==INVALID_HANDLE_VALUE)return result;
    PROCESSENTRY32W p{};p.dwSize=sizeof(p);
    if(Process32FirstW(h,&p))do{
        if((_wcsicmp(p.szExeFile,L"auxhost.exe")==0||
            _wcsicmp(p.szExeFile,L"gauxhost.exe")==0)&&
            hasPro53(p.th32ProcessID))result.push_back(p.th32ProcessID);
    }while(Process32NextW(h,&p));
    CloseHandle(h);return result;
}
struct Choice{HWND hwnd{};DWORD tid{};int score{};};
struct Scan{DWORD pid{};std::vector<Choice> choices;};
void collect(HWND hwnd,Scan& scan){
    DWORD owner{};DWORD tid=GetWindowThreadProcessId(hwnd,&owner);
    if(owner!=scan.pid||!tid||!IsWindowVisible(hwnd)||!IsWindowEnabled(hwnd))
        return;
    RECT r{};if(!GetClientRect(hwnd,&r))return;
    int w=r.right-r.left,h=r.bottom-r.top;
    if(w<300||h<160||w>1600||h>1200)return;
    wchar_t cls[128]{};GetClassNameW(hwnd,cls,128);
    if(_wcsicmp(cls,L"Button")==0||_wcsicmp(cls,L"Static")==0)return;
    wchar_t title[256]{};
    HWND root=GetAncestor(hwnd,GA_ROOT);
    if(root)GetWindowTextW(root,title,256);
    std::wstring text=title;
    std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){
        return static_cast<wchar_t>(towlower(c));});
    int score=100000-30*std::abs(w-665)-30*std::abs(h-315);
    if(text.find(L"pro-53")!=std::wstring::npos||
       text.find(L"pro53")!=std::wstring::npos)score+=1000000;
    if(hwnd!=root)score+=5000;
    scan.choices.push_back({hwnd,tid,score});
}
BOOL CALLBACK child(HWND hwnd,LPARAM param){
    collect(hwnd,*reinterpret_cast<Scan*>(param));return TRUE;
}
BOOL CALLBACK top(HWND hwnd,LPARAM param){
    auto& s=*reinterpret_cast<Scan*>(param);
    collect(hwnd,s);EnumChildWindows(hwnd,child,param);return TRUE;
}
std::vector<Choice> windows(DWORD pid) {
    Scan s{pid,{}};
    EnumWindows(top,reinterpret_cast<LPARAM>(&s));
    std::sort(s.choices.begin(),s.choices.end(),
         [](const Choice& a,const Choice& b){return a.score>b.score;});
    return s.choices;
}
bool connect(HWND hwnd,DWORD pid,DWORD tid,int scale,Session& result,LONG& failure){
    failure=0;
    Session s{};s.hwnd=hwnd;s.pid=pid;s.tid=tid;
    wchar_t name[192]{};
    swprintf_s(name,L"Local\\125A_NativeAttach_%lu_%lu",pid,tid);
    s.mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
                      0,sizeof(NativeAttachCommand),name);
    if(!s.mapping){failure=-30;return false;}
    if(GetLastError()==ERROR_ALREADY_EXISTS){
        failure=-31;release(s);return false;
    }
    s.state=static_cast<NativeAttachCommand*>(MapViewOfFile(
           s.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeAttachCommand)));
    if(!s.state){failure=-32;release(s);return false;}
    ZeroMemory(s.state,sizeof(NativeAttachCommand));
    s.state->hwnd=static_cast<LONG>(reinterpret_cast<std::uintptr_t>(hwnd));
    s.state->scale=scale;
    s.state->targetKind=1; // Only native Pro-53.dll window procedures
    s.hook=SetWindowsHookExW(WH_GETMESSAGE,callback,hookDll,tid);
    if(!s.hook){failure=-33;release(s);return false;}
    if(!PostMessageW(hwnd,WM_NULL,0,0)){
        failure=-34;release(s);return false;
    }
    if(!waitChange(&s.state->status,0,2200)){
        if(InterlockedCompareExchange(&s.state->status,-10,0)==0) {
            failure=-10;release(s);return false;
        }
        // The hook may have completed while we were trying to cancel.
        // Never unload an IAT-modifying DLL in an unknown state.
    }
    if(s.state->status!=1){
        failure=s.state->status;
        release(s);return false;
    }
    result=s;return true;
}
bool processExited(DWORD pid) {
    HANDLE p=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!p)return false; // unknown: fail closed, never guess process death
    const bool stopped=WaitForSingleObject(p,0)==WAIT_OBJECT_0;
    CloseHandle(p);
    return stopped;
}
bool restore() {
    // Protect the original renderer from unloading patched code. Retry a
    // failed/undelivered WM_NULL without hanging the manager forever.
    // An exited auxhost has released the renderer and is safe to forget.
    bool pending=false;
    for(auto& s:active) {
        if(!s.state || processExited(s.pid))continue;
        LONG status=InterlockedCompareExchange(&s.state->status,0,0);
        if(status==2 || status==3)continue; // no installed render hooks
        if(status!=1 && status!=-5){pending=true;continue;}
        InterlockedExchange(&s.state->detach,1);
        for(int attempt=0;attempt<3;++attempt) {
            // A plugin can change/destroy the editor HWND while its GUI
            // thread remains alive: use both window and thread messages.
            if(IsWindow(s.hwnd))PostMessageW(s.hwnd,WM_NULL,0,0);
            PostThreadMessageW(s.tid,WM_NULL,0,0);
            for(int n=0;n<80;++n) {
                if(processExited(s.pid))break;
                status=InterlockedCompareExchange(&s.state->status,0,0);
                if(status==2 || status==3)break;
                Sleep(10);
            }
            status=InterlockedCompareExchange(&s.state->status,0,0);
            if(status==2 || status==3 || processExited(s.pid))break;
        }
        status=InterlockedCompareExchange(&s.state->status,0,0);
        if(status!=2 && status!=3 && !processExited(s.pid)) {
            pending=true;
            output(L"Rueckbau nicht bestaetigt. Diagnosecode: "+
                   std::to_wstring(status)+L"\\n"
                   L"Scaler bleibt zum Schutz von Studio One aktiv.");
        }
    }
    if(pending)return false;
    for(auto& s:active)release(s);
    active.clear();
    return true;
}
void start(int percentage){
    if(!restore()){output(L"Rueckbau fehlgeschlagen. 125A bleibt aktiv.");
        return;}
    auto pids=processes();
    if(pids.empty()){output(L"Kein 32-Bit-jBridge mit Pro-53.dll gefunden.\n"
              L"Pro-53 in Studio One separat oeffnen.");return;}
    int connected=0,inspected=0;
    LONG lastFailure=0;
    for(DWORD pid:pids){
        for(const auto& w:windows(pid)){
            Session session{};
            LONG failure{};
            ++inspected;
            if(connect(w.hwnd,pid,w.tid,percentage,session,failure)){
                active.push_back(session);++connected;break;
            }
            if(failure!=0)lastFailure=failure;
        }
    }
    if(connected)output(std::to_wstring(connected)+
       L" Pro-53-Fenster auf "+std::to_wstring(percentage)+
       L" % skaliert.\n100 % / Beenden stellt die Originalgroesse wieder her.");
    else output(L"Pro-53 geladen, aber Editor-Zuordnung abgelehnt.\n"
                L"Fenster geprueft: "+std::to_wstring(inspected)+
                L" | Diagnose: "+std::to_wstring(lastFailure)+
                L"\nEs wurde nichts vergroessert.");
}
void cleanupDll(){
    if(hookDll){FreeLibrary(hookDll);hookDll=nullptr;}
    if(!tempDll.empty())DeleteFileW(tempDll.c_str());
    if(!tempDir.empty())RemoveDirectoryW(tempDir.c_str());
}
bool unpack(){
    HRSRC resource=FindResourceW(instance,MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!resource)return false;
    DWORD size=SizeofResource(instance,resource);
    auto data=LockResource(LoadResource(instance,resource));
    if(!data||size<100)return false;
    wchar_t temp[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,temp))return false;
    tempDir=std::wstring(temp)+L"125A-Pro53-"+
            std::to_wstring(GetCurrentProcessId());
    if(!CreateDirectoryW(tempDir.c_str(),nullptr)&&
       GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    tempDll=tempDir+L"\\125A-NativePro53Hook-x86.dll";
    HANDLE f=CreateFileW(tempDll.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    DWORD written{};
    BOOL good=WriteFile(f,data,size,&written,nullptr);CloseHandle(f);
    if(!good||written!=size)return false;
    hookDll=LoadLibraryW(tempDll.c_str());
    if(!hookDll)return false;
    callback=reinterpret_cast<HOOKPROC>(GetProcAddress(hookDll,"NativeMouseHook"));
    if(!callback)callback=reinterpret_cast<HOOKPROC>(
                                 GetProcAddress(hookDll,"_NativeMouseHook@12"));
    return callback!=nullptr;
}
LRESULT CALLBACK wndProc(HWND w,UINT m,WPARAM wp,LPARAM lp){
    if(m==WM_CREATE){
        message=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT,
                         20,16,575,326,w,nullptr,instance,nullptr);
        SetTimer(w,1,1000,nullptr);
        CreateWindowW(L"BUTTON",L"150 %",WS_CHILD|WS_VISIBLE,
            20,349,110,34,w,reinterpret_cast<HMENU>(150),instance,nullptr);
        CreateWindowW(L"BUTTON",L"200 %",WS_CHILD|WS_VISIBLE,
            145,349,110,34,w,reinterpret_cast<HMENU>(200),instance,nullptr);
        CreateWindowW(L"BUTTON",L"100 % / Beenden",WS_CHILD|WS_VISIBLE,
            270,349,205,34,w,reinterpret_cast<HMENU>(300),instance,nullptr);
        output(L"125A Pro-53 Scaler\n"
               L"Pro-53 mit jBridge oeffnen, dann 150 oder 200 % anklicken.");
        return 0;
    }
    if(m==WM_TIMER){updateView();return 0;}
    if(m==WM_COMMAND){
        int id=LOWORD(wp);
        if(id==150||id==200){start(id);return 0;}
        if(id==300){SendMessageW(w,WM_CLOSE,0,0);return 0;}
    }
    if(m==WM_CLOSE){
        if(!restore()){
            MessageBoxW(w,L"Der native Rueckbau wurde nicht bestaetigt.\n"
                L"Fuer sicheres Beenden: Projekt speichern, erst Studio One\n"
                L"regulaer beenden, dann Scaler erneut schliessen.\n"
                L"Nicht den Scaler zuerst im Task-Manager abbrechen.",
                L"125A PluginScaler",MB_OK|MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(w);return 0;
    }
    if(m==WM_DESTROY){KillTimer(w,1);PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,m,wp,lp);
}
} // namespace
int WINAPI wWinMain(HINSTANCE h,HINSTANCE,LPWSTR args,int){
    instance=h;
    const bool testing=args && wcsstr(args,L"--self-test");
    if(!unpack()){
        if(!testing)MessageBoxW(nullptr,
            L"Eingebettete 32-Bit-Hook-DLL nicht ladbar.",
            L"125A PluginScaler",MB_OK|MB_ICONERROR);
        cleanupDll();return 2;
    }
    if(testing){cleanupDll();return 0;}
    WNDCLASSW klass{};
    klass.lpfnWndProc=wndProc;klass.hInstance=h;
    klass.lpszClassName=L"125A.Pro53.Manager";
    klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if(!RegisterClassW(&klass)){cleanupDll();return 3;}
    view=CreateWindowW(klass.lpszClassName,L"125A PluginScaler - Pro-53",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VISIBLE,
        CW_USEDEFAULT,CW_USEDEFAULT,612,437,nullptr,nullptr,h,nullptr);
    if(!view){cleanupDll();return 4;}
    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0){
        TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    if(!active.empty())return 5;
    cleanupDll();return 0;
}
