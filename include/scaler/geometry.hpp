#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace scaler {
inline constexpr std::array<int, 7> scales{100, 125, 150, 175, 200, 300, 400};
struct Size {
  int width, height;
};
class Geometry {
  Size native_;
  int percent_ = 100;

public:
  explicit Geometry(Size native) : native_(native) {
    if (native.width <= 0 || native.height <= 0 || native.width > 4096 ||
        native.height > 4096)
      throw std::invalid_argument("invalid native editor dimensions");
  }
  Size size(int percent) const {
    return {(native_.width * percent + 50) / 100,
            (native_.height * percent + 50) / 100};
  }
  Size size() const { return size(percent_); }
  int percent() const { return percent_; }
  bool fits(int percent, Size available) const {
    auto s = size(percent);
    return s.width <= available.width && s.height <= available.height;
  }
  bool select(int percent, Size available) {
    if (std::find(scales.begin(), scales.end(), percent) == scales.end() ||
        !fits(percent, available))
      return false;
    percent_ = percent;
    return true;
  }
  // Signed coordinates deliberately remain unbounded during a captured drag.
  int nativeX(int x) const {
    return static_cast<int>(
        std::floor(double(x) * native_.width / size().width));
  }
  int nativeY(int y) const {
    return static_cast<int>(
        std::floor(double(y) * native_.height / size().height));
  }
};
// Independent two-stage pixel-art filter: nearest-neighbour integer
// enlargement, followed by bilinear sampling. Whole factors bypass
// interpolation entirely.
inline void scalePixels(const uint32_t *src, Size s, uint32_t *dst,
                        int percent) {
  Geometry g(s);
  auto d = g.size(percent);
  if (percent % 100 == 0) {
    int k = percent / 100;
    for (int y = 0; y < d.height; ++y)
      for (int x = 0; x < d.width; ++x)
        dst[y * d.width + x] = src[(y / k) * s.width + x / k];
    return;
  }
  const int k = (percent + 99) / 100;
  auto axis = [](int p, int n, int out, int up, int &a, int &b, double &mix) {
    double q = (double(p) + 0.5) * n * up / out - 0.5;
    int lo = static_cast<int>(std::floor(q));
    mix = q - lo;
    a = std::clamp(lo, 0, n * up - 1) / up;
    b = std::clamp(lo + 1, 0, n * up - 1) / up;
  };
  for (int y = 0; y < d.height; ++y)
    for (int x = 0; x < d.width; ++x) {
      int ax, bx, ay, by;
      double fx, fy;
      axis(x, s.width, d.width, k, ax, bx, fx);
      axis(y, s.height, d.height, k, ay, by, fy);
      uint32_t pixel = 0xff000000;
      for (int shift = 0; shift < 24; shift += 8) {
        auto c = [&](int px, int py) {
          return double((src[py * s.width + px] >> shift) & 255);
        };
        double top = c(ax, ay) * (1 - fx) + c(bx, ay) * fx,
               bottom = c(ax, by) * (1 - fx) + c(bx, by) * fx;
        pixel |= uint32_t(std::lround(top * (1 - fy) + bottom * fy)) << shift;
      }
      dst[y * d.width + x] = pixel;
    }
}
} // namespace scaler
