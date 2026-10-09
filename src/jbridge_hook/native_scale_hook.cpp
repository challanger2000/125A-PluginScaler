#include "native_scale_shared.h"
#include <windowsx.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace {
using BeginPaintFn=HDC (WINAPI*)(HWND, LPPAINTSTRUCT);
BeginPaintFn realBeginPaint{};
HWND target{};
int zoom=100;
bool patched=false;
HDC WINAPI scaledBeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
    const HDC dc=realBeginPaint ? realBeginPaint(hwnd,ps) : nullptr;
    if(dc && hwnd==target && zoom>=100 && zoom<=200) {
        SetMapMode(dc, MM_ANISOTROPIC);
        SetWindowExtEx(dc,kLogicalWidth,kLogicalHeight,nullptr);
        SetViewportExtEx(dc,MulDiv(kLogicalWidth,zoom,100),
                            MulDiv(kLogicalHeight,zoom,100),nullptr);
    }
    return dc;
}
bool patchEditorModuleIAT(HINSTANCE registeredInstance,LPCWSTR className) {
    if(patched) return true;
    // Resolve the real window procedure's image instead of assuming that
    // legacy editor painting lives in the host EXE. With jBridge it often
    // resides in the separately loaded VST2 plug-in DLL.
    WNDCLASSEXW klass{};
    klass.cbSize=sizeof(klass);
    if(!GetClassInfoExW(registeredInstance,className,&klass) ||
       !klass.lpfnWndProc) return false;
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<LPCVOID>(klass.lpfnWndProc),
                     &region,sizeof(region)) || region.Type!=MEM_IMAGE)
        return false;
    auto* base=static_cast<unsigned char*>(region.AllocationBase);
    if(!base) return false;
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE) return false;
    const auto imports=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!imports.VirtualAddress) return false;
    realBeginPaint=reinterpret_cast<BeginPaintFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"),"BeginPaint"));
    if(!realBeginPaint) return false;
    auto* desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+imports.VirtualAddress);
    for(;desc->Name;++desc) {
        const char* module=reinterpret_cast<char*>(base+desc->Name);
        if(_stricmp(module,"user32.dll")!=0 || !desc->OriginalFirstThunk)
            continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+desc->OriginalFirstThunk);
        auto* iat=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+desc->FirstThunk);
        for(;names->u1.AddressOfData;++names,++iat) {
            if(IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))continue;
            const auto* imported=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base+names->u1.AddressOfData);
            if(std::strcmp(reinterpret_cast<const char*>(imported->Name),
                           "BeginPaint")!=0)continue;
            DWORD previous{};
            if(!VirtualProtect(&iat->u1.Function,sizeof(iat->u1.Function),
                               PAGE_READWRITE,&previous))return false;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&iat->u1.Function),
                static_cast<LONG>(reinterpret_cast<std::uintptr_t>(&scaledBeginPaint)));
            DWORD ignored{};
            VirtualProtect(&iat->u1.Function,sizeof(iat->u1.Function),
                           previous,&ignored);
            FlushInstructionCache(GetCurrentProcess(),&iat->u1.Function,
                                  sizeof(iat->u1.Function));
            patched=true;
            return true;
        }
    }
    return false;
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
    if(code==HC_ACTION && wp==PM_REMOVE && target) {
        auto* message=reinterpret_cast<MSG*>(lp);
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
