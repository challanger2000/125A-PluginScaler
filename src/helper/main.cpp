#include "pluginscaler/formats/VST2PluginModule.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/Protocol.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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
        << "outputs=" << result.numOutputs << '\n'
        << "flags=" << result.flags << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.closed) ? 0 : 2;
}

int writeVst2Manifest(const std::filesystem::path& path,
                      const std::filesystem::path& manifestPath) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probe(path);
    if (!(result.loaded && result.opened && result.closed))
        return 20;

    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    if (!out) return 21;

    out << "format=125A-PluginScaler-VST2-Manifest-1\n"
        << "effect=" << result.effectName << "\n"
        << "vendor=" << result.vendor << "\n"
        << "product=" << result.product << "\n"
        << "uniqueId=" << result.uniqueId << "\n"
        << "version=" << result.version << "\n"
        << "programs=" << result.numPrograms << "\n"
        << "params=" << result.numParams << "\n"
        << "inputs=" << result.numInputs << "\n"
        << "outputs=" << result.numOutputs << "\n"
        << "flags=" << result.flags << "\n";
    for (std::size_t i = 0; i < result.parameterDefaults.size(); ++i)
        out << "param." << i << "=" << result.parameterDefaults[i] << "\n";
    return out ? 0 : 22;
}

int runVst2AudioProbe(const std::filesystem::path& path) {
    pluginscaler::formats::VST2PluginModule module;
    const auto result = module.probeAudio(path, 48000.0, 64);

    std::cout << std::fixed << std::setprecision(4)
        << "loaded=" << (result.loaded ? 1 : 0) << '\n'
        << "opened=" << (result.opened ? 1 : 0) << '\n'
        << "configured=" << (result.configured ? 1 : 0) << '\n'
        << "mainsOn=" << (result.mainsOn ? 1 : 0) << '\n'
        << "processed=" << (result.processed ? 1 : 0) << '\n'
        << "mainsOff=" << (result.mainsOff ? 1 : 0) << '\n'
        << "closed=" << (result.closed ? 1 : 0) << '\n'
        << "outL=" << result.firstOutputLeft << '\n'
        << "outR=" << result.firstOutputRight << '\n';

    if (!result.error.empty())
        std::cout << "error=" << result.error << '\n';

    return (result.loaded && result.opened && result.configured &&
            result.mainsOn && result.processed && result.mainsOff &&
            result.closed) ? 0 : 3;
}

