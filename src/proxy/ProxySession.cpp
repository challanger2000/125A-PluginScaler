#include "pluginscaler/ProxySession.h"

#include <utility>

namespace pluginscaler {

ProxySession::ProxySession(PluginDescriptor descriptor)
    : descriptor_(std::move(descriptor)) {}

const PluginDescriptor& ProxySession::descriptor() const noexcept {
    return descriptor_;
}

const CompatibilityProfile& ProxySession::profile() const noexcept {
    return profile_;
}

void ProxySession::setProfile(CompatibilityProfile profile) noexcept {
    profile_ = profile;
}

} // namespace pluginscaler
