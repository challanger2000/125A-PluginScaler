#pragma once

#include "pluginscaler/PluginDescriptor.h"
#include "pluginscaler/CompatibilityProfile.h"

namespace pluginscaler {

class ProxySession {
public:
    explicit ProxySession(PluginDescriptor descriptor);

    const PluginDescriptor& descriptor() const noexcept;
    const CompatibilityProfile& profile() const noexcept;
    void setProfile(CompatibilityProfile profile) noexcept;

private:
    PluginDescriptor descriptor_;
    CompatibilityProfile profile_{};
};

} // namespace pluginscaler
