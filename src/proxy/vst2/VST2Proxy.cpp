#define NOMINMAX
#include "pluginscaler/formats/vst2/VST2LegacyABI.h"
#include "pluginscaler/ipc/AudioSharedChannel.h"
#include "pluginscaler/ipc/ControlProtocol.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <cstddef>
#include <string>
#include <thread>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct ProxyManifest {
    std::string effectName{"125A PluginScaler"};
    std::string vendor{"125A"};
    std::string product{"PluginScaler VST2 Proxy"};
    VstInt32 uniqueId{0x31535041};
    VstInt32 version{100};
    VstInt32 numPrograms{1};
    VstInt32 numParams{0};
    VstInt32 numInputs{2};
    VstInt32 numOutputs{2};
    VstInt32 flags{1 << 4};
    std::vector<float> parameterDefaults;
    bool valid{false};
};

struct ProxySettings {
    std::wstring helper;
    std::wstring target;
    std::wstring manifest;
    int scalePercent{200};
    bool sidecarLoaded{false};
};

HMODULE g_moduleHandle{nullptr};

struct ProxyInstance {
    AEffect effect{};
    AudioMasterCallback host{nullptr};
    ProxyManifest manifest{};
    ProxySettings settings{};

    pluginscaler::ipc::AudioSharedChannel channel;
    PROCESS_INFORMATION helperProcess{};
    HANDLE controlPipe{INVALID_HANDLE_VALUE};
    std::vector<std::uint8_t> stateChunk;
    VstRect editorRect{};
    HWND editorWindow{nullptr};
    HWND editorSurrogate{nullptr};
    HWND editorSurface{nullptr};
    HWND editorHost{nullptr};
    int scalePercent{200};
    std::vector<std::uint8_t> editorBitmap;
    std::uint32_t editorBitmapWidth{0};
    std::uint32_t editorBitmapHeight{0};
    std::uint32_t editorBitmapStride{0};
    bool dragActive{false};
    int dragSurfaceStartX{0};
    int dragSurfaceStartY{0};
    int dragNativeStartX{0};
    int dragNativeStartY{0};
    bool editorOpen{false};

    double sampleRate{48000.0};
    VstInt32 blockSize{512};
    bool mainsOn{false};
    bool bridgeStarted{false};
    std::uint64_t sequence{0};
    std::array<pluginscaler::ipc::MidiSharedEvent, pluginscaler::ipc::kMaxMidiEvents> pendingMidi{};
    std::uint32_t pendingMidiCount{0};
    std::vector<float> parameterValues;
    std::uint32_t parameterGeneration{1};
};

std::atomic<std::uint64_t> g_instanceCounter{1};

std::wstring getenvWide(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};

    std::wstring value(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0) return {};
    value.resize(written);
    return value;
}

std::filesystem::path proxyModulePath() {
    if (!g_moduleHandle) return {};
    std::wstring buffer(32768, L'\0');
    const DWORD written = GetModuleFileNameW(
        g_moduleHandle, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size())
        return {};
    buffer.resize(written);
    return std::filesystem::path(buffer);
}

