#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "native_scale_shared.h"
#include <windowsx.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <array>
#include <algorithm>
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
NativeAttachCommand* debugState{};
HANDLE debugMapping{};
bool restoreOriginalImports();
void releaseDebugState() {
    if(debugState){UnmapViewOfFile(debugState);debugState=nullptr;}
    if(debugMapping){CloseHandle(debugMapping);debugMapping=nullptr;}
}

HMODULE imageAtProcedure(const void* proc) {
    if(!proc)return nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(proc,&mbi,sizeof(mbi))||mbi.Type!=MEM_IMAGE)
        return nullptr;
    auto* base=reinterpret_cast<HMODULE>(mbi.AllocationBase);
    wchar_t file[MAX_PATH]{};
    if(!GetModuleFileNameW(base,file,MAX_PATH))return nullptr;
    return base;
}
bool imageNameIs(HMODULE module,const wchar_t* requested) {
    if(!module)return false;
    wchar_t file[MAX_PATH]{};
    if(!GetModuleFileNameW(module,file,MAX_PATH))return false;
    const wchar_t* name=wcsrchr(file,L'\\');
    return _wcsicmp(name?name+1:file,requested)==0;
}
// Legacy VST2 GUIs can register ANSI window classes and/or subclass
// their HWND after creation. Instance-based class lookups alone miss
// real editor procedures. All queries run INSIDE the original GUI thread.
HMODULE imageOfNativeEditor(HWND hwnd,HINSTANCE registered,
                            LPCWSTR className,bool strictPro53) {
    std::array<const void*,5> procedures{};
    procedures[0]=reinterpret_cast<const void*>(
        GetWindowLongPtrW(hwnd,GWLP_WNDPROC));
    procedures[1]=reinterpret_cast<const void*>(
        GetWindowLongPtrA(hwnd,GWLP_WNDPROC));
    procedures[2]=reinterpret_cast<const void*>(
        GetClassLongPtrW(hwnd,GCLP_WNDPROC));
    procedures[3]=reinterpret_cast<const void*>(
        GetClassLongPtrA(hwnd,GCLP_WNDPROC));
    WNDCLASSEXW clsW{};clsW.cbSize=sizeof(clsW);
    if(GetClassInfoExW(registered,className,&clsW))
        procedures[4]=reinterpret_cast<const void*>(clsW.lpfnWndProc);
    for(const void* proc:procedures) {
        const HMODULE module=imageAtProcedure(proc);
        if(module && (!strictPro53||imageNameIs(module,L"Pro-53.dll")))
            return module;
    }
    // An ANSI-registered class may not be retrievable by GetClassInfoExW.
    char classA[192]{};
    if(GetClassNameA(hwnd,classA,sizeof(classA))) {
        WNDCLASSEXA clsA{};clsA.cbSize=sizeof(clsA);
        if(GetClassInfoExA(registered,classA,&clsA)) {
            const HMODULE module=imageAtProcedure(
                              reinterpret_cast<const void*>(clsA.lpfnWndProc));
            if(module && (!strictPro53||imageNameIs(module,L"Pro-53.dll")))
                return module;
        }
    }
    return nullptr;
}

