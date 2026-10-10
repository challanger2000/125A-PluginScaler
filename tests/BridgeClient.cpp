#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}


bool readExact(HANDLE pipe, void* data, DWORD bytes) {
    auto* p = static_cast<std::uint8_t*>(data);
    DWORD done = 0;
    while (done < bytes) {
        DWORD got = 0;
        if (!ReadFile(pipe, p + done, bytes - done, &got, nullptr) || got == 0)
            return false;
        done += got;
    }
    return true;
}

bool writeExact(HANDLE pipe, const void* data, DWORD bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    DWORD done = 0;
    while (done < bytes) {
        DWORD sent = 0;
        if (!WriteFile(pipe, p + done, bytes - done, &sent, nullptr) || sent == 0)
            return false;
        done += sent;
    }
    return true;
}

HANDLE connectControlPipe(const std::wstring& pipeName) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        HANDLE pipe = CreateFileW(
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            return pipe;
        if (GetLastError() != ERROR_PIPE_BUSY &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        Sleep(20);
    }
    return INVALID_HANDLE_VALUE;
}

bool setMainsHelper(HANDLE pipe, bool active) {
    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    pluginscaler::ipc::ControlMessageHeader request{};
    request.command = pluginscaler::ipc::ControlCommand::SetMains;
    request.arg0 = active ? 1 : 0;

    if (!writeExact(pipe, &request, sizeof(request)))
        return false;

    pluginscaler::ipc::ControlMessageHeader response{};
    if (!readExact(pipe, &response, sizeof(response)))
        return false;

    if (response.responseBytes > 0) {
        std::vector<std::uint8_t> ignored(response.responseBytes);
        if (!readExact(pipe, ignored.data(), response.responseBytes))
            return false;
    }

    return response.magic == pluginscaler::ipc::kControlMagic &&
           response.version == pluginscaler::ipc::kControlVersion &&
           response.command == pluginscaler::ipc::ControlCommand::SetMains &&
           response.status == pluginscaler::ipc::ControlStatus::Ok;
}

bool reconfigureHelper(HANDLE pipe, double sampleRate, std::int32_t blockSize) {
    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    pluginscaler::ipc::ReconfigurePayload payload{};
    payload.sampleRate = sampleRate;
    payload.blockSize = blockSize;

    pluginscaler::ipc::ControlMessageHeader request{};
    request.command = pluginscaler::ipc::ControlCommand::Reconfigure;
    request.payloadBytes = sizeof(payload);

    if (!writeExact(pipe, &request, sizeof(request)) ||
        !writeExact(pipe, &payload, sizeof(payload)))
        return false;

    pluginscaler::ipc::ControlMessageHeader response{};
    if (!readExact(pipe, &response, sizeof(response)))
        return false;

    if (response.responseBytes > 0) {
        std::vector<std::uint8_t> ignored(response.responseBytes);
        if (!readExact(pipe, ignored.data(), response.responseBytes))
            return false;
    }

    return response.magic == pluginscaler::ipc::kControlMagic &&
           response.version == pluginscaler::ipc::kControlVersion &&
           response.command == pluginscaler::ipc::ControlCommand::Reconfigure &&
           response.status == pluginscaler::ipc::ControlStatus::Ok;
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
    const std::wstring controlPipeName = L"\\\\.\\pipe\\125A_PluginScaler_Test_Control_" + suffix;

    pluginscaler::ipc::AudioSharedChannel channel;
    if (!channel.create(mapName, inEvent, outEvent)) return 2;

    std::wstring command =
        quote(helper.wstring()) + L" --serve-vst2-shm " +
        quote(plugin.wstring()) + L" " +
        quote(mapName) + L" " +
        quote(inEvent) + L" " +
        quote(outEvent) + L" " +
        quote(controlPipeName);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return 3;
    }

    CloseHandle(pi.hThread);

    HANDLE controlPipe = connectControlPipe(controlPipeName);
    if (controlPipe == INVALID_HANDLE_VALUE ||
        !reconfigureHelper(controlPipe, 48000.0, 64) ||
        !setMainsHelper(controlPipe, true)) {
        if (controlPipe != INVALID_HANDLE_VALUE)
            CloseHandle(controlPipe);
        TerminateProcess(pi.hProcess, 7);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        return 7;
    }

    auto cleanup = [&] {
        auto* block = channel.block();
        if (block) {
            block->header.state.store(
                static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::Shutdown),
                std::memory_order_release);
            channel.signalInput();
        }
        if (controlPipe != INVALID_HANDLE_VALUE) {
            (void)setMainsHelper(controlPipe, false);
            CloseHandle(controlPipe);
            controlPipe = INVALID_HANDLE_VALUE;
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
        std::fabs(block->outputs[0][0] - 0.145f) < 0.00001f &&
        std::fabs(block->outputs[1][0] - 0.165f) < 0.00001f &&
        std::fabs(block->outputs[0][63] - 1.405f) < 0.00001f &&
        std::fabs(block->outputs[1][63] - 2.685f) < 0.00001f;

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
