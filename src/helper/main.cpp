#include "pluginscaler/formats/VST2PluginModule.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/Protocol.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

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
                        const std::wstring& outputEvent,
                        const std::wstring& controlPipeName) {
    using namespace pluginscaler;

    ipc::AudioSharedChannel channel;
    if (!channel.open(mappingName, inputEvent, outputEvent)) {
        std::cerr << "error=shared-channel-open\n";
        return 10;
    }

    auto* block = channel.block();
    if (!block) return 11;

    formats::VST2PluginModule module;
    std::mutex moduleMutex;
    std::vector<std::uint8_t> persistedChunk;
    std::int32_t persistedChunkIndex = 0;
    std::atomic<bool> controlStop{false};

    std::int32_t configuredBlockSize = 512;
    std::uint32_t configuredSampleRate = 48000;
    std::uint32_t appliedParameterGeneration = 0;

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        std::string error;
        if (!module.openForProcessing(path, 48000.0, 512, error)) {
            std::cerr << "error=" << error << '\n';
            return 12;
        }
    }

    auto readExact = [](HANDLE pipe, void* data, DWORD bytes) -> bool {
        auto* p = static_cast<std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD got = 0;
            if (!ReadFile(pipe, p + done, bytes - done, &got, nullptr) || got == 0)
                return false;
            done += got;
        }
        return true;
    };
    auto writeExact = [](HANDLE pipe, const void* data, DWORD bytes) -> bool {
        const auto* p = static_cast<const std::uint8_t*>(data);
        DWORD done = 0;
        while (done < bytes) {
            DWORD sent = 0;
            if (!WriteFile(pipe, p + done, bytes - done, &sent, nullptr) || sent == 0)
                return false;
            done += sent;
        }
        return true;
    };

    HANDLE controlServerPipe = CreateNamedPipeW(
        controlPipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 64 * 1024, 64 * 1024, 0, nullptr);
    if (controlServerPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "error=control-pipe-create\n";
        return 14;
    }

    std::thread controlThread([&] {
        HANDLE pipe = controlServerPipe;

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            return;
        }

        while (!controlStop.load(std::memory_order_acquire)) {
            ipc::ControlMessageHeader req{};
            if (!readExact(pipe, &req, sizeof(req)))
                break;

            ipc::ControlMessageHeader resp{};
            resp.command = req.command;
            resp.arg0 = req.arg0;

            if (req.magic != ipc::kControlMagic ||
                req.version != ipc::kControlVersion ||
                req.payloadBytes > ipc::kMaxControlPayload) {
                resp.status = ipc::ControlStatus::InvalidRequest;
                if (!writeExact(pipe, &resp, sizeof(resp))) break;
                continue;
            }

            std::vector<std::uint8_t> payload(req.payloadBytes);
            if (req.payloadBytes &&
                !readExact(pipe, payload.data(), req.payloadBytes))
                break;

            std::vector<std::uint8_t> reply;
            {
                std::lock_guard<std::mutex> lock(moduleMutex);
                switch (req.command) {
                case ipc::ControlCommand::GetState: {
                    if (!module.getChunk(req.arg0, reply)) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = reply;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                }
                case ipc::ControlCommand::SetState:
                    if (payload.empty() ||
                        !module.setChunk(req.arg0, payload.data(), payload.size())) {
                        resp.status = ipc::ControlStatus::PluginError;
                    } else {
                        persistedChunk = payload;
                        persistedChunkIndex = req.arg0;
                    }
                    break;
                case ipc::ControlCommand::GetParameters: {
                    const auto count = std::max<std::int32_t>(0, module.numParams());
                    reply.resize(static_cast<std::size_t>(count) * sizeof(float));
                    auto* values = reinterpret_cast<float*>(reply.data());
                    for (std::int32_t i = 0; i < count; ++i)
                        values[i] = module.getParameter(i);
                    break;
                }
                case ipc::ControlCommand::Shutdown:
                    controlStop.store(true, std::memory_order_release);
                    break;
                default:
                    resp.status = ipc::ControlStatus::InvalidRequest;
                    break;
                }
            }

            if (reply.size() > ipc::kMaxControlPayload) {
                reply.clear();
                resp.status = ipc::ControlStatus::PayloadTooLarge;
            }
            resp.responseBytes = static_cast<std::uint32_t>(reply.size());

            if (!writeExact(pipe, &resp, sizeof(resp))) break;
            if (!reply.empty() &&
                !writeExact(pipe, reply.data(), resp.responseBytes))
                break;

            if (req.command == ipc::ControlCommand::Shutdown)
                break;
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    });

    int resultCode = 0;
    for (;;) {
        if (!channel.waitForInput(std::chrono::seconds(10))) {
            if (controlStop.load(std::memory_order_acquire))
                break;
            std::cerr << "error=input-timeout\n";
            resultCode = 13;
            break;
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

        bool ok = true;
        {
            std::lock_guard<std::mutex> lock(moduleMutex);

            const auto requestedBlockSize = static_cast<std::int32_t>(block->header.frames);
            const auto requestedSampleRate = block->header.sampleRateHz;
            if (configuredBlockSize != requestedBlockSize ||
                configuredSampleRate != requestedSampleRate) {
                module.close();

                std::string error;
                if (!module.openForProcessing(path, static_cast<double>(requestedSampleRate),
                                              requestedBlockSize, error)) {
                    std::cerr << "error=" << error << '\n';
                    ok = false;
                } else {
                    configuredBlockSize = requestedBlockSize;
                    configuredSampleRate = requestedSampleRate;
                    appliedParameterGeneration = 0;
                    if (!persistedChunk.empty())
                        ok = module.setChunk(persistedChunkIndex,
                                             persistedChunk.data(),
                                             persistedChunk.size());
                }
            }

            if (ok && block->header.parameterCount > 0 &&
                appliedParameterGeneration != block->header.parameterGeneration) {
                for (std::uint32_t i = 0; i < block->header.parameterCount; ++i) {
                    if (!module.setParameter(static_cast<std::int32_t>(i),
                                             block->parameterValues[i])) {
                        ok = false;
                        break;
                    }
                }
                if (ok)
                    appliedParameterGeneration = block->header.parameterGeneration;
            }

            if (ok && block->header.midiEventCount > 0) {
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
                ok = module.processMidiEvents(midi.data(),
                                              static_cast<std::int32_t>(midi.size()));
            }

            if (ok) {
                const auto inCount = std::max<std::uint32_t>(1, block->header.inputChannels);
                const auto outCount = std::max<std::uint32_t>(1, block->header.outputChannels);
                std::vector<float*> inputs(inCount);
                std::vector<float*> outputs(outCount);
                for (std::uint32_t ch = 0; ch < inCount; ++ch)
                    inputs[ch] = block->inputs[ch];
                for (std::uint32_t ch = 0; ch < outCount; ++ch)
                    outputs[ch] = block->outputs[ch];

                ok = module.processReplacing(
                    inputs.data(), outputs.data(),
                    static_cast<std::int32_t>(block->header.frames));
            }
        }

        block->header.errorCode = ok ? 0u : 103u;
        block->header.state.store(static_cast<std::uint32_t>(
                                      ok ? ipc::AudioBlockState::OutputReady
                                         : ipc::AudioBlockState::Error),
                                  std::memory_order_release);
        channel.signalOutput();
    }

    controlStop.store(true, std::memory_order_release);

    // Wake a blocked control pipe server during normal audio shutdown.
    HANDLE wake = CreateFileW(controlPipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE)
        CloseHandle(wake);

    if (controlThread.joinable())
        controlThread.join();

    {
        std::lock_guard<std::mutex> lock(moduleMutex);
        module.close();
    }
    return resultCode;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2")
        return runVst2Probe(argv[2]);

    if (argc == 3 && std::wstring_view(argv[1]) == L"--probe-vst2-audio")
        return runVst2AudioProbe(argv[2]);

    if (argc == 4 && std::wstring_view(argv[1]) == L"--write-vst2-manifest")
        return writeVst2Manifest(argv[2], argv[3]);

    if (argc == 7 && std::wstring_view(argv[1]) == L"--serve-vst2-shm")
        return runSharedVst2Server(argv[2], argv[3], argv[4], argv[5], argv[6]);

    std::cout << "125A PluginScaler Helper\n"
              << "protocol=" << pluginscaler::ipc::kProtocolMajor << "."
              << pluginscaler::ipc::kProtocolMinor << "\n"
              << "usage:\n"
              << "  PluginScalerHelper --probe-vst2 <plugin.dll>\n"
              << "  PluginScalerHelper --probe-vst2-audio <plugin.dll>\n"
              << "  PluginScalerHelper --write-vst2-manifest <plugin.dll> <manifest.txt>\n"
              << "  PluginScalerHelper --serve-vst2-shm <plugin.dll> <map> <in-event> <out-event> <control-pipe>\n";
    return 0;
}
