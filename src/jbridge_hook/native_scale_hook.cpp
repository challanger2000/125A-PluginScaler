#include "native_scale_shared.h"
#include <windowsx.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <climits>
#include <cstdlib>

namespace {
using BeginPaintFn=HDC (WINAPI*)(HWND,LPPAINTSTRUCT);
using GetDCFn=HDC (WINAPI*)(HWND);
using SetDIBitsFn=int (WINAPI*)(HDC,int,int,DWORD,DWORD,int,int,UINT,UINT,
                                 const void*,const BITMAPINFO*,UINT);
BeginPaintFn originalBeginPaint{};
GetDCFn originalGetDC{};
SetDIBitsFn originalSetDIBits{};
HWND target{};
int zoom=100;
int logicalWidth=kLogicalWidth,logicalHeight=kLogicalHeight;
struct Patch {
    volatile LONG* slot{};
    LONG previous{};
    LONG inserted{};
};
Patch patches[3]{};
unsigned patchCount{};
bool patched=false;
void transformDC(HDC dc) {
    if(!dc||zoom<100||zoom>200)return;
    SetMapMode(dc,MM_ANISOTROPIC);
    SetWindowExtEx(dc,logicalWidth,logicalHeight,nullptr);
    SetViewportExtEx(dc,MulDiv(logicalWidth,zoom,100),
                       MulDiv(logicalHeight,zoom,100),nullptr);
}
HDC WINAPI scaledBeginPaint(HWND hwnd,LPPAINTSTRUCT ps) {
    HDC dc=originalBeginPaint ? originalBeginPaint(hwnd,ps):nullptr;
    if(hwnd==target)transformDC(dc);
    return dc;
}
HDC WINAPI scaledGetDC(HWND hwnd) {
    HDC dc=originalGetDC ? originalGetDC(hwnd):nullptr;
    if(hwnd==target)transformDC(dc);
    return dc;
}
int WINAPI scaledSetDIBits(HDC dc,int dx,int dy,DWORD width,DWORD height,
                           int sx,int sy,UINT start,UINT lines,
                           const void* bits,const BITMAPINFO* info,UINT usage) {
    // SetDIBitsToDevice only maps the destination ORIGIN. It does not zoom
    // pixels. For a complete RGB DIB, use the GDI stretch operation instead
    // so destination width/height observe our anisotropic map.
    if(dc&&bits&&info&&target&&zoom>100&&WindowFromDC(dc)==target &&
       info->bmiHeader.biSize>=sizeof(BITMAPINFOHEADER) &&
       (info->bmiHeader.biCompression==BI_RGB ||
        info->bmiHeader.biCompression==BI_BITFIELDS) &&
       info->bmiHeader.biWidth>0 &&
       (info->bmiHeader.biHeight>0||info->bmiHeader.biHeight<0) &&
       info->bmiHeader.biHeight!=LONG_MIN &&
       width<=INT_MAX&&height<=INT_MAX &&
       static_cast<DWORD>(info->bmiHeader.biWidth)==width &&
       static_cast<DWORD>(std::abs(info->bmiHeader.biHeight))==height &&
       start==0&&lines==height&&sx==0&&sy==0) {
        return StretchDIBits(dc,dx,dy,static_cast<int>(width),
              static_cast<int>(height),0,0,static_cast<int>(width),
              static_cast<int>(height),bits,info,usage,SRCCOPY);
    }
    // Partial/banded DIB writes and unknown source formats are unchanged
    // rather than silently guessing their image bounds or orientation.
    return originalSetDIBits ?
           originalSetDIBits(dc,dx,dy,width,height,sx,sy,start,lines,
                             bits,info,usage):0;
}
bool applyPatch(volatile LONG* slot,LONG replacement,LONG& original) {
    if(patchCount>=std::size(patches))return false;
    DWORD old{};
    if(!VirtualProtect(const_cast<LONG*>(slot),sizeof(LONG),
                       PAGE_READWRITE,&old))return false;
    original=InterlockedExchange(slot,replacement);
    DWORD ignored{};
    VirtualProtect(const_cast<LONG*>(slot),sizeof(LONG),old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),const_cast<LONG*>(slot),
                          sizeof(LONG));
    patches[patchCount++]={slot,original,replacement};
    return true;
}
bool patchEditorModuleIAT(HINSTANCE instance,LPCWSTR className) {
    if(patched)return true;
    WNDCLASSEXW klass{};
    klass.cbSize=sizeof(klass);
    if(!GetClassInfoExW(instance,className,&klass)||!klass.lpfnWndProc)
        return false;
    MEMORY_BASIC_INFORMATION memory{};
    if(!VirtualQuery(reinterpret_cast<LPCVOID>(klass.lpfnWndProc),
                     &memory,sizeof(memory))||memory.Type!=MEM_IMAGE)
        return false;
    auto* base=static_cast<unsigned char*>(memory.AllocationBase);
    if(!base)return false;
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                      base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    const DWORD size=nt->OptionalHeader.SizeOfImage;
    const auto imports=
         nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!imports.VirtualAddress||imports.VirtualAddress>=size)return false;
    originalBeginPaint=nullptr;
    originalGetDC=nullptr;
    originalSetDIBits=nullptr;
    patchCount=0;
    const auto* desc=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
                         base+imports.VirtualAddress);
    bool failed=false;
    for(;desc->Name && !failed;++desc) {
        if(desc->Name>=size||desc->FirstThunk>=size||
           !desc->OriginalFirstThunk||desc->OriginalFirstThunk>=size)break;
        const char* module=reinterpret_cast<const char*>(base+desc->Name);
        const bool user=_stricmp(module,"user32.dll")==0;
        const bool gdi=_stricmp(module,"gdi32.dll")==0;
        if(!user&&!gdi)continue;
        const auto* names=reinterpret_cast<const IMAGE_THUNK_DATA32*>(
                              base+desc->OriginalFirstThunk);
        auto* iat=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;++names,++iat) {
            if(IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))continue;
            if(names->u1.AddressOfData>=size)break;
            const auto* entry=reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                  base+names->u1.AddressOfData);
            const char* symbol=reinterpret_cast<const char*>(entry->Name);
            const auto slot=reinterpret_cast<volatile LONG*>(&iat->u1.Function);
            LONG original{};
            if(user&&std::strcmp(symbol,"BeginPaint")==0) {
                if(!applyPatch(slot,static_cast<LONG>(
                    reinterpret_cast<std::uintptr_t>(&scaledBeginPaint)),
                    original)){failed=true;break;}
                originalBeginPaint=reinterpret_cast<BeginPaintFn>(original);
            } else if(user&&std::strcmp(symbol,"GetDC")==0) {
                if(!applyPatch(slot,static_cast<LONG>(
                    reinterpret_cast<std::uintptr_t>(&scaledGetDC)),
                    original)){failed=true;break;}
                originalGetDC=reinterpret_cast<GetDCFn>(original);
            } else if(gdi&&std::strcmp(symbol,"SetDIBitsToDevice")==0) {
                if(!applyPatch(slot,static_cast<LONG>(
                    reinterpret_cast<std::uintptr_t>(&scaledSetDIBits)),
                    original)){failed=true;break;}
                originalSetDIBits=reinterpret_cast<SetDIBitsFn>(original);
            }
        }
    }
    patched=patchCount>0&&!failed;
    return patched;
}
NativeScaleState* mapState(HANDLE& mapping) {
    wchar_t name[192]{};
    const DWORD length=GetEnvironmentVariableW(kNativeScaleMapName,name,192);
    if(!length || length>=192)return nullptr;
    mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping)return nullptr;
    auto* state=static_cast<NativeScaleState*>(
        MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeScaleState)));
    if(!state){CloseHandle(mapping);mapping=nullptr;}
    return state;
}
void closeState(NativeScaleState* state,HANDLE mapping) {
    if(state)UnmapViewOfFile(state);
    if(mapping)CloseHandle(mapping);
}
bool restoreOriginalImports() {
    if(!patched||!patchCount)return false;
    // Multiple imported rendering entry points must all be rolled back.
    for(unsigned i=patchCount;i>0;--i) {
        auto& patch=patches[i-1];
        if(!patch.slot)return false;
        DWORD old{};
        if(!VirtualProtect(const_cast<LONG*>(patch.slot),sizeof(LONG),
                           PAGE_READWRITE,&old))return false;
        const LONG previous=InterlockedCompareExchange(
             patch.slot,patch.previous,patch.inserted);
        DWORD ignored{};
        VirtualProtect(const_cast<LONG*>(patch.slot),sizeof(LONG),
                       old,&ignored);
        FlushInstructionCache(GetCurrentProcess(),
              const_cast<LONG*>(patch.slot),sizeof(LONG));
        if(previous!=patch.inserted)return false;
    }
    patchCount=0;
    originalBeginPaint=nullptr;
    originalGetDC=nullptr;
    originalSetDIBits=nullptr;
    patched=false;
    return true;
}
void attachToAlreadyOpenEditor() {
    // PID/TID-based control block, usable long after auxhost starts.
    wchar_t name[192]{};
    swprintf_s(name,L"Local\\125A_NativeAttach_%lu_%lu",
               GetCurrentProcessId(),GetCurrentThreadId());
    HANDLE mapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,name);
    if(!mapping)return;
    auto* command=static_cast<NativeAttachCommand*>(
        MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeAttachCommand)));
    if(!command){CloseHandle(mapping);return;}
    if(command->status==1 && command->detach && target) {
        // Restore the original DLL import BEFORE releasing the Windows hook.
        // Otherwise the module would retain a pointer into an unloaded DLL.
        if(!restoreOriginalImports()) {
            InterlockedExchange(&command->status,-5);
        } else {
            const BOOL sized=SetWindowPos(target,nullptr,0,0,
                logicalWidth,logicalHeight,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            InvalidateRect(target,nullptr,FALSE);
            target=nullptr;
            zoom=100;
            InterlockedExchange(&command->status,sized?2:-6);
        }
    }
    if(command->status==0 && !target) {
        const HWND hwnd=reinterpret_cast<HWND>(
             static_cast<std::uintptr_t>(command->hwnd));
        DWORD pid=0;
        const DWORD tid=GetWindowThreadProcessId(hwnd,&pid);
        const LONG factor=InterlockedCompareExchange(&command->scale,0,0);
        wchar_t klass[192]{};
        if(!IsWindow(hwnd)||pid!=GetCurrentProcessId()||
           tid!=GetCurrentThreadId()||(factor!=150&&factor!=200)||
           !GetClassNameW(hwnd,klass,192)||
           std::wcscmp(klass,kNativeScaleEditorClass)!=0) {
            InterlockedExchange(&command->status,-1);
        } else {
            RECT client{};
            if(!GetClientRect(hwnd,&client)||client.right<=0||
               client.bottom<=0) {
                InterlockedExchange(&command->status,-2);
            } else {
                logicalWidth=client.right;
                logicalHeight=client.bottom;
                zoom=factor;
                target=hwnd;
                const auto module=reinterpret_cast<HINSTANCE>(
                    GetWindowLongPtrW(hwnd,GWLP_HINSTANCE));
                if(!patchEditorModuleIAT(module,klass)) {
                    target=nullptr;
                    InterlockedExchange(&command->status,-3);
                } else if(!SetWindowPos(hwnd,nullptr,0,0,
                              MulDiv(logicalWidth,zoom,100),
                              MulDiv(logicalHeight,zoom,100),
                              SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)) {
                    InterlockedExchange(&command->status,-4);
                } else {
                    InterlockedExchange(&command->dibImported,originalSetDIBits?1:0);
                    InterlockedExchange(&command->beginImported,originalBeginPaint?1:0);
                    InterlockedExchange(&command->originalWidth,logicalWidth);
                    InterlockedExchange(&command->originalHeight,logicalHeight);
                    InvalidateRect(hwnd,nullptr,FALSE);
                    InterlockedExchange(&command->status,1);
                }
            }
        }
    }
    UnmapViewOfFile(command);
    CloseHandle(mapping);
}
}