std::wstring trimWide(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::filesystem::path resolveSidecarPath(const std::filesystem::path& baseDir,
                                         const std::wstring& value) {
    if (value.empty()) return {};
    std::filesystem::path p(value);
    if (p.is_relative())
        p = baseDir / p;
    return p.lexically_normal();
}

ProxySettings loadSettings() {
    ProxySettings settings;

    const auto modulePath = proxyModulePath();
    const auto baseDir = modulePath.empty()
        ? std::filesystem::path{}
        : modulePath.parent_path();

    if (!modulePath.empty()) {
        auto configPath = modulePath;
        configPath.replace_extension(L".pluginscaler.ini");

        std::wifstream in(configPath);
        if (in) {
            std::wstring line;
            while (std::getline(in, line)) {
                line = trimWide(line);
                if (line.empty() || line[0] == L'#' || line[0] == L';')
                    continue;
                const auto eq = line.find(L'=');
                if (eq == std::wstring::npos)
                    continue;
                const auto key = trimWide(line.substr(0, eq));
                const auto value = trimWide(line.substr(eq + 1));

                if (key == L"helper")
                    settings.helper = resolveSidecarPath(baseDir, value).wstring();
                else if (key == L"target")
                    settings.target = resolveSidecarPath(baseDir, value).wstring();
                else if (key == L"manifest")
                    settings.manifest = resolveSidecarPath(baseDir, value).wstring();
                else if (key == L"scale") {
                    try {
                        settings.scalePercent = std::clamp(std::stoi(value), 100, 400);
                    } catch (...) {
                        settings.scalePercent = 200;
                    }
                }
            }
            settings.sidecarLoaded = true;
        }
    }

    if (const auto env = getenvWide(L"PLUGINSCALER_HELPER_X86"); !env.empty())
        settings.helper = env;
    if (const auto env = getenvWide(L"PLUGINSCALER_TARGET_VST2"); !env.empty())
        settings.target = env;
    if (const auto env = getenvWide(L"PLUGINSCALER_TARGET_MANIFEST"); !env.empty())
        settings.manifest = env;
    if (const auto env = getenvWide(L"PLUGINSCALER_SCALE_PERCENT"); !env.empty()) {
        try {
            settings.scalePercent = std::clamp(std::stoi(env), 100, 400);
        } catch (...) {
            settings.scalePercent = 200;
        }
    }

    if (settings.helper.empty() && !baseDir.empty())
        settings.helper = (baseDir / L"PluginScalerHelper-x86.exe").wstring();

    return settings;
}

std::wstring quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

ProxyManifest loadManifest(const std::wstring& manifestPath) {
    ProxyManifest m;
    if (manifestPath.empty()) return m;

    std::ifstream in(std::filesystem::path(manifestPath), std::ios::binary);
    if (!in) return m;

    std::string line;
    bool formatOk = false;
    while (std::getline(in, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const auto key = line.substr(0, pos);
        const auto value = line.substr(pos + 1);
        try {
            if (key == "format") formatOk = (value == "125A-PluginScaler-VST2-Manifest-1");
            else if (key == "effect") m.effectName = value;
            else if (key == "vendor") m.vendor = value;
            else if (key == "product") m.product = value;
            else if (key == "uniqueId") m.uniqueId = static_cast<VstInt32>(std::stol(value));
            else if (key == "version") m.version = static_cast<VstInt32>(std::stol(value));
            else if (key == "programs") m.numPrograms = static_cast<VstInt32>(std::stol(value));
            else if (key == "params") m.numParams = static_cast<VstInt32>(std::stol(value));
            else if (key == "inputs") m.numInputs = static_cast<VstInt32>(std::stol(value));
            else if (key == "outputs") m.numOutputs = static_cast<VstInt32>(std::stol(value));
            else if (key == "flags") m.flags = static_cast<VstInt32>(std::stol(value));
            else if (key.rfind("param.", 0) == 0) {
                const auto index = static_cast<std::size_t>(std::stoul(key.substr(6)));
                if (m.parameterDefaults.size() <= index)
                    m.parameterDefaults.resize(index + 1, 0.0f);
                m.parameterDefaults[index] = std::stof(value);
            }
        } catch (...) {
            return ProxyManifest{};
        }
    }

    m.valid = formatOk &&
        m.numPrograms >= 0 && m.numParams >= 0 &&
        m.numInputs >= 0 && m.numOutputs >= 0 &&
        m.numParams <= static_cast<VstInt32>(pluginscaler::ipc::kMaxParameters);
    if (m.valid)
        m.parameterDefaults.resize(static_cast<std::size_t>(m.numParams), 0.0f);
    return m;
}

ProxyInstance* self(AEffect* effect) noexcept {
    return effect ? static_cast<ProxyInstance*>(effect->object) : nullptr;
}

void zeroOutputs(AEffect* effect, float** outputs, VstInt32 frames) noexcept {
    if (!effect || !outputs || frames <= 0) return;
    const auto channels = std::max<VstInt32>(0, effect->numOutputs);
    for (VstInt32 ch = 0; ch < channels; ++ch) {
        if (outputs[ch])
            std::fill(outputs[ch], outputs[ch] + frames, 0.0f);
    }
}

bool readExact(HANDLE pipe, void* data, DWORD bytes) noexcept {
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

bool writeExact(HANDLE pipe, const void* data, DWORD bytes) noexcept {
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

bool controlCall(ProxyInstance* inst,
                 pluginscaler::ipc::ControlCommand command,
                 std::int32_t arg0,
                 const void* payload,
                 std::uint32_t payloadBytes,
                 std::vector<std::uint8_t>& reply) {
    reply.clear();
    if (!inst || inst->controlPipe == INVALID_HANDLE_VALUE ||
        payloadBytes > pluginscaler::ipc::kMaxControlPayload)
        return false;

    pluginscaler::ipc::ControlMessageHeader req{};
    req.command = command;
    req.arg0 = arg0;
    req.payloadBytes = payloadBytes;

    if (!writeExact(inst->controlPipe, &req, sizeof(req)) ||
        (payloadBytes && !writeExact(inst->controlPipe, payload, payloadBytes)))
        return false;

    pluginscaler::ipc::ControlMessageHeader resp{};
    if (!readExact(inst->controlPipe, &resp, sizeof(resp)) ||
        resp.magic != pluginscaler::ipc::kControlMagic ||
        resp.version != pluginscaler::ipc::kControlVersion ||
        resp.command != command ||
        resp.status != pluginscaler::ipc::ControlStatus::Ok ||
        resp.responseBytes > pluginscaler::ipc::kMaxControlPayload)
        return false;

    reply.resize(resp.responseBytes);
    return resp.responseBytes == 0 ||
           readExact(inst->controlPipe, reply.data(), resp.responseBytes);
}

void refreshParametersFromHelper(ProxyInstance* inst) {
    if (!inst) return;
    std::vector<std::uint8_t> reply;
    if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetParameters,
                     0, nullptr, 0, reply) ||
        reply.size() % sizeof(float) != 0)
        return;

    const auto count = reply.size() / sizeof(float);
    const auto copyCount = (std::min)(count, inst->parameterValues.size());
    const auto* values = reinterpret_cast<const float*>(reply.data());
    for (std::size_t i = 0; i < copyCount; ++i)
        inst->parameterValues[i] = values[i];
    ++inst->parameterGeneration;
    if (inst->parameterGeneration == 0)
        inst->parameterGeneration = 1;
}

bool captureEditorBitmap(ProxyInstance* inst) {
    if (!inst) return false;
    std::vector<std::uint8_t> reply;
    if (!controlCall(inst, pluginscaler::ipc::ControlCommand::CaptureEditor,
                     0, nullptr, 0, reply) ||
        reply.size() < sizeof(pluginscaler::ipc::EditorBitmapHeader))
        return false;

    pluginscaler::ipc::EditorBitmapHeader header{};
    std::memcpy(&header, reply.data(), sizeof(header));
    if (header.format != 1 || header.width == 0 || header.height == 0 ||
        header.strideBytes < header.width * 4u)
        return false;

    const std::uint64_t pixels =
        static_cast<std::uint64_t>(header.strideBytes) * header.height;
    if (pixels > pluginscaler::ipc::kMaxControlPayload ||
        sizeof(header) + pixels != reply.size())
        return false;

    inst->editorBitmap.assign(reply.begin() + sizeof(header), reply.end());
    inst->editorBitmapWidth = header.width;
    inst->editorBitmapHeight = header.height;
    inst->editorBitmapStride = header.strideBytes;
    return true;
}

bool forwardScaledMouse(ProxyInstance* inst, UINT message,
                        WPARAM wp, LPARAM lp) {
    if (!inst || !inst->editorOpen ||
        inst->editorBitmapWidth == 0 || inst->editorBitmapHeight == 0)
        return false;

    RECT rc{};
    if (!inst->editorSurface || !GetClientRect(inst->editorSurface, &rc))
        return false;
    const int surfaceWidth = rc.right - rc.left;
    const int surfaceHeight = rc.bottom - rc.top;
    if (surfaceWidth <= 0 || surfaceHeight <= 0)
        return false;

    const int scaledX = GET_X_LPARAM(lp);
    const int scaledY = GET_Y_LPARAM(lp);

    const auto absoluteNativeX = [&] {
        return std::clamp(
            scaledX * static_cast<int>(inst->editorBitmapWidth) / surfaceWidth,
            0, static_cast<int>(inst->editorBitmapWidth) - 1);
    };
    const auto absoluteNativeY = [&] {
        return std::clamp(
            scaledY * static_cast<int>(inst->editorBitmapHeight) / surfaceHeight,
            0, static_cast<int>(inst->editorBitmapHeight) - 1);
    };

    int nativeX = absoluteNativeX();
    int nativeY = absoluteNativeY();

    if (message == WM_LBUTTONDOWN) {
        inst->dragActive = true;
        inst->dragSurfaceStartX = scaledX;
        inst->dragSurfaceStartY = scaledY;
        inst->dragNativeStartX = nativeX;
        inst->dragNativeStartY = nativeY;
    } else if ((message == WM_MOUSEMOVE || message == WM_LBUTTONUP) &&
               inst->dragActive) {
        nativeX = std::clamp(
            inst->dragNativeStartX + (scaledX - inst->dragSurfaceStartX),
            0, static_cast<int>(inst->editorBitmapWidth) - 1);
        nativeY = std::clamp(
            inst->dragNativeStartY + (scaledY - inst->dragSurfaceStartY),
            0, static_cast<int>(inst->editorBitmapHeight) - 1);
    }

    pluginscaler::ipc::EditorMousePayload mouse{};
    mouse.message = message;
    mouse.x = nativeX;
    mouse.y = nativeY;
    mouse.keyFlags = static_cast<std::uint32_t>(wp);

    std::vector<std::uint8_t> ignored;
    const bool ok = controlCall(inst, pluginscaler::ipc::ControlCommand::SendEditorMouse,
                                0, &mouse, sizeof(mouse), ignored);
    if (message == WM_LBUTTONUP)
        inst->dragActive = false;
    return ok;
}

LRESULT CALLBACK scalerSurfaceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* inst = reinterpret_cast<ProxyInstance*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        inst = static_cast<ProxyInstance*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(inst));
    }

    if (!inst)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        if (msg == WM_LBUTTONDOWN)
            SetCapture(hwnd);
        const bool sent = forwardScaledMouse(inst, msg, wp, lp);
        if (msg == WM_LBUTTONUP && GetCapture() == hwnd)
            ReleaseCapture();
        if (sent) {
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        return sent ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        if (captureEditorBitmap(inst) && !inst->editorBitmap.empty()) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(inst->editorBitmapWidth);
            bmi.bmiHeader.biHeight = -static_cast<LONG>(inst->editorBitmapHeight);
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc,
                          0, 0, rc.right - rc.left, rc.bottom - rc.top,
                          0, 0,
                          static_cast<int>(inst->editorBitmapWidth),
                          static_cast<int>(inst->editorBitmapHeight),
                          inst->editorBitmap.data(),
                          &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool ensureScalerSurfaceClass() {
    static const wchar_t* kClassName = L"125A_PluginScaler_ScaledSurface";
    static bool ready = false;
    if (ready) return true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = scalerSurfaceProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    ATOM atom = RegisterClassW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    ready = true;
    return true;
}

void stopBridge(ProxyInstance* inst) noexcept {
    if (!inst) return;

    if (inst->bridgeStarted) {
        if (inst->editorOpen && inst->controlPipe != INVALID_HANDLE_VALUE) {
            if (inst->editorSurface && IsWindow(inst->editorSurface))
                DestroyWindow(inst->editorSurface);
            inst->editorSurface = nullptr;
            std::vector<std::uint8_t> ignored;
            (void)controlCall(inst, pluginscaler::ipc::ControlCommand::CloseEditor,
                              0, nullptr, 0, ignored);
            inst->editorWindow = nullptr;
            inst->editorSurrogate = nullptr;
            inst->editorOpen = false;
        }
        if (inst->controlPipe != INVALID_HANDLE_VALUE) {
            std::vector<std::uint8_t> ignored;
            (void)controlCall(inst, pluginscaler::ipc::ControlCommand::Shutdown,
                              0, nullptr, 0, ignored);
            CloseHandle(inst->controlPipe);
            inst->controlPipe = INVALID_HANDLE_VALUE;
        }

        if (auto* block = inst->channel.block()) {
            block->header.state.store(
                static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::Shutdown),
                std::memory_order_release);
            inst->channel.signalInput();
        }

        if (inst->helperProcess.hProcess) {
            const DWORD wait = WaitForSingleObject(inst->helperProcess.hProcess, 1500);
            if (wait == WAIT_TIMEOUT)
                TerminateProcess(inst->helperProcess.hProcess, 1);
            CloseHandle(inst->helperProcess.hProcess);
        }
        if (inst->helperProcess.hThread)
            CloseHandle(inst->helperProcess.hThread);

        inst->helperProcess = {};
        inst->channel.close();
        inst->bridgeStarted = false;
    }
}

