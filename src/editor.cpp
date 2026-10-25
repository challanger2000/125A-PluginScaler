#include "scaler/editor.hpp"
#include <windowsx.h>

namespace scaler {
extern HMODULE pluginModule;
static std::mutex surfaceClasses;
static int surfaceCount = 0;
constexpr int toolbar = 28;
Editor::Editor(std::shared_ptr<Bridge> b, IEditController *owner,
               volatile LONG *scale, std::function<void()> notify)
    : lifetime_(b), bridge_(*b), owner_(owner),
      geometry_({int(b->shared->width), int(b->shared->height)}), scale_(scale),
      notify_(std::move(notify)) {
  owner_->addRef();
  geometry_.select(int(*scale_), {16384, 16384});
}
Editor::~Editor() {
  removed();
  if (frame_)
    frame_->release();
  owner_->release();
}
tresult PLUGIN_API Editor::queryInterface(const TUID iid, void **obj) {
  if (!obj)
    return kInvalidArgument;
  *obj = nullptr;
  SCALER_QUERY(IPlugView);
  SCALER_QUERY(FUnknown);
  return kNoInterface;
}
tresult PLUGIN_API Editor::isPlatformTypeSupported(FIDString type) {
  return type && !strcmp(type, kPlatformTypeHWND) ? kResultTrue : kResultFalse;
}
tresult PLUGIN_API Editor::getSize(ViewRect *r) {
  if (!r)
    return kInvalidArgument;
  auto s = geometry_.size();
  *r = ViewRect(0, 0, s.width, s.height + toolbar);
  return kResultOk;
}
tresult PLUGIN_API Editor::checkSizeConstraint(ViewRect *r) {
  return getSize(r);
}
tresult PLUGIN_API Editor::setFrame(IPlugFrame *frame) {
  if (frame)
    frame->addRef();
  if (frame_)
    frame_->release();
  frame_ = frame;
  return kResultOk;
}
tresult PLUGIN_API Editor::onSize(ViewRect *r) {
  if (!r)
    return kInvalidArgument;
  auto s = geometry_.size();
  if (r->getWidth() != s.width || r->getHeight() != s.height + toolbar)
    return kResultFalse;
  if (window_)
    SetWindowPos(window_, nullptr, 0, 0, r->getWidth(), r->getHeight(),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  return kResultOk;
}
tresult PLUGIN_API Editor::attached(void *parent, FIDString type) {
  std::lock_guard<std::mutex> lock(surfaceClasses);
  if (window_ || !parent || isPlatformTypeSupported(type) != kResultTrue)
    return kInvalidArgument;
  WNDCLASSW wc{};
  wc.lpfnWndProc = procedure;
  wc.hInstance = pluginModule;
  wc.lpszClassName = L"125A.ScaledSurface";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return kInternalError;
  auto s = geometry_.size();
  window_ = CreateWindowExW(0, wc.lpszClassName, L"125A PluginScaler prototype",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, s.width,
                            s.height + toolbar, static_cast<HWND>(parent),
                            nullptr, wc.hInstance, this);
  if (!window_) {
    if (!surfaceCount)
      UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return kInternalError;
  }
  ++surfaceCount;
  SetTimer(window_, 1, 16, nullptr);
  return kResultOk;
}
tresult PLUGIN_API Editor::removed() {
  std::lock_guard<std::mutex> lock(surfaceClasses);
  if (window_) {
    KillTimer(window_, 1);
    if (dragging_) {
      bridge_.input({WM_CANCELMODE, 0, 0, 0});
      ReleaseCapture();
      dragging_ = false;
    }
    DestroyWindow(window_);
    window_ = nullptr;
    if (--surfaceCount == 0)
      UnregisterClassW(L"125A.ScaledSurface", pluginModule);
  }
  if (bridge_.shared)
    InterlockedExchange(&bridge_.shared->editorWanted, 0);
  return kResultOk;
}
LRESULT CALLBACK Editor::procedure(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  auto *self = reinterpret_cast<Editor *>(GetWindowLongPtrW(w, GWLP_USERDATA));
  if (m == WM_NCCREATE) {
    self = static_cast<Editor *>(
        reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
    self->window_ = w;
    SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->message(m, wp, lp) : DefWindowProcW(w, m, wp, lp);
}
void Editor::refresh() {
  auto *s = bridge_.shared;
  notify_();
  if (!s || s->editorOpen != 1 || s->frameGeneration == generation_)
    return;
  if (InterlockedCompareExchange(&s->frameLock, 1, 0) != 0)
    return;
  auto width = s->width, height = s->height;
  if (width <= 0 || height <= 0 || width > LONG(maxWidth) ||
      height > LONG(maxHeight)) {
    InterlockedExchange(&s->frameLock, 0);
    return;
  }
  native_.assign(s->pixels, s->pixels + size_t(width) * size_t(height));
  generation_ = s->frameGeneration;
  InterlockedExchange(&s->frameLock, 0);
  auto size = geometry_.size();
  display_.resize(size_t(size.width) * size_t(size.height));
  scalePixels(native_.data(), {int(width), int(height)}, display_.data(),
              geometry_.percent());
  InvalidateRect(window_, nullptr, FALSE);
}
void Editor::menu() {
  if (dragging_)
    return;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                  &monitor);
  Size available{monitor.rcWork.right - monitor.rcWork.left - 32,
                 monitor.rcWork.bottom - monitor.rcWork.top - toolbar - 64};
  HMENU menu = CreatePopupMenu();
  for (auto p : scales) {
    auto title = std::to_wstring(p) + L" %";
    AppendMenuW(menu,
                MF_STRING | (!geometry_.fits(p, available) ? MF_GRAYED : 0) |
                    (p == geometry_.percent() ? MF_CHECKED : 0),
                UINT_PTR(p), title.c_str());
  }
  POINT pt{0, toolbar};
  ClientToScreen(window_, &pt);
  int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y,
                                0, window_, nullptr);
  DestroyMenu(menu);
  if (selected)
    selectScale(selected);
}
bool Editor::selectScale(int selected) {
  if (dragging_)
    return false;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST),
                  &monitor);
  Size available{monitor.rcWork.right - monitor.rcWork.left - 32,
                 monitor.rcWork.bottom - monitor.rcWork.top - toolbar - 64};
  Geometry previous = geometry_;
  if (!geometry_.select(selected, available))
    return false;
  ViewRect rect;
  getSize(&rect);
  if (!frame_ || frame_->resizeView(this, &rect) != kResultOk) {
    geometry_ = previous;
    return false;
  }
  InterlockedExchange(scale_, selected);
  generation_ = -1;
  refresh();
  return true;
}
LRESULT Editor::message(UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_COMMAND && lp == 0)
    return selectScale(LOWORD(wp)) ? 1 : 0;
  if (m == WM_TIMER) {
    refresh();
    return 0;
  }
  if (m == WM_ERASEBKGND)
    return 1;
  if (m == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(window_, &ps);
    RECT rect{};
    GetClientRect(window_, &rect);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    auto title =
        L"Zoom " + std::to_wstring(geometry_.percent()) +
        (bridge_.healthy() ? L" %  |  prototype" : L" %  |  disconnected");
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 6, 6, title.c_str(), int(title.size()));
    if (!display_.empty()) {
      auto s = geometry_.size();
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = s.width;
      info.bmiHeader.biHeight = -s.height;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      SetDIBitsToDevice(dc, 0, toolbar, s.width, s.height, 0, 0, 0, s.height,
                        display_.data(), &info, DIB_RGB_COLORS);
    }
    EndPaint(window_, &ps);
    return 0;
  }
  if (m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) {
    bridge_.input({m, uint32_t(wp), 0, 0});
    return 0;
  }
  if (m == WM_CAPTURECHANGED || m == WM_CANCELMODE) {
    if (dragging_) {
      dragging_ = false;
      bridge_.input({WM_CANCELMODE, 0, 0, 0});
    }
    return 0;
  }
  if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_LBUTTONUP ||
      m == WM_RBUTTONUP || m == WM_MOUSEMOVE || m == WM_MOUSEWHEEL ||
      m == WM_LBUTTONDBLCLK) {
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (m == WM_MOUSEWHEEL)
      ScreenToClient(window_, &p);
    if (m == WM_LBUTTONDOWN && p.y < toolbar) {
      menu();
      return 0;
    }
    if (!dragging_ &&
        (p.y < toolbar || p.x < 0 || p.x >= geometry_.size().width ||
         p.y >= toolbar + geometry_.size().height))
      return 0;
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) {
      dragging_ = true;
      SetCapture(window_);
      SetFocus(window_);
    }
    bool ok = bridge_.input({m, uint32_t(wp), geometry_.nativeX(p.x),
                             geometry_.nativeY(p.y - toolbar)});
    if (!ok) {
      bridge_.disconnect();
      dragging_ = false;
      ReleaseCapture();
    }
    if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) {
      dragging_ = false;
      ReleaseCapture();
    }
    return 0;
  }
  return DefWindowProcW(window_, m, wp, lp);
}
} // namespace scaler
