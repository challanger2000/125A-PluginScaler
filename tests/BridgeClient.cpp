#include "pluginscaler/ipc/AudioSharedChannel.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;

    const std::filesystem::path helper = argv[1];
    const std::filesystem::path plugin = argv[2];

    const DWORD pid = GetCurrentProcessId();
    const std::wstring suffix = std::to_wstring(pid);
    const std::wstring mapName = L"Local\\125A_PluginScaler_Test_Map_" + suffix;
    const std::wstring inEvent = L"Local\\125A_PluginScaler_Test_In_" + suffix;
    const std::wstring outEvent = L"Local\\125A_PluginScaler_Test_Out_" + suffix;

    pluginscaler::ipc::AudioSharedChannel channel;
    if (!channel.create(mapName, inEvent, outEvent)) return 2;

    std::wstring command =
        quote(helper.wstring()) + L" --serve-vst2-shm " +
        quote(plugin.wstring()) + L" " +
        quote(mapName) + L" " +
        quote(inEvent) + L" " +
        quote(outEvent);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return 3;
    }

    CloseHandle(pi.hThread);

    auto cleanup = [&] {
        auto* block = channel.block();
        if (block) {
            block->header.state.store(
                static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::Shutdown),
                std::memory_order_release);
            channel.signalInput();
        }
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
    };

    auto* block = channel.block();
    if (!block) {
        cleanup();
        return 4;
    }

    block->header.inputChannels = 2;
    block->header.outputChannels = 2;
    block->header.frames = 64;
    block->header.sampleRateHz = 48000;
    block->header.sequence = 1;

    for (std::uint32_t i = 0; i < block->header.frames; ++i) {
        block->inputs[0][i] = static_cast<float>(i + 1) / 100.0f;
        block->inputs[1][i] = static_cast<float>((i + 1) * 2) / 100.0f;
        block->outputs[0][i] = 0.0f;
        block->outputs[1][i] = 0.0f;
    }

    block->header.state.store(
        static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::InputReady),
        std::memory_order_release);

    if (!channel.signalInput() ||
        !channel.waitForOutput(std::chrono::seconds(5))) {
        cleanup();
        return 5;
    }

    const auto state = static_cast<pluginscaler::ipc::AudioBlockState>(
        block->header.state.load(std::memory_order_acquire));

    const bool ok =
        state == pluginscaler::ipc::AudioBlockState::OutputReady &&
        block->header.errorCode == 0 &&
        std::fabs(block->outputs[0][0] - 0.02f) < 0.00001f &&
        std::fabs(block->outputs[1][0] - 0.04f) < 0.00001f &&
        std::fabs(block->outputs[0][63] - 1.28f) < 0.00001f &&
        std::fabs(block->outputs[1][63] - 2.56f) < 0.00001f;

    std::cout
        << "state=" << static_cast<unsigned>(state) << "\n"
        << "error=" << block->header.errorCode << "\n"
        << "outL0=" << block->outputs[0][0] << "\n"
        << "outR0=" << block->outputs[1][0] << "\n"
        << "outL63=" << block->outputs[0][63] << "\n"
        << "outR63=" << block->outputs[1][63] << "\n";

    cleanup();
    return ok ? 0 : 6;
}