bool startBridge(ProxyInstance* inst) {
    if (!inst) return false;
    if (inst->bridgeStarted) return true;

    const std::wstring& helper = inst->settings.helper;
    const std::wstring& target = inst->settings.target;
    if (helper.empty() || target.empty())
        return false;

    const auto id = g_instanceCounter.fetch_add(1, std::memory_order_relaxed);
    const std::wstring suffix =
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(id);

    const std::wstring mapName = L"Local\\125A_PluginScaler_Proxy_Map_" + suffix;
    const std::wstring inEvent = L"Local\\125A_PluginScaler_Proxy_In_" + suffix;
    const std::wstring outEvent = L"Local\\125A_PluginScaler_Proxy_Out_" + suffix;
    const std::wstring controlPipeName = L"\\\\.\\pipe\\125A_PluginScaler_Control_" + suffix;

    if (!inst->channel.create(mapName, inEvent, outEvent))
        return false;

    std::wstring command =
        quote(helper) + L" --serve-vst2-shm " +
        quote(target) + L" " +
        quote(mapName) + L" " +
        quote(inEvent) + L" " +
        quote(outEvent) + L" " +
        quote(controlPipeName);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        inst->channel.close();
        return false;
    }

    HANDLE control = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100 && control == INVALID_HANDLE_VALUE; ++attempt) {
        control = CreateFileW(controlPipeName.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (control != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_PIPE_BUSY &&
            GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        WaitNamedPipeW(controlPipeName.c_str(), 20);
        Sleep(10);
    }

    if (control == INVALID_HANDLE_VALUE) {
        TerminateProcess(pi.hProcess, 2);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        inst->channel.close();
        return false;
    }

    inst->controlPipe = control;
    inst->helperProcess = pi;
    inst->bridgeStarted = true;

    if (!inst->stateChunk.empty()) {
        std::vector<std::uint8_t> ignored;
        if (controlCall(inst, pluginscaler::ipc::ControlCommand::SetState,
                        0, inst->stateChunk.data(),
                        static_cast<std::uint32_t>(inst->stateChunk.size()), ignored))
            refreshParametersFromHelper(inst);
    }

    return true;
}

VstIntPtr __cdecl dispatcher(AEffect* effect, VstInt32 opcode, VstInt32 index,
                             VstIntPtr value, void* ptr, float opt) {
    auto* inst = self(effect);
    if (!inst) return 0;

    switch (opcode) {
    case EffOpen:
        return 1;

    case EffClose:
        stopBridge(inst);
        delete inst;
        return 1;

    case EffSetSampleRate:
        inst->sampleRate = opt > 0.0f ? static_cast<double>(opt) : 48000.0;
        return 1;

    case EffSetBlockSize:
        inst->blockSize = value > 0 ? static_cast<VstInt32>(value) : 512;
        return 1;

    case EffMainsChanged:
        inst->mainsOn = value != 0;
        if (inst->mainsOn)
            return startBridge(inst) ? 1 : 0;
        stopBridge(inst);
        return 1;

    case EffEditGetRect: {
        if (!ptr || !startBridge(inst)) return 0;
        std::vector<std::uint8_t> reply;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetEditorRect,
                         0, nullptr, 0, reply) ||
            reply.size() != sizeof(pluginscaler::ipc::EditorRectPayload))
            return 0;
        pluginscaler::ipc::EditorRectPayload remote{};
        std::memcpy(&remote, reply.data(), sizeof(remote));
        const int nativeWidth = remote.right - remote.left;
        const int nativeHeight = remote.bottom - remote.top;
        const int scaledWidth = nativeWidth * inst->scalePercent / 100;
        const int scaledHeight = nativeHeight * inst->scalePercent / 100;
        inst->editorRect.left = 0;
        inst->editorRect.top = 0;
        inst->editorRect.right = static_cast<std::int16_t>(scaledWidth);
        inst->editorRect.bottom = static_cast<std::int16_t>(scaledHeight);
        *static_cast<VstRect**>(ptr) = &inst->editorRect;
        return 1;
    }

    case EffEditOpen: {
        if (!ptr || !startBridge(inst)) return 0;
        std::vector<std::uint8_t> reply;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::OpenEditor,
                         0, nullptr, 0, reply) ||
            reply.size() != sizeof(pluginscaler::ipc::EditorOpenResult))
            return 0;

        pluginscaler::ipc::EditorOpenResult result{};
        std::memcpy(&result, reply.data(), sizeof(result));
        HWND editor = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(result.editorWindow));
        HWND surrogate = reinterpret_cast<HWND>(
            static_cast<std::uintptr_t>(result.surrogateWindow));
        HWND parent = static_cast<HWND>(ptr);
        if (!editor || !surrogate || !IsWindow(editor) || !IsWindow(surrogate) ||
            !ensureScalerSurfaceClass())
            return 0;

        const int width = inst->editorRect.right - inst->editorRect.left;
        const int height = inst->editorRect.bottom - inst->editorRect.top;
        HWND surface = CreateWindowExW(
            0, L"125A_PluginScaler_ScaledSurface", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, width, height,
            parent, nullptr, GetModuleHandleW(nullptr), inst);
        if (!surface)
            return 0;

        inst->editorWindow = editor;
        inst->editorSurrogate = surrogate;
        inst->editorSurface = surface;
        inst->editorHost = parent;
        inst->dragActive = false;
        inst->editorOpen = true;
        InvalidateRect(surface, nullptr, TRUE);
        UpdateWindow(surface);
        return 1;
    }

    case EffEditClose: {
        if (!inst->bridgeStarted || inst->controlPipe == INVALID_HANDLE_VALUE)
            return 1;

        if (inst->editorSurface && IsWindow(inst->editorSurface))
            DestroyWindow(inst->editorSurface);
        inst->editorSurface = nullptr;

        std::vector<std::uint8_t> ignored;
        const bool ok = controlCall(inst, pluginscaler::ipc::ControlCommand::CloseEditor,
                                    0, nullptr, 0, ignored);
        inst->editorWindow = nullptr;
        inst->editorSurrogate = nullptr;
        inst->dragActive = false;
        inst->editorOpen = false;
        return ok ? 1 : 0;
    }

    case EffGetChunk: {
        if (!ptr || !startBridge(inst)) return 0;
        std::vector<std::uint8_t> reply;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::GetState,
                         index, nullptr, 0, reply) ||
            reply.empty())
            return 0;
        inst->stateChunk = std::move(reply);
        *static_cast<void**>(ptr) = inst->stateChunk.data();
        return static_cast<VstIntPtr>(inst->stateChunk.size());
    }

    case EffSetChunk: {
        if (!ptr || value <= 0 || !startBridge(inst) ||
            static_cast<std::uint64_t>(value) > pluginscaler::ipc::kMaxControlPayload)
            return 0;
        std::vector<std::uint8_t> ignored;
        if (!controlCall(inst, pluginscaler::ipc::ControlCommand::SetState,
                         index, ptr, static_cast<std::uint32_t>(value), ignored))
            return 0;
        inst->stateChunk.assign(static_cast<const std::uint8_t*>(ptr),
                                static_cast<const std::uint8_t*>(ptr) +
                                    static_cast<std::size_t>(value));
        refreshParametersFromHelper(inst);
        return 1;
    }

    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.effectName.c_str());
        return 1;

    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.vendor.c_str());
        return 1;

    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, inst->manifest.product.c_str());
        return 1;

    case EffGetVendorVersion:
        return inst->manifest.version;

    case EffProcessEvents: {
        if (!ptr) return 0;
        auto* events = static_cast<VstEvents*>(ptr);
        if (events->numEvents < 0) return 0;

        const auto count = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(events->numEvents),
            pluginscaler::ipc::kMaxMidiEvents);
        auto** eventPtrs = reinterpret_cast<VstEvent**>(
            reinterpret_cast<std::uint8_t*>(events) + offsetof(VstEvents, events));

        std::uint32_t written = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto* ev = eventPtrs[i];
            if (!ev || ev->type != kVstMidiType ||
                ev->byteSize < static_cast<VstInt32>(sizeof(VstMidiEvent)))
                continue;
            auto* midi = reinterpret_cast<VstMidiEvent*>(ev);
            auto& dst = inst->pendingMidi[written++];
            dst.deltaFrames = midi->deltaFrames;
            dst.flags = midi->flags;
            for (int b = 0; b < 4; ++b)
                dst.data[b] = static_cast<std::uint8_t>(midi->midiData[b]);
        }
        inst->pendingMidiCount = written;
        return 1;
    }

    case EffCanDo:
        return 0;

    default:
        return 0;
    }
}