void transformDC(HDC dc) {
    if(!dc||zoom<100||zoom>200)return;
    SetMapMode(dc,MM_ANISOTROPIC);
    SetWindowExtEx(dc,logicalWidth,logicalHeight,nullptr);
    SetViewportExtEx(dc,MulDiv(logicalWidth,zoom,100),
                       MulDiv(logicalHeight,zoom,100),nullptr);
}
HDC WINAPI scaledBeginPaint(HWND hwnd,LPPAINTSTRUCT ps) {
    HDC dc=originalBeginPaint ? originalBeginPaint(hwnd,ps):nullptr;
    if(hwnd==target) {
        if(debugState)InterlockedIncrement(&debugState->diagPaint);
        transformDC(dc);
    }
    return dc;
}
HDC WINAPI scaledGetDC(HWND hwnd) {
    HDC dc=originalGetDC ? originalGetDC(hwnd):nullptr;
    if(hwnd==target) {
        if(debugState)InterlockedIncrement(&debugState->diagGetDC);
        transformDC(dc);
    }
    return dc;
}
int WINAPI scaledSetDIBits(HDC dc,int dx,int dy,DWORD width,DWORD height,
                           int sx,int sy,UINT start,UINT lines,
                           const void* bits,const BITMAPINFO* info,UINT usage) {
    const HWND owner=dc?WindowFromDC(dc):nullptr;
    if(debugState&&target) {
        InterlockedIncrement(&debugState->diagDibCalls);
        InterlockedExchange(&debugState->diagLastWidth,static_cast<LONG>(width));
        InterlockedExchange(&debugState->diagLastHeight,static_cast<LONG>(height));
        InterlockedExchange(&debugState->diagLastStart,static_cast<LONG>(start));
        InterlockedExchange(&debugState->diagLastLines,static_cast<LONG>(lines));
        InterlockedExchange(&debugState->diagLastXSrc,sx);
        InterlockedExchange(&debugState->diagLastYSrc,sy);
        InterlockedExchange(&debugState->diagLastOwner,
                           static_cast<LONG>(reinterpret_cast<std::uintptr_t>(owner)));
    }
    // Pro-53's actual binary (call site 0x100A99B3) uses CLIPPED
    // SetDIBitsToDevice transfers. Requiring source==full image dimensions
    // incorrectly leaves its whole GUI at 100%, although HWND grows.
    //
    // A complete in-memory DIB may draw ANY in-bounds source RECT:
    // dest dx/dy and width/height refer to the selected source region;
    // the DC's viewport makes the logical destination 150%/200%.
    // Keep the conditions explicit: one unsupported base-image blit
    // must not hide behind the more numerous knob redraws.
    LONG reason=0;
    if(!dc)reason|=1;
    if(!bits||!info)reason|=2;
    if(!target||zoom<=100)reason|=4;
    if(owner!=target)reason|=8;
    if(info && info->bmiHeader.biSize<sizeof(BITMAPINFOHEADER))reason|=16;
    if(info && info->bmiHeader.biCompression!=BI_RGB &&
       info->bmiHeader.biCompression!=BI_BITFIELDS)reason|=32;
    if(info && (info->bmiHeader.biWidth<=0 ||
       info->bmiHeader.biHeight==LONG_MIN ||
       info->bmiHeader.biHeight==0))reason|=64;
    if(!width||!height||width>INT_MAX||height>INT_MAX)reason|=128;
    if(sx<0||sy<0)reason|=256;
    if(info && info->bmiHeader.biHeight!=LONG_MIN) {
        if(static_cast<std::uint64_t>(std::max(sx,0))+width>
               static_cast<std::uint64_t>(std::max<LONG>(info->bmiHeader.biWidth,0L)) ||
           static_cast<std::uint64_t>(std::max(sy,0))+height>
               static_cast<std::uint64_t>(std::abs(info->bmiHeader.biHeight)))
           reason|=512;
        if(start!=0||lines!=
               static_cast<DWORD>(std::abs(info->bmiHeader.biHeight)))
           reason|=1024;
    }
    if(reason==0) {
        // A cropped StretchDIBits source rectangle can use a different
        // vertical origin than SetDIBitsToDevice, particularly for top-down
        // DIBs. Preserve source image orientation by scaling the WHOLE
        // bitmap as in our full-frame test; clip its destination to the
        // original update rectangle. This also handles knob/LED repaints.
        if(debugState)InterlockedIncrement(&debugState->diagDibConverted);
        transformDC(dc);
        if(debugState) {
            POINT point{100,100};
            if(LPtoDP(dc,&point,1)) {
                InterlockedExchange(&debugState->diagMappedDx,point.x);
                InterlockedExchange(&debugState->diagMappedDy,point.y);
            }
        }
        const int saved=SaveDC(dc);
        if(saved) {
            IntersectClipRect(dc,dx,dy,dx+static_cast<int>(width),
                                dy+static_cast<int>(height));
            // SetDIBitsToDevice's YSrc specifies the lower edge of
            // the selected scanlines, while our full-image stretch
            // anchors from the top edge. Pro-53's actual x86 assembly
            // explicitly calculates YSrc=bitmapH-top-clippedH.
            const int topOfSelectedSource=
                std::abs(info->bmiHeader.biHeight)-sy-
                static_cast<int>(height);
            const int rendered=StretchDIBits(dc,dx-sx,
                dy-topOfSelectedSource,
                info->bmiHeader.biWidth,
                std::abs(info->bmiHeader.biHeight),
                0,0,info->bmiHeader.biWidth,
                std::abs(info->bmiHeader.biHeight),
                bits,info,usage,SRCCOPY);
            RestoreDC(dc,saved);
            if(debugState) {
                InterlockedExchange(&debugState->diagLastStretchReturn,rendered);
                if(rendered==GDI_ERROR || rendered==0)
                    InterlockedIncrement(&debugState->diagStretchFailure);
                else
                    InterlockedIncrement(&debugState->diagStretchSuccess);
            }
            return rendered==GDI_ERROR ? 0 : rendered;
        }
        // Fail safe: unchanged output rather than a misaligned bitmap.
        return originalSetDIBits ?
            originalSetDIBits(dc,dx,dy,width,height,sx,sy,start,lines,
                              bits,info,usage) : 0;
    }
    // Capture the one failing call, including its actual source format.
    if(debugState&&target) {
        InterlockedIncrement(owner==target?
          &debugState->diagDibSkipped:&debugState->diagDibOtherDC);
        InterlockedExchange(&debugState->diagRejectReason,reason);
        InterlockedExchange(&debugState->diagRejectedWidth,
                            static_cast<LONG>(width));
        InterlockedExchange(&debugState->diagRejectedHeight,
                            static_cast<LONG>(height));
        InterlockedExchange(&debugState->diagRejectedX,sx);
        InterlockedExchange(&debugState->diagRejectedY,sy);
        InterlockedExchange(&debugState->diagRejectedLines,
                            static_cast<LONG>(lines));
        InterlockedExchange(&debugState->diagRejectedStart,
                            static_cast<LONG>(start));
        InterlockedExchange(&debugState->diagRejectedBits,info?
                            static_cast<LONG>(info->bmiHeader.biBitCount):-1);
        InterlockedExchange(&debugState->diagRejectedBitmapWidth,info?
                            info->bmiHeader.biWidth:0);
        InterlockedExchange(&debugState->diagRejectedBitmapHeight,info?
                            info->bmiHeader.biHeight:0);
        InterlockedExchange(&debugState->diagRejectedCompression,info?
                            static_cast<LONG>(info->bmiHeader.biCompression):-1);
    }
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
bool patchEditorModuleIAT(HINSTANCE instance,LPCWSTR className,HMODULE knownModule=nullptr) {
    if(patched)return true;
    HMODULE origin=knownModule;
    if(!origin) {
        WNDCLASSEXW klass{};
        klass.cbSize=sizeof(klass);
        if(!GetClassInfoExW(instance,className,&klass)||
           !klass.lpfnWndProc)return false;
        origin=imageAtProcedure(
               reinterpret_cast<const void*>(klass.lpfnWndProc));
    }
    auto* base=reinterpret_cast<unsigned char*>(origin);
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
    if(failed||!patchCount){
        // Restore every successful import replacement if a later patch
        // fails. Never leave a proprietary renderer half-hooked.
        if(patchCount){patched=true;restoreOriginalImports();}
        return false;
    }
    patched=true;
    return true;
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
            const BOOL sized=!IsWindow(target) ||
                SetWindowPos(target,nullptr,0,0,
                    command->originalOuterWidth ? command->originalOuterWidth : logicalWidth,
                    command->originalOuterHeight ? command->originalOuterHeight : logicalHeight,
                    SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            const HWND root=reinterpret_cast<HWND>(
                static_cast<std::uintptr_t>(command->rootHwnd));
            const BOOL rootSized=!root||!IsWindow(root)||
                SetWindowPos(root,nullptr,0,0,
                  command->rootOuterWidth,command->rootOuterHeight,
                  SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(IsWindow(target))InvalidateRect(target,nullptr,FALSE);
            target=nullptr;
            zoom=100;
            InterlockedExchange(&command->status,sized&&rootSized?2:-6);
            releaseDebugState();
        }
    }
    if(command->status==0 && !target) {
        const HWND hwnd=reinterpret_cast<HWND>(
             static_cast<std::uintptr_t>(command->hwnd));
        DWORD pid=0;
        const DWORD tid=GetWindowThreadProcessId(hwnd,&pid);
        const LONG factor=InterlockedCompareExchange(&command->scale,0,0);
        wchar_t klass[192]{};
        const LONG kind=InterlockedCompareExchange(&command->targetKind,0,0);
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr,exe,MAX_PATH);
        const wchar_t* exeName=wcsrchr(exe,L'\\');
        exeName=exeName?exeName+1:exe;
        const bool realAuxhost=(_wcsicmp(exeName,L"auxhost.exe")==0 ||
                                _wcsicmp(exeName,L"gauxhost.exe")==0);
        const bool nativeWindow=IsWindow(hwnd)&&pid==GetCurrentProcessId()&&
                                tid==GetCurrentThreadId();
        const bool classFound=nativeWindow&&GetClassNameW(hwnd,klass,192)>0;
        const auto registered=reinterpret_cast<HINSTANCE>(
            nativeWindow?GetWindowLongPtrW(hwnd,GWLP_HINSTANCE):0);
        // Pick the actual 32-bit renderer image, not the auxhost's frame.
        // For the real plug-in, ownership must be verified via the PE module.
        const HMODULE editorImage=classFound?
            imageOfNativeEditor(hwnd,registered,klass,kind==1):nullptr;
        if(!nativeWindow)InterlockedExchange(&command->status,-11);
        else if(factor!=150&&factor!=200)
            InterlockedExchange(&command->status,-12);
        else if(!classFound)InterlockedExchange(&command->status,-13);
        else if(kind==1&&!realAuxhost)
            InterlockedExchange(&command->status,-14);
        else if(kind==0 && std::wcscmp(klass,kNativeScaleEditorClass)!=0)
            InterlockedExchange(&command->status,-15);
        else if(!editorImage)InterlockedExchange(&command->status,-16);
        else {
            RECT client{},outer{};
            if(!GetClientRect(hwnd,&client)||!GetWindowRect(hwnd,&outer)||
               client.right<=0||client.bottom<=0||
               (kind==1 && (client.right<300 || client.bottom<160 ||
                             client.right>1600 || client.bottom>1200))) {
                InterlockedExchange(&command->status,-2);
            } else {
                logicalWidth=client.right;
                logicalHeight=client.bottom;
                zoom=factor;
                target=hwnd;
                const bool compatible=patchEditorModuleIAT(
                    registered,klass,editorImage);
                if(!compatible ||
                   (kind==1 && (!originalBeginPaint||!originalSetDIBits))) {
                    if(patched)restoreOriginalImports();
                    target=nullptr;
                    InterlockedExchange(&command->status,-3);
                } else if(!SetWindowPos(hwnd,nullptr,0,0,
                              MulDiv(logicalWidth,zoom,100)+
                                  (outer.right-outer.left-client.right),
                              MulDiv(logicalHeight,zoom,100)+
                                  (outer.bottom-outer.top-client.bottom),
                              SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)) {
                    if(patched)restoreOriginalImports();
                    target=nullptr;
                    InterlockedExchange(&command->status,-4);
                } else {
                    // The original plug-in may be a child of the separate
                    // jBridge presentation frame. Enlarge only a same-process
                    // root explicitly titled Pro-53, never the Studio One frame.
                    HWND root=GetAncestor(hwnd,GA_ROOT);
                    DWORD rootPid=0;
                    if(root)GetWindowThreadProcessId(root,&rootPid);
                    wchar_t caption[256]{};
                    if(root)GetWindowTextW(root,caption,256);
                    const bool trustedRoot=root&&root!=hwnd &&
                        rootPid==GetCurrentProcessId() &&
                        (wcsstr(caption,L"Pro-53")||wcsstr(caption,L"Pro53"));
                    bool rootOK=true;
                    if(trustedRoot) {
                        RECT rootRect{};
                        if(!GetWindowRect(root,&rootRect))rootOK=false;
                        else {
                            const int rootW=rootRect.right-rootRect.left;
                            const int rootH=rootRect.bottom-rootRect.top;
                            const int addW=MulDiv(logicalWidth,zoom,100)-logicalWidth;
                            const int addH=MulDiv(logicalHeight,zoom,100)-logicalHeight;
                            rootOK=SetWindowPos(root,nullptr,0,0,
                                rootW+addW,rootH+addH,
                                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE;
                            if(rootOK) {
                                InterlockedExchange(&command->rootHwnd,
                                    static_cast<LONG>(reinterpret_cast<std::uintptr_t>(root)));
                                InterlockedExchange(&command->rootOuterWidth,rootW);
                                InterlockedExchange(&command->rootOuterHeight,rootH);
                            }
                        }
                    }
                    if(!rootOK) {
                        SetWindowPos(hwnd,nullptr,0,0,
                            outer.right-outer.left,outer.bottom-outer.top,
                            SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                        restoreOriginalImports();
                        target=nullptr;
                        InterlockedExchange(&command->status,-9);
                    }else{
                        InterlockedExchange(&command->dibImported,originalSetDIBits?1:0);
                        InterlockedExchange(&command->beginImported,originalBeginPaint?1:0);
                        InterlockedExchange(&command->originalWidth,logicalWidth);
                        InterlockedExchange(&command->originalHeight,logicalHeight);
                        InterlockedExchange(&command->originalOuterWidth,
                            outer.right-outer.left);
                        InterlockedExchange(&command->originalOuterHeight,
                            outer.bottom-outer.top);
                        // Keep this mapping alive for renderer-call counters
                        // until the original import table is restored.
                        debugMapping=OpenFileMappingW(FILE_MAP_ALL_ACCESS,
                                                       FALSE,name);
                        if(debugMapping) {
                            debugState=static_cast<NativeAttachCommand*>(
                                MapViewOfFile(debugMapping,FILE_MAP_ALL_ACCESS,
                                    0,0,sizeof(NativeAttachCommand)));
                            if(!debugState){CloseHandle(debugMapping);debugMapping=nullptr;}
                        }
                        InvalidateRect(hwnd,nullptr,FALSE);
                        InterlockedExchange(&command->status,1);
                    }
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
            message->message==WM_RBUTTONDOWN ||
            message->message==WM_RBUTTONUP ||
            message->message==WM_MBUTTONDOWN ||
            message->message==WM_MBUTTONUP ||
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
