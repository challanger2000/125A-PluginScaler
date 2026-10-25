#include "scaler/geometry.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(#x);                                            \
  } while (false)
int main(int argc, char **) {
  try {
    CHECK(argc ==
          1); // Negative CTest invocation verifies Release checks are live.
    scaler::Geometry g({321, 183});
    for (int cycle = 0; cycle < 1000; ++cycle)
      for (int p : scaler::scales) {
        CHECK(g.select(p, {4096, 4096}));
        CHECK(g.size().width == (321 * p + 50) / 100);
        CHECK(g.nativeX(g.size().width) == 321);
        CHECK(g.nativeX(-5) < 0);
      }
    CHECK(g.select(100, {321, 183}));
    CHECK(g.size().width == 321 && g.size().height == 183);
    CHECK(!g.select(125, {401, 228}));
    CHECK(g.percent() == 100);
    CHECK(!g.select(110, {4096, 4096}));
    for (int p : scaler::scales) {
      CHECK(g.select(p, {4096, 4096}));
      for (int x = -50; x < g.size().width + 50; ++x)
        CHECK(g.nativeX(x) ==
              int(std::floor(double(x) * 321 / g.size().width)));
    }
    uint32_t pixels[]{0xff000000, 0xffffffff, 0xffff0000, 0xff00ff00};
    for (int p : scaler::scales) {
      scaler::Geometry src({2, 2});
      auto d = src.size(p);
      std::vector<uint32_t> out(size_t(d.width) * d.height);
      scaler::scalePixels(pixels, {2, 2}, out.data(), p);
      CHECK(out.front() == pixels[0] && out.back() == pixels[3]);
      if (p % 100 == 0) {
        int k = p / 100;
        for (int y = 0; y < d.height; ++y)
          for (int x = 0; x < d.width; ++x)
            CHECK(out[y * d.width + x] == pixels[(y / k) * 2 + x / k]);
      }
    }
    uint32_t solid[6]{0xff123456, 0xff123456, 0xff123456,
                      0xff123456, 0xff123456, 0xff123456};
    for (int p : scaler::scales) {
      auto d = scaler::Geometry({3, 2}).size(p);
      std::vector<uint32_t> out(size_t(d.width) * d.height);
      scaler::scalePixels(solid, {3, 2}, out.data(), p);
      for (auto v : out)
        CHECK(v == solid[0]);
    }
    uint32_t edge[]{0xff000000, 0xffffffff};
    uint32_t filtered[6]{};
    scaler::scalePixels(edge, {2, 1}, filtered, 150);
    CHECK(filtered[0] == 0xff000000 && filtered[1] == 0xff808080 &&
          filtered[2] == 0xffffffff);
    bool rejected = false;
    try {
      scaler::Geometry invalid({0, 12});
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    CHECK(rejected);
    std::cout << "PASS geometry: all 7 scales, 1000 roundtrips, signed drag, "
                 "screen rejection, pixel filter\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
