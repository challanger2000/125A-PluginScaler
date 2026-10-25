#pragma once
#include "protocol.hpp"
#include <mutex>
#include <string>

namespace scaler {
class Bridge {
  HANDLE mapping_ = nullptr, request_ = nullptr, done_ = nullptr,
         process_ = nullptr, job_ = nullptr;
  volatile LONG inFlight_ = 0, broken_ = 0;
  std::mutex control_;
  bool transact(Op op, DWORD timeout);

public:
  Shared *shared = nullptr;
  Bridge() = default;
  Bridge(const Bridge &) = delete;
  ~Bridge();
  bool open(const std::wstring &directory, const std::wstring &target,
            const std::wstring &helper);
  bool configure(double rate);
  bool activate(bool state);
  bool audio(uint32_t frames, bool offline);
  bool state(bool restore);
  bool healthy() const { return shared && !broken_; }
  bool input(Input input);
  void disconnect() { InterlockedExchange(&broken_, 1); }
};
std::wstring moduleDirectory(HMODULE module);
} // namespace scaler