int runSharedVst2Server(const std::filesystem::path& path,
                        const std::wstring& mappingName,
                        const std::wstring& inputEvent,
                        const std::wstring& outputEvent) {
    using namespace pluginscaler;

    ipc::AudioSharedChannel channel;
    if (!channel.open(mappingName, inputEvent, outputEvent)) {
        std::cerr << "error=shared-channel-open\n";
        return 10;
    }

    auto* block = channel.block();
    if (!block) return 11;

    formats::VST2PluginModule module;
    std::int32_t configuredBlockSize = 0;
    std::uint32_t configuredSampleRate = 0;
    std::uint32_t appliedParameterGeneration = 0;

    for (;;) {
        if (!channel.waitForInput(std::chrono::seconds(10))) {
            std::cerr << "error=input-timeout\n";
            return 13;
        }

        const auto state = static_cast<ipc::AudioBlockState>(
            block->header.state.load(std::memory_order_acquire));

        if (state == ipc::AudioBlockState::Shutdown)
            break;

        if (state != ipc::AudioBlockState::InputReady) {
            block->header.errorCode = 101;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        if (block->header.frames == 0 || block->header.frames > ipc::kMaxAudioFrames ||
            block->header.sampleRateHz == 0 ||
            block->header.inputChannels > ipc::kMaxAudioChannels ||
            block->header.outputChannels > ipc::kMaxAudioChannels ||
            block->header.midiEventCount > ipc::kMaxMidiEvents ||
            block->header.parameterCount > ipc::kMaxParameters) {
            block->header.errorCode = 102;
            block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                      std::memory_order_release);
            channel.signalOutput();
            continue;
        }

        block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Processing),
                                  std::memory_order_release);

        const auto requestedBlockSize = static_cast<std::int32_t>(block->header.frames);
        const auto requestedSampleRate = block->header.sampleRateHz;
        if (configuredBlockSize != requestedBlockSize ||
            configuredSampleRate != requestedSampleRate) {
            module.close();

            std::string error;
            if (!module.openForProcessing(path, static_cast<double>(requestedSampleRate),
                                          requestedBlockSize, error)) {
                block->header.errorCode = 100;
                block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                          std::memory_order_release);
                channel.signalOutput();
                std::cerr << "error=" << error << '\n';
                return 12;
            }

            configuredBlockSize = requestedBlockSize;
            configuredSampleRate = requestedSampleRate;
            appliedParameterGeneration = 0;
        }

        if (block->header.parameterCount > 0 &&
            appliedParameterGeneration != block->header.parameterGeneration) {
            bool parameterOk = true;
            for (std::uint32_t i = 0; i < block->header.parameterCount; ++i) {
                if (!module.setParameter(static_cast<std::int32_t>(i),
                                         block->parameterValues[i])) {
                    parameterOk = false;
                    break;
                }
            }
            if (!parameterOk) {
                block->header.errorCode = 105;
                block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                          std::memory_order_release);
                channel.signalOutput();
                continue;
            }
            appliedParameterGeneration = block->header.parameterGeneration;
        }

        if (block->header.midiEventCount > 0) {
            std::vector<formats::vst2abi::VstMidiEvent> midi(
                static_cast<std::size_t>(block->header.midiEventCount));
            for (std::uint32_t i = 0; i < block->header.midiEventCount; ++i) {
                auto& dst = midi[static_cast<std::size_t>(i)];
                const auto& src = block->midiEvents[i];
                dst.type = formats::vst2abi::kVstMidiType;
                dst.byteSize = sizeof(formats::vst2abi::VstMidiEvent);
                dst.deltaFrames = src.deltaFrames;
                dst.flags = src.flags;
                for (int b = 0; b < 4; ++b)
                    dst.midiData[b] = static_cast<char>(src.data[b]);
            }
            if (!module.processMidiEvents(midi.data(),
                                          static_cast<std::int32_t>(midi.size()))) {
                block->header.errorCode = 104;
                block->header.state.store(static_cast<std::uint32_t>(ipc::AudioBlockState::Error),
                                          std::memory_order_release);
                channel.signalOutput();
                continue;
            }
        }

        const auto inCount = std::max<std::uint32_t>(1, block->header.inputChannels);
        const auto outCount = std::max<std::uint32_t>(1, block->header.outputChannels);

        std::vector<float*> inputs(inCount);
        std::vector<float*> outputs(outCount);
        for (std::uint32_t ch = 0; ch < inCount; ++ch)
            inputs[ch] = block->inputs[ch];
        for (std::uint32_t ch = 0; ch < outCount; ++ch)
            outputs[ch] = block->outputs[ch];

        const bool ok = module.processReplacing(
            inputs.data(), outputs.data(), static_cast<std::int32_t>(block->header.frames));

        block->header.errorCode = ok ? 0u : 103u;
        block->header.state.store(static_cast<std::uint32_t>(
                                      ok ? ipc::AudioBlockState::OutputReady
                                         : ipc::AudioBlockState::Error),
                                  std::memory_order_release);
        channel.signalOutput();
    }

    module.close();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2")
        return runVst2Probe(argv[2]);

    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2-audio")
        return runVst2AudioProbe(argv[2]);

    if (argc == 4 && std::wstring_view(argv[1]) == L"--write-vst2-manifest")
        return writeVst2Manifest(argv[2], argv[3]);

    if (argc == 6 && std::wstring_view(argv[1]) == L"--serve-vst2-shm")
        return runSharedVst2Server(argv[2], argv[3], argv[4], argv[5]);

    std::cout << "125A PluginScaler Helper\n"
              << "protocol=" << pluginscaler::ipc::kProtocolMajor << "."
              << pluginscaler::ipc::kProtocolMinor << "\n"
              << "usage:\n"
              << "  PluginScalerHelper --probe-vst2 <plugin.dll>\n"
              << "  PluginScalerHelper --probe-vst2-audio <plugin.dll>\n"
              << "  PluginScalerHelper --write-vst2-manifest <plugin.dll> <manifest.txt>\n"
              << "  PluginScalerHelper --serve-vst2-shm <plugin.dll> <map> <in-event> <out-event>\n";
    return 0;
}