void __cdecl processReplacing(AEffect* effect, float** inputs, float** outputs,
                               VstInt32 frames) {
    auto* inst = self(effect);
    if (!inst || !inst->mainsOn || frames <= 0 ||
        frames > static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioFrames)) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    if (!startBridge(inst)) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    auto* block = inst->channel.block();
    if (!block) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    const std::uint32_t inChannels =
        static_cast<std::uint32_t>(std::clamp<VstInt32>(
            effect->numInputs, 0, static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioChannels)));
    const std::uint32_t outChannels =
        static_cast<std::uint32_t>(std::clamp<VstInt32>(
            effect->numOutputs, 0, static_cast<VstInt32>(pluginscaler::ipc::kMaxAudioChannels)));

    block->header.inputChannels = inChannels;
    block->header.outputChannels = outChannels;
    block->header.frames = static_cast<std::uint32_t>(frames);
    block->header.sampleRateHz =
        static_cast<std::uint32_t>(std::llround(inst->sampleRate));
    block->header.sequence = ++inst->sequence;
    block->header.errorCode = 0;
    block->header.midiEventCount = inst->pendingMidiCount;
    for (std::uint32_t i = 0; i < inst->pendingMidiCount; ++i)
        block->midiEvents[i] = inst->pendingMidi[i];

    block->header.parameterCount = static_cast<std::uint32_t>(
        std::min<std::size_t>(inst->parameterValues.size(),
                              pluginscaler::ipc::kMaxParameters));
    block->header.parameterGeneration = inst->parameterGeneration;
    for (std::uint32_t i = 0; i < block->header.parameterCount; ++i)
        block->parameterValues[i] = inst->parameterValues[i];

    for (std::uint32_t ch = 0; ch < inChannels; ++ch) {
        if (inputs && inputs[ch])
            std::copy(inputs[ch], inputs[ch] + frames, block->inputs[ch]);
        else
            std::fill(block->inputs[ch], block->inputs[ch] + frames, 0.0f);
    }

    for (std::uint32_t ch = 0; ch < outChannels; ++ch)
        std::fill(block->outputs[ch], block->outputs[ch] + frames, 0.0f);

    block->header.state.store(
        static_cast<std::uint32_t>(pluginscaler::ipc::AudioBlockState::InputReady),
        std::memory_order_release);

    inst->pendingMidiCount = 0;

    if (!inst->channel.signalInput() ||
        !inst->channel.waitForOutput(std::chrono::milliseconds(1000))) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    const auto state = static_cast<pluginscaler::ipc::AudioBlockState>(
        block->header.state.load(std::memory_order_acquire));

    if (state != pluginscaler::ipc::AudioBlockState::OutputReady ||
        block->header.errorCode != 0) {
        zeroOutputs(effect, outputs, frames);
        return;
    }

    for (std::uint32_t ch = 0; ch < outChannels; ++ch) {
        if (outputs && outputs[ch])
            std::copy(block->outputs[ch], block->outputs[ch] + frames, outputs[ch]);
    }
}

