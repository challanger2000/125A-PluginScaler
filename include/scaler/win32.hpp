#pragma once
#include <cstring>
#include <windows.h>
namespace scaler {
// Win32 documents GetProcAddress as an untyped procedure address. Preserve its
// representation; callers supply the documented export signature/calling
// convention.
template <class Function> Function symbol(HMODULE module, const char *name) {
  auto address = GetProcAddress(module, name);
  static_assert(sizeof(Function) == sizeof(address));
  Function function{};
  std::memcpy(&function, &address, sizeof(function));
  return function;
}
} // namespace scaler
