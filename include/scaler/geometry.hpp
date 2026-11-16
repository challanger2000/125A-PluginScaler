#pragma once

#include <cstdint>

namespace scaler {

// Integer arithmetic is used for repeatable pixel mapping at 125-200%.
inline int scaled_pixels(int value, int percent) {
    return static_cast<int>((static_cast<std::int64_t>(value) * percent + 50) / 100);
}

inline int source_pixel(int viewer_coordinate, int percent) {
    // Coordinates can become negative during a drag outside the editor.
    return static_cast<int>((static_cast<std::int64_t>(viewer_coordinate) * 100) / percent);
}

inline bool valid_zoom(int percent) {
    return percent == 125 || percent == 150 || percent == 175 || percent == 200;
}
} // namespace scaler
