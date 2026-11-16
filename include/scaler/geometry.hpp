#pragma once

#include <cstdint>
#include <algorithm>

namespace scaler {

// Integer arithmetic is used for repeatable pixel mapping at 125-200%.
inline int scaled_pixels(int value, int percent) {
    return static_cast<int>((static_cast<std::int64_t>(value) * percent + 50) / 100);
}

inline int source_pixel(int viewer_coordinate, int percent) {
    // Coordinates can become negative during a drag outside the editor.
    return static_cast<int>((static_cast<std::int64_t>(viewer_coordinate) * 100) / percent);
}

inline int fit_percent(int wanted, int srcW, int srcH, int maxW, int maxH) {
    if (wanted <= 0 || srcW <= 0 || srcH <= 0 || maxW <= 0 || maxH <= 0) return 0;
    int p = (std::min)(wanted, (std::min)(maxW * 100 / srcW, maxH * 100 / srcH));
    while (p > 1 && (scaled_pixels(srcW,p) > maxW || scaled_pixels(srcH,p) > maxH)) --p;
    return (std::max)(p,1);
}
inline bool valid_zoom(int percent) {
    return percent == 125 || percent == 150 || percent == 175 || percent == 200;
}
} // namespace scaler
