#pragma once
#include "protocol.hpp"
#include <memory>
#include <string>

namespace scaler {
class Adapter {
public:
  virtual ~Adapter() = default;
  virtual void configure(double rate) = 0;
  virtual void activate(bool state) = 0;
  virtual void process() = 0;
  virtual bool getState() = 0;
  virtual bool setState() = 0;
  virtual bool openEditor(HWND parent) = 0;
  virtual void closeEditor() = 0;
  virtual void idle() = 0;
};
std::unique_ptr<Adapter> loadVst2(const std::wstring &, Shared *);
std::unique_ptr<Adapter> loadVst3(const std::wstring &, Shared *);
} // namespace scaler