extern "C" __declspec(dllexport)
LRESULT CALLBACK NativeCBTHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HCBT_CREATEWND) {
        const auto* creation=reinterpret_cast<const CBT_CREATEWNDW*>(lp);
        auto* cs=creation ? creation->lpcs : nullptr;
        if(cs && cs->lpszClass && !IS_INTRESOURCE(cs->lpszClass) &&
           std::wcscmp(cs->lpszClass,kNativeScaleEditorClass)==0) {
            HANDLE mapping{};
            auto* state=mapState(mapping);
            if(state && !target) {
                const LONG requested=InterlockedCompareExchange(&state->scale,0,0);
                if(requested==150||requested==200) {
                    target=reinterpret_cast<HWND>(wp);
                    zoom=requested;
                    cs->cx=MulDiv(cs->cx,zoom,100);
                    cs->cy=MulDiv(cs->cy,zoom,100);
                    const bool success=patchEditorModuleIAT(cs->hInstance,cs->lpszClass);
                    InterlockedExchange(&state->targetHwnd,
                       static_cast<LONG>(reinterpret_cast<std::uintptr_t>(target)));
                    InterlockedExchange(&state->patchOK,success?1:0);
                    InterlockedExchange(&state->hooked,1);
                }
            }
            closeState(state,mapping);
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
extern "C" __declspec(dllexport)
LRESULT CALLBACK NativeMouseHook(int code,WPARAM wp,LPARAM lp) {
    if(code==HC_ACTION && wp==PM_REMOVE) {
        auto* message=reinterpret_cast<MSG*>(lp);
        if(message && message->message==WM_NULL)
            attachToAlreadyOpenEditor();
        if(message && message->hwnd==target &&
           (message->message==WM_LBUTTONDOWN ||
            message->message==WM_LBUTTONUP ||
            message->message==WM_MOUSEMOVE)) {
            // Modify only the native target's queued client mouse coordinates.
            // Leave real mouse capture/focus/window ownership untouched.
            const int physicalX=GET_X_LPARAM(message->lParam);
            const int physicalY=GET_Y_LPARAM(message->lParam);
            message->lParam=MAKELPARAM(MulDiv(physicalX,100,zoom),
                                       MulDiv(physicalY,100,zoom));
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
