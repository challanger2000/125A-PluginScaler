#include "pluginscaler/formats/VST3Adapter.h"

#if defined(PLUGINSCALER_HAS_VST3_SDK)
#include "pluginterfaces/vst/vsttypes.h"
#endif

namespace pluginscaler::formats {

const char* VST3Adapter::sdkVersionTarget() noexcept {
    return "3.8.1";
}

} // namespace pluginscaler::formats
