#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>
#include <windowsx.h>

using namespace pluginscaler::formats::vst2abi;

namespace {

constexpr int kNativeWidth = 762;
constexpr int kNativeHeight = 358;

struct State {
    HWND editor{nullptr};
    VstRect rect{0, 0, kNativeHeight, kNativeWidth};
    bool keyDown{false};
    bool knobArmed{false};
    int knobValue{50};
};

void paintLegacyDib(HWND hwnd, HDC dc, State* state) {
    if (!dc || !state) return;

    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kNativeWidth) * kNativeHeight, 0x00202020u);
    auto fill = [&](int l, int t, int r, int b, std::uint32_t rgb) {
        l = std::clamp(l, 0, kNativeWidth);
        r = std::clamp(r, 0, kNativeWidth);
        t = std::clamp(t, 0, kNativeHeight);
        b = std::clamp(b, 0, kNativeHeight);
        for (int y=t; y<b; ++y)
            for (int x=l; x<r; ++x)
                pixels[static_cast<std::size_t>(y) * kNativeWidth + x] = rgb;
    };

    fill(0, 0, kNativeWidth, kNativeHeight, 0x00303030u);
    fill(90, 270, 220, 345, state->keyDown ? 0x0000CC00u : 0x00E0E0E0u);
    fill(390, 90, 465, 165, state->knobArmed ? 0x0000A0FFu : 0x00606060u);
    const int markerY = 155 - state->knobValue;
    fill(420, markerY, 435, markerY + 6, 0x0000FFFFu);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kNativeWidth;
    bmi.bmiHeader.biHeight = -kNativeHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = std::max(1L, rc.right - rc.left);
    const int h = std::max(1L, rc.bottom - rc.top);

    SetDIBitsToDevice(dc, 0, 0,
                      static_cast<DWORD>(w), static_cast<DWORD>(h),
                      0, kNativeHeight - h,
                      0, kNativeHeight,
                      pixels.data(), &bmi, DIB_RGB_COLORS);
}

LRESULT CALLBACK editorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = static_cast<State*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        if (x >= 90 && x < 220 && y >= 270 && y < 345) {
            state->keyDown = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (x >= 390 && x < 465 && y >= 90 && y < 165) {
            state->knobArmed = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 1;
    }
    case WM_MOUSEMOVE:
        if (state->knobArmed && (wp & MK_LBUTTON)) {
            const int y = GET_Y_LPARAM(lp);
            state->knobValue = std::clamp(155 - y, 0, 100);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 1;
    case WM_LBUTTONUP:
        if (state->keyDown || state->knobArmed) {
            state->keyDown = false;
            state->knobArmed = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        paintLegacyDib(hwnd, dc, state);
        EndPaint(hwnd, &ps);
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32,
                           VstIntPtr, void* ptr, float) {
    auto* state = static_cast<State*>(effect ? effect->object : nullptr);
    switch (opcode) {
    case EffOpen: return 1;
    case EffClose:
        delete state;
        delete effect;
        return 1;
    case EffEditGetRect:
        if (state && ptr) {
            *static_cast<VstRect**>(ptr) = &state->rect;
            return 1;
        }
        return 0;
    case EffEditOpen:
        if (state && ptr) {
            static ATOM atom = 0;
            static const wchar_t* cls = L"125A_MockLegacyGdiEditor";
            if (!atom) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = editorProc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = cls;
                atom = RegisterClassW(&wc);
                if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;
            }
            state->editor = CreateWindowExW(
                0, cls, L"125A Mock Legacy GDI",
                WS_CHILD | WS_VISIBLE,
                0, 0, kNativeWidth, kNativeHeight,
                static_cast<HWND>(ptr), nullptr, GetModuleHandleW(nullptr), state);
            return state->editor ? 1 : 0;
        }
        return 0;
    case EffEditClose:
        if (state && state->editor) {
            DestroyWindow(state->editor);
            state->editor = nullptr;
        }
        return 1;
    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A Mock Legacy GDI");
        return 1;
    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A");
        return 1;
    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "MockLegacyGdi");
        return 1;
    case EffGetVendorVersion:
        return 1000;
    default:
        return 1;
    }
}

void __cdecl setParameter(AEffect*, VstInt32, float) {}
float __cdecl getParameter(AEffect*, VstInt32) { return 0.0f; }
void __cdecl processReplacing(AEffect* effect, float**, float** outputs, VstInt32 frames) {
    if (!effect || !outputs) return;
    for (VstInt32 ch=0; ch<effect->numOutputs; ++ch)
        if (outputs[ch]) std::fill(outputs[ch], outputs[ch] + std::max<VstInt32>(frames,0), 0.0f);
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host || host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f) < 2400) return nullptr;
    auto* effect = new AEffect{};
    effect->magic = kEffectMagic;
    effect->dispatcher = dispatch;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->processReplacing = processReplacing;
    effect->numPrograms = 1;
    effect->numParams = 0;
    effect->numInputs = 0;
    effect->numOutputs = 2;
    effect->flags = (1 << 4);
    effect->object = new State{};
    effect->uniqueId = 0x47444931; // GDI1
    effect->version = 1000;
    return effect;
}
