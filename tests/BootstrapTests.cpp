#include "pluginscaler/CompatibilityProfile.h"
#include "pluginscaler/formats/VST2Adapter.h"
#include "pluginscaler/formats/VST3Adapter.h"
#include "pluginscaler/ipc/Protocol.h"

#include <cmath>
#include <cstring>

int main() {
    using namespace pluginscaler;

    CompatibilityProfile profile;
    if (std::abs(profile.scale - 2.0) > 0.0001) return 1;
    if (ipc::kProtocolMagic != 0x31535041u) return 2;
    if (std::strcmp(formats::VST3Adapter::sdkVersionTarget(), "3.8.1") != 0) return 3;
    if (formats::VST2Adapter::headersAvailable()) return 4;

    return 0;
}
