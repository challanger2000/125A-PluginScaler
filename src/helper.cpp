#include "scaler/adapter.hpp"
#include <cstdio>
#include <memory>
#include <shellapi.h>
#include <stdexcept>

namespace {
using namespace scaler;
constexpr UINT controlMessage = WM_APP + 17;
struct Runtime {
  Shared *s;
  HANDLE request, done;
  std::unique_ptr<Adapter> adapter;
  HWND window = nullptr, dragTarget = nullptr;
  DWORD guiThread = GetCurrentThreadId();
  HDC dc = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ oldBitmap = nullptr;
  void *bits = nullptr;
  ~Runtime() {
    if (oldBitmap)
      SelectObject(dc, oldBitmap);
    if (bitmap)
      DeleteObject(bitmap);
    if (dc)
      DeleteDC(dc);
  }
  void tick() {
    if (s->stopping) {
      PostQuitMessage(0);
      return;
    }
    if (s->editorWanted && !s->editorOpen) {
      if (!adapter->openEditor(window)) {
        InterlockedExchange(&s->editorOpen, -1);
        return;
      }
      SetWindowPos(window, nullptr, -15000, -15000, s->width, s->height,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      ShowWindow(window, SW_SHOWNOACTIVATE);
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = s->width;
      info.bmiHeader.biHeight = -s->height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      dc = CreateCompatibleDC(nullptr);
      bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (!dc || !bitmap || !bits) {
        InterlockedExchange(&s->editorOpen, -1);
        return;
      }
      oldBitmap = SelectObject(dc, bitmap);
      InterlockedExchange(&s->editorOpen, 1);
    }
    if (!s->editorWanted && s->editorOpen) {
      adapter->closeEditor();
      ShowWindow(window, SW_HIDE);
      if (oldBitmap) {
        SelectObject(dc, oldBitmap);
        oldBitmap = nullptr;
      }
      if (bitmap) {
        DeleteObject(bitmap);
        bitmap = nullptr;
      }
      if (dc) {
        DeleteDC(dc);
        dc = nullptr;
      }
      bits = nullptr;
      dragTarget = nullptr;
      InterlockedExchange(&s->editorOpen, 0);
    }
    LONG read = s->inputRead,
         write = InterlockedCompareExchange(&s->inputWrite, 0, 0);
    while (read != write) {
      auto input = s->input[uint32_t(read) % 512];
      if (s->editorOpen == 1) {
        POINT p{input.x, input.y};
        HWND target = dragTarget ? dragTarget : window;
        if (!dragTarget) {
          for (int depth = 0; depth < 16; ++depth) {
            HWND child = ChildWindowFromPointEx(
                target, p, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
            if (!child || child == target)
              break;
            MapWindowPoints(target, child, &p, 1);
            target = child;
          }
        } else
          MapWindowPoints(window, target, &p, 1);
        if (input.message == WM_LBUTTONDOWN ||
            input.message == WM_RBUTTONDOWN) {
          dragTarget = target;
          SetFocus(target);
        }
        if (input.message == WM_MOUSEWHEEL) {
          ClientToScreen(target, &p);
        }
        LPARAM l = MAKELPARAM(short(p.x), short(p.y));
        if (input.message == WM_KEYDOWN || input.message == WM_KEYUP ||
            input.message == WM_CHAR)
          l = 1;
        if (input.message == WM_CANCELMODE) {
          SendMessageW(target, WM_CANCELMODE, 0, 0);
          dragTarget = nullptr;
        } else
          SendMessageW(target, input.message, input.keys, l);
        if (input.message == WM_LBUTTONUP || input.message == WM_RBUTTONUP)
          dragTarget = nullptr;
      }
      read = LONG(uint32_t(read) + 1);
      InterlockedExchange(&s->inputRead, read);
    }
    if (s->editorOpen == 1 &&
        InterlockedCompareExchange(&s->frameLock, 1, 0) == 0) {
      // UI-only capture: never called from the DAW audio callback.
      bool ok = PrintWindow(window, dc, PW_CLIENTONLY) != 0;
      // Some Win32 containers do not include owned plugin children in
      // WM_PRINT. Ask each immediate child to render its client explicitly.
      for (HWND child = GetWindow(window, GW_CHILD); child;
           child = GetWindow(child, GW_HWNDNEXT)) {
        POINT origin{0, 0};
        MapWindowPoints(child, window, &origin, 1);
        int saved = SaveDC(dc);
        SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc),
                     PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
      }
      if (ok) {
        GdiFlush();
        memcpy(s->pixels, bits, size_t(s->width) * size_t(s->height) * 4);
        InterlockedIncrement(&s->frameGeneration);
      }
      InterlockedExchange(&s->frameLock, 0);
    }
    // effEditIdle is intentionally not entered concurrently with DSP in this
    // conservative prototype. Plugins requiring it need a measured profile.
    adapter->idle();
  }
  static DWORD WINAPI audioThread(void *ptr) {
    auto &r = *static_cast<Runtime *>(ptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (WaitForSingleObject(r.request, INFINITE) == WAIT_OBJECT_0 &&
           !r.s->stopping) {
      if (static_cast<Op>(r.s->operation) != Op::Audio) {
        if (!PostThreadMessageW(r.guiThread, controlMessage, 0, 0)) {
          InterlockedExchange(&r.s->result, 0);
          SetEvent(r.done);
        }
        continue;
      }
      bool ok = true;
      try {
        switch (static_cast<Op>(r.s->operation)) {
        case Op::Audio:
          if (r.s->frames > maxFrames || r.s->eventCount > maxEvents) {
            ok = false;
            break;
          }
          for (uint32_t i = 0; i < r.s->eventCount; ++i)
            if (r.s->events[i].offset >= std::max(1u, r.s->frames) ||
                (i && r.s->events[i].offset < r.s->events[i - 1].offset))
              ok = false;
          if (ok)
            r.adapter->process();
          break;
        default:
          ok = false;
          break;
        }
      } catch (...) {
        ok = false;
      }
      InterlockedExchange(&r.s->result, ok ? 1 : 0);
      SetEvent(r.done);
    }
    return 0;
  }
  void control() {
    bool ok = true;
    try {
      switch (static_cast<Op>(s->operation)) {
      case Op::Configure:
        adapter->configure(s->sampleRate);
        break;
      case Op::Activate:
        adapter->activate(s->offline != 0);
        break;
      case Op::GetState:
        ok = adapter->getState();
        break;
      case Op::SetState:
        ok = s->stateBytes <= maxState && adapter->setState();
        break;
      default:
        ok = false;
        break;
      }
    } catch (...) {
      ok = false;
    }
    InterlockedExchange(&s->result, ok ? 1 : 0);
    SetEvent(done);
  }
};
LRESULT CALLBACK windowProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_PRINT || m == WM_PRINTCLIENT) {
    RECT rect{};
    GetClientRect(w, &rect);
    FillRect(reinterpret_cast<HDC>(wp), &rect,
             static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  }
  return DefWindowProcW(w, m, wp, lp);
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  if (argc != 3)
    return 2;
  std::wstring name = argv[1], path = argv[2];
  HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!map)
    return 3;
  auto *s = static_cast<scaler::Shared *>(
      MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(scaler::Shared)));
  if (!s || s->version != scaler::protocolVersion ||
      s->bytes != sizeof(scaler::Shared))
    return 4;
  HANDLE request = OpenEventW(SYNCHRONIZE, FALSE, (name + L"-request").c_str());
  HANDLE done =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-done").c_str());
  if (!request || !done)
    return 5;
  int result = 0;
  try {
    Runtime r{s, request, done, {}};
    std::wstring lower = path;
    for (auto &c : lower)
      c = towlower(c);
    r.adapter = lower.size() >= 5 && lower.substr(lower.size() - 5) == L".vst3"
                    ? scaler::loadVst3(path, s)
                    : scaler::loadVst2(path, s);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = windowProc;
    wc.lpszClassName = L"125A.NativeContainer";
    if (!RegisterClassW(&wc))
      throw std::runtime_error("native container registration failed");
    r.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               wc.lpszClassName, L"125A native editor",
                               WS_POPUP | WS_CLIPCHILDREN, -15000, -15000, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!r.window)
      throw std::runtime_error("native container creation failed");
    HANDLE thread =
        CreateThread(nullptr, 0, Runtime::audioThread, &r, 0, nullptr);
    if (!thread)
      throw std::runtime_error("worker creation failed");
    InterlockedExchange(&s->ready, 1);
    SetEvent(done);
    // Timer on this thread, not an HWND callback: the plugin owns a normal
    // message loop.
    UINT_PTR timer = SetTimer(nullptr, 0, 16, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == controlMessage)
        r.control();
      else if (msg.message == WM_TIMER && msg.wParam == timer)
        r.tick();
      else {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    KillTimer(nullptr, timer);
    InterlockedExchange(&s->stopping, 1);
    // Event was opened for synchronize only; a finite wait lets the parent job
    // terminate the blocked worker. Normal shutdown is initiated by the parent.
    if (WaitForSingleObject(thread, 500) == WAIT_OBJECT_0) {
      r.adapter->closeEditor();
      DestroyWindow(r.window);
      CloseHandle(thread);
    } else
      ExitProcess(0); // No destruction while plugin code is still running.
  } catch (const std::exception &e) {
    fprintf(stderr, "helper: %s\n", e.what());
    InterlockedExchange(&s->ready, -1);
    SetEvent(done);
    result = 6;
  }
  UnmapViewOfFile(s);
  CloseHandle(map);
  CloseHandle(request);
  CloseHandle(done);
  return result;
}
