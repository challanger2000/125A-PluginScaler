#include "scaler/geometry.hpp"
#include <cstdlib>
#include <iostream>

int main() {
    for (const int percent : {125, 150, 175, 200}) {
        if (!scaler::valid_zoom(percent)) return 1;
        for (int pixel = 0; pixel < 2400; ++pixel) {
            const int expanded = scaler::scaled_pixels(pixel, percent);
            const int original = scaler::source_pixel(expanded, percent);
            if (std::abs(original - pixel) > 1) {
                std::cerr << "Bad round-trip at " << pixel << " / " << percent << "\n";
                return 2;
            }
        }
    }
    if (scaler::valid_zoom(100) || scaler::valid_zoom(250)) return 3;
    if (scaler::scaled_pixels(762, 150) != 1143) return 4;
    if (scaler::scaled_pixels(358, 200) != 716) return 5;
    if (scaler::source_pixel(-15, 150) != -10) return 6;
    // Zoom must be reduced, preserving aspect ratio, when the full window
    // would exceed the monitor work area.
    const int fit = scaler::fit_percent(150, 860, 611, 1600, 780);
    if (fit < 100 || fit >= 150) return 7;
    if (scaler::scaled_pixels(860, fit) > 1600 ||
        scaler::scaled_pixels(611, fit) > 780) return 8;
    if (scaler::fit_percent(200, 300, 300, 1600, 780) != 200) return 9;
    if (scaler::fit_percent(125, 800, 600, 400, 300) > 50) return 10;
    if (scaler::fit_percent(150, 0, 300, 700, 700) != 0) return 11;
    for (int p = 25; p <= 200; ++p) {
        const int x = scaler::scaled_pixels(860,p);
        const int back = scaler::source_pixel(x,p);
        if (std::abs(back - 860) > (100 + p - 1) / p) return 12;
    }
    std::cout << "Geometry mapping: PASS\n";
}