void __cdecl process(AEffect* effect, float** inputs, float** outputs, VstInt32 frames) {
    processReplacing(effect, inputs, outputs, frames);
}

void __cdecl setParameter(AEffect* effect, VstInt32 index, float value) {
    auto* inst = self(effect);
    if (!inst || index < 0 ||
        index >= static_cast<VstInt32>(inst->parameterValues.size()))
        return;
    inst->parameterValues[static_cast<std::size_t>(index)] =
        std::clamp(value, 0.0f, 1.0f);
    ++inst->parameterGeneration;
    if (inst->parameterGeneration == 0)
        inst->parameterGeneration = 1;
}

float __cdecl getParameter(AEffect* effect, VstInt32 index) {
    auto* inst = self(effect);
    if (!inst || index < 0 ||
        index >= static_cast<VstInt32>(inst->parameterValues.size()))
        return 0.0f;
    return inst->parameterValues[static_cast<std::size_t>(index)];
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        g_moduleHandle = instance;
    return TRUE;
}

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host) return nullptr;

    const VstIntPtr hostVersion = host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f);
    if (hostVersion <= 0) return nullptr;

    auto* inst = new ProxyInstance{};
    inst->host = host;
    inst->settings = loadSettings();
    inst->manifest = loadManifest(inst->settings.manifest);
    inst->parameterValues = inst->manifest.parameterDefaults;
    inst->scalePercent = inst->settings.scalePercent;

    inst->effect.magic = kEffectMagic;
    inst->effect.dispatcher = dispatcher;
    inst->effect.process = process;
    inst->effect.setParameter = setParameter;
    inst->effect.getParameter = getParameter;
    inst->effect.numPrograms = inst->manifest.numPrograms;
    inst->effect.numParams = inst->manifest.numParams;
    inst->effect.numInputs = inst->manifest.numInputs;
    inst->effect.numOutputs = inst->manifest.numOutputs;
    inst->effect.flags = inst->manifest.flags;
    inst->effect.object = inst;
    inst->effect.uniqueId = inst->manifest.uniqueId;
    inst->effect.version = inst->manifest.version;
    inst->effect.processReplacing = processReplacing;

    return &inst->effect;
}
