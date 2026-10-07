#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring lower(std::wstring v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](wchar_t c){ return static_cast<wchar_t>(towlower(c)); });
    return v;
}

std::set<DWORD> auxPids() {
    std::set<DWORD> out;
    HANDLE s=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(s==INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{}; e.dwSize=sizeof(e);
    if(Process32FirstW(s,&e)) {
        do {
            const auto n=lower(e.szExeFile);
            if(n==L"auxhost.exe" || n==L"auxhost64.exe") out.insert(e.th32ProcessID);
        } while(Process32NextW(s,&e));
    }
    CloseHandle(s);
    return out;
}

std::wstring cls(HWND h) {
    std::array<wchar_t,512> b{};
    int n=GetClassNameW(h,b.data(),static_cast<int>(b.size()));
    return n>0?std::wstring(b.data(),static_cast<std::size_t>(n)):L"";
}

struct Ctx {
    const std::set<DWORD>* pids{};
    std::vector<HWND>* found{};
};

void maybe(HWND h,Ctx& c) {
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(!c.pids->contains(pid) || !IsWindowVisible(h)) return;
    auto name=cls(h);
    if(name.rfind(L"NIVSTChildWindow",0)!=0) return;
    RECT cr{}; if(!GetClientRect(h,&cr)) return;
    if(cr.right-cr.left<100 || cr.bottom-cr.top<100) return;
    c.found->push_back(h);
}

BOOL CALLBACK childProc(HWND h,LPARAM p) {
    auto* c=reinterpret_cast<Ctx*>(p); if(!c) return FALSE;
    maybe(h,*c); return TRUE;
}
BOOL CALLBACK topProc(HWND h,LPARAM p) {
    auto* c=reinterpret_cast<Ctx*>(p); if(!c) return FALSE;
    maybe(h,*c); EnumChildWindows(h,childProc,p); return TRUE;
}

std::filesystem::path dir() {
    std::array<wchar_t,32768> b{};
    DWORD n=GetModuleFileNameW(nullptr,b.data(),static_cast<DWORD>(b.size()));
    if(!n || n>=b.size()) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(b.data(),n)).parent_path();
}

bool saveBmp(const std::filesystem::path& path,int w,int h,const std::vector<std::uint8_t>& px) {
    if(w<=0 || h<=0 || px.size()!=static_cast<std::size_t>(w)*h*4u) return false;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    ih.biSize=sizeof(ih); ih.biWidth=w; ih.biHeight=-h; ih.biPlanes=1;
    ih.biBitCount=32; ih.biCompression=BI_RGB;
    ih.biSizeImage=static_cast<DWORD>(px.size());
    fh.bfType=0x4D42;
    fh.bfOffBits=sizeof(fh)+sizeof(ih);
    fh.bfSize=fh.bfOffBits+ih.biSizeImage;
    std::ofstream f(path,std::ios::binary);
    if(!f) return false;
    f.write(reinterpret_cast<const char*>(&fh),sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih),sizeof(ih));
    f.write(reinterpret_cast<const char*>(px.data()),static_cast<std::streamsize>(px.size()));
    return static_cast<bool>(f);
}

struct Metrics { std::size_t nonBlack{}; std::uint8_t minV{255}; std::uint8_t maxV{}; std::uint32_t hash{2166136261u}; };

Metrics metrics(const std::vector<std::uint8_t>& px) {
    Metrics m{};
    const std::size_t pixels=px.size()/4u;
    const std::size_t step=(std::max<std::size_t>)(1,pixels/4096u);
    for(std::size_t p=0;p<pixels;p+=step) {
        const std::size_t i=p*4u;
        auto b=px[i],g=px[i+1],r=px[i+2];
        auto v=(std::max)({r,g,b});
        m.minV=(std::min)(m.minV,v);
        m.maxV=(std::max)(m.maxV,v);
        if(v>8) ++m.nonBlack;
        m.hash^=b; m.hash*=16777619u;
        m.hash^=g; m.hash*=16777619u;
        m.hash^=r; m.hash*=16777619u;
    }
    return m;
}

bool captureScreen(HWND h,std::vector<std::uint8_t>& out,int& w,int& hgt) {
    RECT wr{}; if(!GetWindowRect(h,&wr)) return false;
    w=wr.right-wr.left; hgt=wr.bottom-wr.top;
    if(w<=0||hgt<=0) return false;
    HDC screen=GetDC(nullptr);
    HDC mem=CreateCompatibleDC(screen);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth=w; bmi.bmiHeader.biHeight=-hgt; bmi.bmiHeader.biPlanes=1;
    bmi.bmiHeader.biBitCount=32; bmi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP bmp=CreateDIBSection(screen,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!screen||!mem||!bmp||!bits) {
        if(bmp) DeleteObject(bmp); if(mem) DeleteDC(mem); if(screen) ReleaseDC(nullptr,screen);
        return false;
    }
    auto old=SelectObject(mem,bmp);
    BOOL ok=BitBlt(mem,0,0,w,hgt,screen,wr.left,wr.top,SRCCOPY|CAPTUREBLT);
    if(ok) {
        out.assign(static_cast<std::uint8_t*>(bits),static_cast<std::uint8_t*>(bits)+static_cast<std::size_t>(w)*hgt*4u);
    }
    SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr,screen);
    return ok!=FALSE;
}

