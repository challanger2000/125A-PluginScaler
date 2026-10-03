#include "pluginscaler/formats/VST2Adapter.h"

namespace pluginscaler::formats {

bool VST2Adapter::headersAvailable() noexcept {
    // Deliberately false in the public repository.
    // No legacy Steinberg VST2 SDK headers are redistributed here.
    return false;
}

} // namespace pluginscaler::formats
