#include "pluginscaler/formats/VST2PluginModule.h"
#include "pluginscaler/ipc/Protocol.h"

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int runVst2Probe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);

    std::cout
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "entry=" << result.entryPoint << '\n'
        << "effect=" << result.effectName << '\n'
        << "vendor=" << result.vendor << '\n'
        << "product=" << result.product << '\n'
        << "uniqueId=" << result.uniqueId << '\n'
        << "version=" << result.version << '\n'
        << "programs=" << result.numPrograms << '\n'
        << "params=" << result.numParams << '\n'
        << "inputs=" << result.numInputs << '\n'
        << "outputs=" << result.numOutputs << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.closed) ? 0 : 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2")
        return runVst2Probe(argv[2]);

    std::cout << "125A PluginScaler Helper\n"
              << "protocol=" << pluginscaler::ipc::kProtocolMajor << "."
              << pluginscaler::ipc::kProtocolMinor << "\n"
              << "usage: PluginScalerHelper --probe-vst2 <plugin.dll>\n";
    return 0;
}
