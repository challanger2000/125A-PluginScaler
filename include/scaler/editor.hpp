#pragma once
#include "bridge.hpp"
#include "geometry.hpp"
#include "vst3.hpp"
#include <functional>
#include <memory>
#include <vector>

namespace scaler {
class Editor final : public IPlugView {
  SCALER_REFS
  std::shared_ptr<Bridge> lifetime_;
  Bridge &bridge_;
  IEditController *owner_;
  IPlugFrame *frame_ = nullptr;
  HWND window_ = nullptr;
  Geometry geometry_;
  volatile LONG *scale_;
  std::function<void()> notify_;
  std::vector<uint32_t> native_, display_;
  LONG generation_ = -1;
  bool dragging_ = false;
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  LRESULT message(UINT, WPARAM, LPARAM);
  void refresh();
  void menu();
  bool selectScale(int);

public:
  Editor(std::shared_ptr<Bridge>, IEditController *, volatile LONG *,
         std::function<void()>);
  ~Editor();
  tresult PLUGIN_API queryInterface(const TUID, void **) override;
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override;
  tresult PLUGIN_API attached(void *parent, FIDString type) override;
  tresult PLUGIN_API removed() override;
  tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
  tresult PLUGIN_API onKeyDown(char16, int16, int16) override {
    return kResultFalse;
  }
  tresult PLUGIN_API onKeyUp(char16, int16, int16) override {
    return kResultFalse;
  }
  tresult PLUGIN_API getSize(ViewRect *) override;
  tresult PLUGIN_API onSize(ViewRect *) override;
  tresult PLUGIN_API onFocus(TBool state) override {
    if (state && window_)
      SetFocus(window_);
    return kResultOk;
  }
  tresult PLUGIN_API setFrame(IPlugFrame *) override;
  tresult PLUGIN_API canResize() override { return kResultFalse; }
  tresult PLUGIN_API checkSizeConstraint(ViewRect *) override;
};
} // namespace scaler
