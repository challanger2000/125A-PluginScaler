// Original, synthetic VST2 ABI fixture. No third-party plugin/audio/assets.
#include "scaler/vst2_abi.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <windows.h>
#include <windowsx.h>
namespace abi = pluginscaler::formats::vst2abi;
namespace {
HMODULE instance;
struct Fixture {
  abi::AEffect effect{};
  abi::AudioMasterCallback host;
  std::atomic<float> gain{0.5f};
  std::atomic<float> fault{0.f};
  float note = 0;
  HWND editor = nullptr;
  bool dragging = false;
  void set(float value) {
    gain = std::clamp(value, 0.f, 1.f);
    host(&effect, abi::AudioMasterAutomate, 0, 0, nullptr, gain);
    if (editor)
      InvalidateRect(editor, nullptr, FALSE);
  }
  static LRESULT CALLBACK windowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    auto *f = reinterpret_cast<Fixture *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
      f = static_cast<Fixture *>(
          reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(f));
    }
    if (!f)
      return DefWindowProcW(w, m, wp, lp);
    if (m == WM_PAINT || m == WM_PRINTCLIENT) {
      PAINTSTRUCT ps{};
      HDC dc = m == WM_PAINT ? BeginPaint(w, &ps) : reinterpret_cast<HDC>(wp);
      RECT r{0, 0, 320, 180};
      HBRUSH b = CreateSolidBrush(RGB(20, 30, 40));
      FillRect(dc, &r, b);
      DeleteObject(b);
      RECT bar{16, 72, 304, 108};
      b = CreateSolidBrush(RGB(80, 80, 80));
      FillRect(dc, &bar, b);
      DeleteObject(b);
      bar.right = 16 + int(f->gain * 288);
      b = CreateSolidBrush(RGB(20, 220, 120));
      FillRect(dc, &bar, b);
      DeleteObject(b);
      SetTextColor(dc, RGB(255, 255, 255));
      SetBkMode(dc, TRANSPARENT);
      wchar_t text[100];
      swprintf(text, 100, L"Synthetic fixture | Gain %.3f", double(f->gain));
      TextOutW(dc, 16, 20, text, lstrlenW(text));
      if (m == WM_PAINT)
        EndPaint(w, &ps);
      return 0;
    }
    if (m == WM_LBUTTONDOWN) {
      f->dragging = true;
      f->set((wp & MK_CONTROL) ? 0.5f : float(GET_X_LPARAM(lp) - 16) / 288);
      return 0;
    }
    if (m == WM_MOUSEMOVE && f->dragging) {
      f->set(float(GET_X_LPARAM(lp) - 16) / 288);
      return 0;
    }
    if (m == WM_LBUTTONUP) {
      if (f->dragging)
        f->set(float(GET_X_LPARAM(lp) - 16) / 288);
      f->dragging = false;
      return 0;
    }
    if (m == WM_CANCELMODE) {
      f->dragging = false;
      return 0;
    }
    if (m == WM_MOUSEWHEEL) {
      f->set(f->gain + float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 0.05f);
      return 0;
    }
    if (m == WM_KEYDOWN && (wp == VK_UP || wp == VK_DOWN)) {
      f->set(f->gain + (wp == VK_UP ? 0.05f : -0.05f));
      return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
  }
  static intptr_t __cdecl dispatcher(abi::AEffect *e, int32_t op, int32_t index,
                                     intptr_t value, void *ptr, float) {
    auto &f = *static_cast<Fixture *>(e->object);
    switch (op) {
    case abi::EffClose:
      if (f.editor)
        DestroyWindow(f.editor);
      delete &f;
      return 1;
    case abi::EffGetParamName:
      strcpy(static_cast<char *>(ptr),
             index == 0 ? "Gain" : "Test fault (0=off)");
      return 1;
    case abi::EffGetProgram:
      return 0;
    case abi::EffMainsChanged:
      if (!value)
        f.note = 0;
      return 1;
    case abi::EffProcessEvents: {
      auto *list = static_cast<abi::VstEvents *>(ptr);
      for (int i = 0; i < list->numEvents; ++i) {
        auto *midi = reinterpret_cast<abi::VstMidiEvent *>(list->events[i]);
        if (midi->type != 1 || midi->byteSize != 24)
          return 0;
        auto *data = reinterpret_cast<unsigned char *>(midi->midiData);
        int status = data[0] & 0xf0;
        if (status == 0x90)
          f.note = 0.25f * data[2] / 127.f;
        else if (status == 0x80)
          f.note = 0;
      }
      return 1;
    }
    case abi::EffEditGetRect: {
      static abi::VstRect r{0, 0, 180, 320};
      *static_cast<abi::VstRect **>(ptr) = &r;
      return 1;
    }
    case abi::EffEditOpen: {
      WNDCLASSW wc{};
      wc.lpfnWndProc = windowProc;
      wc.hInstance = instance;
      wc.lpszClassName = L"125A.Fixture";
      RegisterClassW(&wc);
      f.editor = CreateWindowExW(0, wc.lpszClassName, L"Synthetic",
                                 WS_CHILD | WS_VISIBLE, 0, 0, 320, 180,
                                 static_cast<HWND>(ptr), nullptr, instance, &f);
      return f.editor ? 1 : 0;
    }
    case abi::EffEditClose:
      if (f.editor) {
        DestroyWindow(f.editor);
        f.editor = nullptr;
      }
      return 1;
    default:
      return 0;
    }
  }
  static void __cdecl process(abi::AEffect *e, float **in, float **out,
                              int32_t frames) {
    auto &f = *static_cast<Fixture *>(e->object);
    if (f.fault >= 1.f)
      ExitProcess(99);
    if (f.fault >= 0.5f)
      Sleep(200);
    float gain = f.gain;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < frames; ++i)
        out[c][i] = (in[c][i] + f.note) * gain;
  }
  static void __cdecl setParam(abi::AEffect *e, int32_t index, float value) {
    auto &f = *static_cast<Fixture *>(e->object);
    if (index == 0)
      f.gain = value;
    else
      f.fault = value;
  }
  static float __cdecl getParam(abi::AEffect *e, int32_t index) {
    auto &f = *static_cast<Fixture *>(e->object);
    return index == 0 ? float(f.gain) : float(f.fault);
  }
  explicit Fixture(abi::AudioMasterCallback callback) : host(callback) {
    effect.magic = abi::kEffectMagic;
    effect.dispatcher = dispatcher;
    effect.setParameter = setParam;
    effect.getParameter = getParam;
    effect.numPrograms = 1;
    effect.numParams = 2;
    effect.numInputs = 2;
    effect.numOutputs = 2;
    effect.flags = abi::kEffectFlagCanReplacing | abi::kEffectFlagHasEditor;
    effect.object = this;
    effect.uniqueId = 0x125A7001;
    effect.version = 1;
    effect.processReplacing = process;
  }
};
} // namespace
extern "C" __declspec(dllexport)
abi::AEffect *__cdecl VSTPluginMain(abi::AudioMasterCallback host) {
  return &(new Fixture(host))->effect;
}
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    instance = module;
  return TRUE;
}
