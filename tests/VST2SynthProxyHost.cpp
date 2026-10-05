#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {

std::atomic<int> gAutomationCount{0};
std::atomic<int> gAutomationIndex{-1};
std::atomic<std::uint32_t> gAutomationValueBits{0};

LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND createHostWindow() {
    static const wchar_t* kClassName = L"125A_PluginScaler_TestHost";
    static ATOM atom = 0;
    if (!atom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = hostWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        atom = RegisterClassW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
    }
    return CreateWindowExW(0, kClassName, L"PluginScaler Test Host",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

BOOL CALLBACK countChildProc(HWND, LPARAM param) {
    auto* count = reinterpret_cast<int*>(param);
    ++(*count);
    return TRUE;
}

int childCount(HWND parent) {
    int count = 0;
    EnumChildWindows(parent, countChildProc, reinterpret_cast<LPARAM>(&count));
    return count;
}

VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32 index,
                               VstIntPtr, void*, float opt) {
    switch (opcode) {
    case AudioMasterVersion: return 2400;
    case AudioMasterGetSampleRate: return 48000;
    case AudioMasterGetBlockSize: return 64;
    case AudioMasterGetVendorVersion: return 1000;
    case AudioMasterAutomate: {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(opt));
        std::memcpy(&bits, &opt, sizeof(bits));
        gAutomationIndex.store(index, std::memory_order_relaxed);
        gAutomationValueBits.store(bits, std::memory_order_relaxed);
        gAutomationCount.fetch_add(1, std::memory_order_release);
        return 1;
    }
    default: return 0;
    }
}

bool sendMidi(AEffect* effect, std::uint8_t status, std::uint8_t note,
              std::uint8_t velocity, VstInt32 deltaFrames) {
    VstMidiEvent midi{};
    midi.type = kVstMidiType;
    midi.byteSize = sizeof(VstMidiEvent);
    midi.deltaFrames = deltaFrames;
    midi.midiData[0] = static_cast<char>(status);
    midi.midiData[1] = static_cast<char>(note);
    midi.midiData[2] = static_cast<char>(velocity);

    struct OneEventList {
        VstInt32 numEvents;
        VstIntPtr reserved;
        VstEvent* events[2];
    } list{};
    list.numEvents = 1;
    list.events[0] = reinterpret_cast<VstEvent*>(&midi);
    return effect->dispatcher(effect, EffProcessEvents, 0, 0, &list, 0.0f) != 0;
}

bool processSilence(AEffect* effect, VstInt32 frames) {
    std::vector<float> left(static_cast<std::size_t>(frames), -1.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), -1.0f);
    float* outputs[2]{left.data(), right.data()};
    effect->processReplacing(effect, nullptr, outputs, frames);
    for (VstInt32 i = 0; i < frames; ++i) {
        if (std::fabs(left[static_cast<std::size_t>(i)]) > 0.00001f ||
            std::fabs(right[static_cast<std::size_t>(i)]) > 0.00001f)
            return false;
    }
    return true;
}

bool processNoteOnBlock(AEffect* effect, VstInt32 frames,
                        VstInt32 deltaFrames, float expectedAmplitude) {
    std::vector<float> left(static_cast<std::size_t>(frames), -1.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), -1.0f);
    float* outputs[2]{left.data(), right.data()};
    effect->processReplacing(effect, nullptr, outputs, frames);

    for (VstInt32 i = 0; i < frames; ++i) {
        const float expected = i < deltaFrames ? 0.0f : expectedAmplitude;
        if (std::fabs(left[static_cast<std::size_t>(i)] - expected) > 0.00001f ||
            std::fabs(right[static_cast<std::size_t>(i)] - expected) > 0.00001f)
            return false;
    }
    return true;
}

