#pragma once
#include "vst3.hpp"
#include <algorithm>
#include <limits>
#include <vector>

namespace scaler {
// Bounded in-memory IBStream, used only on non-realtime state paths.
class Stream final : public IBStream {
  int64 position_ = 0;

public:
  std::vector<uint8_t> bytes;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IBStream);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API read(void *buffer, int32 count,
                          int32 *readCount) override {
    if (readCount)
      *readCount = 0;
    if (count < 0 || (!buffer && count))
      return kInvalidArgument;
    int32 n = int32(std::min<int64>(count, int64(bytes.size()) - position_));
    if (n)
      memcpy(buffer, bytes.data() + position_, size_t(n));
    position_ += n;
    if (readCount)
      *readCount = n;
    return n == count ? kResultOk : kResultFalse;
  }
  tresult PLUGIN_API write(void *buffer, int32 count, int32 *written) override {
    if (written)
      *written = 0;
    if (count < 0 || (!buffer && count) || position_ + count > 1024 * 1024)
      return kInvalidArgument;
    bytes.resize(std::max(bytes.size(), size_t(position_ + count)));
    if (count)
      memcpy(bytes.data() + position_, buffer, size_t(count));
    position_ += count;
    if (written)
      *written = count;
    return kResultOk;
  }
  tresult PLUGIN_API seek(int64 offset, int32 mode, int64 *result) override {
    int64 base = mode == kIBSeekSet   ? 0
                 : mode == kIBSeekCur ? position_
                 : mode == kIBSeekEnd ? int64(bytes.size())
                                      : -1;
    if (base < 0 || offset < -base || offset > int64(bytes.size()) - base)
      return kInvalidArgument;
    position_ = base + offset;
    if (result)
      *result = position_;
    return kResultOk;
  }
  tresult PLUGIN_API tell(int64 *result) override {
    if (!result)
      return kInvalidArgument;
    *result = position_;
    return kResultOk;
  }
};
} // namespace scaler
