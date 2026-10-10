#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
struct NativeScaleState {
    volatile LONG ready;
    volatile LONG proceed;
    volatile LONG finish;
    volatile LONG scale;
    volatile LONG targetHwnd;
    volatile LONG hooked;
    volatile LONG patchOK;
    volatile LONG mouseDown;
    volatile LONG mouseMove;
    volatile LONG mouseUp;
    volatile LONG mismatch;
    volatile LONG originalLogicalX;
    volatile LONG originalLogicalY;
    volatile LONG rendererMode; // 0=vector GDI, 1=full-frame DIB, 2=clipped DIB
    volatile LONG clippedDibCalls;
};
constexpr wchar_t kNativeScaleMapName[] = L"125A_NATIVE_SCALE_SMOKE_MAP";
constexpr wchar_t kNativeScaleEditorClass[] = L"125A.OriginalUnscaledGdiEditor";
constexpr int kLogicalWidth=200, kLogicalHeight=140;

struct NativeAttachCommand {
    volatile LONG hwnd;
    volatile LONG scale;
    volatile LONG status; // 0=pending, 1=ready, negative=error
    volatile LONG originalWidth;
    volatile LONG originalHeight;
    volatile LONG detach;
    volatile LONG dibImported;
    volatile LONG beginImported;
    volatile LONG targetKind; // 0=controlled mock, 1=original Pro-53.dll only
    volatile LONG originalOuterWidth;
    volatile LONG originalOuterHeight;
    volatile LONG rootHwnd;
    volatile LONG rootOuterWidth;
    volatile LONG rootOuterHeight;
    // Live evidence from calls INTO the original renderer, not from GUI size.
    volatile LONG diagPaint;
    volatile LONG diagGetDC;
    volatile LONG diagDibCalls;
    volatile LONG diagDibConverted;
    volatile LONG diagDibOtherDC;
    volatile LONG diagDibSkipped;
    volatile LONG diagLastWidth;
    volatile LONG diagLastHeight;
    volatile LONG diagLastStart;
    volatile LONG diagLastLines;
    volatile LONG diagLastXSrc;
    volatile LONG diagLastYSrc;
    volatile LONG diagLastOwner;
};