bool processNoteOffBlock(AEffect* effect, VstInt32 frames,
                         VstInt32 deltaFrames, float expectedAmplitude) {
    std::vector<float> left(static_cast<std::size_t>(frames), -1.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), -1.0f);
    float* outputs[2]{left.data(), right.data()};
    effect->processReplacing(effect, nullptr, outputs, frames);

    for (VstInt32 i = 0; i < frames; ++i) {
        const float expected = i < deltaFrames ? expectedAmplitude : 0.0f;
        if (std::fabs(left[static_cast<std::size_t>(i)] - expected) > 0.00001f ||
            std::fabs(right[static_cast<std::size_t>(i)] - expected) > 0.00001f)
            return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5 && argc != 6) return 1;

    const bool sidecarMode = argc == 6 && std::wstring_view(argv[5]) == L"--sidecar";
    if (!sidecarMode) {
        SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86", argv[2]);
        SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2", argv[3]);
        SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST", argv[4]);
        SetEnvironmentVariableW(L"PLUGINSCALER_SCALE_PERCENT", L"100");
        SetEnvironmentVariableW(L"PLUGINSCALER_EDITOR_MODE", L"Direct");
    } else {
        SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86", nullptr);
        SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2", nullptr);
        SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST", nullptr);
        SetEnvironmentVariableW(L"PLUGINSCALER_SCALE_PERCENT", nullptr);
        SetEnvironmentVariableW(L"PLUGINSCALER_EDITOR_MODE", nullptr);
    }

    HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) return 2;
    auto entry = reinterpret_cast<EntryProc>(GetProcAddress(proxy, "VSTPluginMain"));
    if (!entry) {
        FreeLibrary(proxy);
        return 3;
    }

    AEffect* effect = entry(hostCallback);
    if (!effect || effect->magic != kEffectMagic || !effect->dispatcher ||
        !effect->processReplacing) {
        FreeLibrary(proxy);
        return 4;
    }

    bool ok =
        effect->numInputs == 0 &&
        effect->numOutputs == 2 &&
        effect->numParams == 1 &&
        effect->numPrograms == 4 &&
        effect->uniqueId == 0x53594E31 &&
        (effect->flags & (1 << 8)) != 0;
    std::cout << "synth-metadata=" << (ok ? "PASS" : "FAIL") << "\n";

    HWND editorHost = nullptr;
    if (ok) {
        editorHost = createHostWindow();
        VstRect* rect = nullptr;
        const auto rectResult = editorHost
            ? effect->dispatcher(effect, EffEditGetRect, 0, 0, &rect, 0.0f)
            : 0;
        const int rectWidth = rect ? (rect->right - rect->left) : 0;
        const int rectHeight = rect ? (rect->bottom - rect->top) : 0;
        const auto openResult =
            (editorHost && rectResult && rectWidth == 320 && rectHeight == 180)
                ? effect->dispatcher(effect, EffEditOpen, 0, 0, editorHost, 0.0f)
                : 0;

        ShowWindow(editorHost, SW_SHOW);
        UpdateWindow(editorHost);
        Sleep(100);

        const int children = childCount(editorHost);
        HWND surface = FindWindowExW(editorHost, nullptr,
                                     L"125A_MockVST2SynthEditor", nullptr);

        COLORREF pixel = CLR_INVALID;
        if (surface) {
            RedrawWindow(surface, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            HDC dc = GetDC(surface);
            pixel = GetPixel(dc, 50, 40);
            ReleaseDC(surface, dc);
        }

        std::cout << "editor-rect=" << rectWidth << "x" << rectHeight << "\n";
        std::cout << "editor-open-result=" << openResult << "\n";
        std::cout << "editor-children=" << children << "\n";
        std::cout << "editor-surface=" << (surface ? 1 : 0) << "\n";
        std::cout << "editor-pixel=";
        if (pixel == CLR_INVALID) {
            std::cout << "INVALID\n";
        } else {
            std::cout << static_cast<unsigned>(GetRValue(pixel)) << ","
                      << static_cast<unsigned>(GetGValue(pixel)) << ","
                      << static_cast<unsigned>(GetBValue(pixel)) << "\n";
        }

        ok = editorHost != nullptr &&
             rectResult != 0 &&
             rect != nullptr &&
             rectWidth == 320 &&
             rectHeight == 180 &&
             openResult != 0 &&
             children >= 1 &&
             surface != nullptr &&
             pixel != CLR_INVALID &&
             GetRValue(pixel) > 180 &&
             GetGValue(pixel) < 120 &&
             GetBValue(pixel) < 100;

        std::cout << "editor-direct=" << (ok ? "PASS" : "FAIL") << "\n";
        std::cout << "editor-open=" << (ok ? "PASS" : "FAIL") << "\n";

        if (ok && surface) {
            SendMessageW(surface, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 40));
            SendMessageW(surface, WM_LBUTTONUP, 0, MAKELPARAM(50, 40));
            RedrawWindow(surface, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

            HDC dc = GetDC(surface);
            const COLORREF mappedPixel = GetPixel(dc, 50, 40);
            ReleaseDC(surface, dc);

            const bool mappingOk =
                mappedPixel != CLR_INVALID &&
                GetRValue(mappedPixel) < 100 &&
                GetGValue(mappedPixel) > 170 &&
                GetBValue(mappedPixel) < 120;
            std::cout << "editor-native-mouse=" << (mappingOk ? "PASS" : "FAIL") << "\n";
            ok = ok && mappingOk;

            std::uint32_t automationBits =
                gAutomationValueBits.load(std::memory_order_acquire);
            float automationValue = 0.0f;
            std::memcpy(&automationValue, &automationBits, sizeof(automationValue));
            const bool automationOk =
                gAutomationCount.load(std::memory_order_acquire) >= 1 &&
                gAutomationIndex.load(std::memory_order_relaxed) == 0 &&
                std::fabs(automationValue - 0.75f) < 0.00001f;
            std::cout << "editor-automation-callback="
                      << (automationOk ? "PASS" : "FAIL") << "\n";
            ok = ok && automationOk;

            if (ok) {
                SendMessageW(surface, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 40));
                SendMessageW(surface, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(70, 20));
                SendMessageW(surface, WM_LBUTTONUP, 0, MAKELPARAM(70, 20));
                RedrawWindow(surface, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

                HDC dragDc = GetDC(surface);
                const COLORREF dragPixel = GetPixel(dragDc, 140, 100);
                ReleaseDC(surface, dragDc);

                const bool dragOk =
                    dragPixel != CLR_INVALID &&
                    GetRValue(dragPixel) > 180 &&
                    GetGValue(dragPixel) > 170 &&
                    GetBValue(dragPixel) < 100;
                std::cout << "editor-native-drag=" << (dragOk ? "PASS" : "FAIL") << "\n";
                ok = ok && dragOk;
            }
        }
    }

    if (ok) {
        ok = effect->dispatcher(effect, EffEditClose, 0, 0, nullptr, 0.0f) != 0;
        Sleep(50);
        ok = ok && childCount(editorHost) == 0;
        std::cout << "editor-close=" << (ok ? "PASS" : "FAIL") << "\n";
    }
    if (editorHost)
        DestroyWindow(editorHost);

    if (ok)
        ok = effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f) != 0 &&
             effect->dispatcher(effect, EffSetSampleRate, 0, 0, nullptr, 48000.0f) != 0 &&
             effect->dispatcher(effect, EffSetBlockSize, 0, 64, nullptr, 0.0f) != 0 &&
             effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f) != 0;

    if (ok)
        ok = processSilence(effect, 64);

    const float gain = effect ? effect->getParameter(effect, 0) : 0.0f;
    const float expectedAmplitude =
        (60.0f / 127.0f) * (100.0f / 127.0f) * gain;

    if (ok)
        ok = std::fabs(gain - 0.5f) < 0.00001f &&
             sendMidi(effect, 0x90, 60, 100, 7) &&
             processNoteOnBlock(effect, 64, 7, expectedAmplitude);
    std::cout << "synth-note-on=" << (ok ? "PASS" : "FAIL") << "\n";

    if (ok)
        ok = sendMidi(effect, 0x80, 60, 0, 11) &&
             processNoteOffBlock(effect, 64, 11, expectedAmplitude);
    std::cout << "synth-note-off=" << (ok ? "PASS" : "FAIL") << "\n";

    if (effect) {
        effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
    }
    FreeLibrary(proxy);

    std::cout << "synth=PASS" << (ok ? "" : "-FAIL") << "\n";
    return ok ? 0 : 5;
}
