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
    std::cout << "Geometry mapping: PASS\n";
}