bool capturePrintWindow(HWND h,std::vector<std::uint8_t>& out,int& w,int& hgt) {
    RECT wr{}; if(!GetWindowRect(h,&wr)) return false;
    w=wr.right-wr.left; hgt=wr.bottom-wr.top;
    if(w<=0||hgt<=0) return false;
    HDC screen=GetDC(nullptr);
    HDC mem=CreateCompatibleDC(screen);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth=w; bmi.bmiHeader.biHeight=-hgt; bmi.bmiHeader.biPlanes=1;
    bmi.bmiHeader.biBitCount=32; bmi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HBITMAP bmp=CreateDIBSection(screen,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!screen||!mem||!bmp||!bits) {
        if(bmp) DeleteObject(bmp); if(mem) DeleteDC(mem); if(screen) ReleaseDC(nullptr,screen);
        return false;
    }
    auto old=SelectObject(mem,bmp);
    PatBlt(mem,0,0,w,hgt,BLACKNESS);
    BOOL ok=PrintWindow(h,mem,PW_RENDERFULLCONTENT);
    if(ok) {
        out.assign(static_cast<std::uint8_t*>(bits),static_cast<std::uint8_t*>(bits)+static_cast<std::size_t>(w)*hgt*4u);
    }
    SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr,screen);
    return ok!=FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    const auto pids=auxPids();
    std::vector<HWND> found; Ctx c{&pids,&found};
    if(!pids.empty()) EnumWindows(topProc,reinterpret_cast<LPARAM>(&c));

    const auto base=dir();
    const auto logPath=base/L"125A-jBridge-CaptureProbe.txt";
    std::wofstream log(logPath,std::ios::trunc);
    log<<L"125A jBridge Capture Probe\n";
    log<<L"candidateCount="<<found.size()<<L"\n";

    if(found.empty()) {
        MessageBoxW(nullptr,L"Kein sichtbares NI-VST-Fenster gefunden.",L"125A Capture Probe",MB_OK|MB_ICONWARNING);
        return 2;
    }

    HWND target=*std::max_element(found.begin(),found.end(),[](HWND a,HWND b){
        RECT ra{},rb{}; GetWindowRect(a,&ra); GetWindowRect(b,&rb);
        return (ra.right-ra.left)*(ra.bottom-ra.top) < (rb.right-rb.left)*(rb.bottom-rb.top);
    });

    DWORD pid=0; GetWindowThreadProcessId(target,&pid);
    RECT wr{}; GetWindowRect(target,&wr);
    log<<L"targetClass=\""<<cls(target)<<L"\" pid="<<pid
       <<L" window="<<(wr.right-wr.left)<<L"x"<<(wr.bottom-wr.top)<<L"\n";

    std::vector<std::uint8_t> screenPx,printPx;
    int sw=0,sh=0,pw=0,ph=0;
    const bool screenOk=captureScreen(target,screenPx,sw,sh);
    const bool printOk=capturePrintWindow(target,printPx,pw,ph);

    if(screenOk) {
        const auto m=metrics(screenPx);
        saveBmp(base/L"125A-jBridge-Capture-Screen.bmp",sw,sh,screenPx);
        log<<L"screen ok=1 size="<<sw<<L"x"<<sh
           <<L" nonBlack="<<m.nonBlack
           <<L" min="<<static_cast<unsigned>(m.minV)
           <<L" max="<<static_cast<unsigned>(m.maxV)
           <<L" hash=0x"<<std::hex<<m.hash<<std::dec<<L"\n";
    } else log<<L"screen ok=0\n";

    if(printOk) {
        const auto m=metrics(printPx);
        saveBmp(base/L"125A-jBridge-Capture-PrintWindow.bmp",pw,ph,printPx);
        log<<L"printWindow ok=1 size="<<pw<<L"x"<<ph
           <<L" nonBlack="<<m.nonBlack
           <<L" min="<<static_cast<unsigned>(m.minV)
           <<L" max="<<static_cast<unsigned>(m.maxV)
           <<L" hash=0x"<<std::hex<<m.hash<<std::dec<<L"\n";
    } else log<<L"printWindow ok=0\n";

    MessageBoxW(nullptr,
        L"Capture-Test fertig.\n\nBitte 125A-jBridge-CaptureProbe.txt hochladen.",
        L"125A jBridge Capture Probe",
        MB_OK|MB_ICONINFORMATION|MB_SETFOREGROUND);
    return 0;
}
