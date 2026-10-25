#pragma once

#include <cstdint>

namespace pluginscaler {

enum class GuiMode : std::uint8_t {
    Auto,
    DirectIntegrated,
    OffscreenCapture,
    SeparateHelper
};

enum class RefreshMode : std::uint8_t {
    Auto,
    Normal,
    Full,
    Aggressive
};

enum class InputMode : std::uint8_t {
    Auto,
    AbsoluteScaled,
    NativeDragDelta,
    RelativeMouse
};

enum class ProcessMode : std::uint8_t {
    IsolatedPerInstance,
    SharedWhenSafe
};

struct CompatibilityProfile {
    GuiMode gui{GuiMode::Auto};
    RefreshMode refresh{RefreshMode::Auto};
    InputMode input{InputMode::Auto};
    ProcessMode process{ProcessMode::IsolatedPerInstance};
    double scale{2.0};
};

} // namespace pluginscaler
