#pragma once
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <atomic>
#include <cstring>

namespace scaler {
using namespace Steinberg;
using namespace Steinberg::Vst;
inline constexpr TUID componentId =
    INLINE_UID(0x125A2000, 0x5CA14E01, 0xB6711130, 0xC0010001);
inline bool same(const char *a, const char *b) {
  return a && b && std::memcmp(a, b, 16) == 0;
}
template <size_t N> void text16(char16 (&out)[N], const char *in) {
  size_t i = 0;
  for (; i + 1 < N && in[i]; ++i)
    out[i] = static_cast<unsigned char>(in[i]);
  out[i] = 0;
}
#define SCALER_REFS                                                            \
  std::atomic<Steinberg::uint32> references_{1};                               \
                                                                               \
public:                                                                        \
  Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }     \
  Steinberg::uint32 PLUGIN_API release() override {                            \
    auto n = --references_;                                                    \
    if (!n)                                                                    \
      delete this;                                                             \
    return n;                                                                  \
  }                                                                            \
                                                                               \
private:
#define SCALER_QUERY(Interface)                                                \
  if (same(iid, INLINE_UID_OF(Interface))) {                                   \
    *obj = static_cast<Interface *>(this);                                     \
    addRef();                                                                  \
    return kResultOk;                                                          \
  }
} // namespace scaler
