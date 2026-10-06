#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {

int gIoChangedCount = 0;
int gUpdateDisplayCount = 0;
bool gSawDynamicIo = false;
bool gSawRestoredIo = false;
int gPluginMidiOutputCount = 0;
VstInt32 gPluginMidiDelta = -1;
std::uint8_t gPluginMidiStatus = 0;
std::uint8_t gPluginMidiNote = 0;
std::uint8_t gPluginMidiVelocity = 0;

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

void pumpMessagesFor(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

VstIntPtr __cdecl hostCallback(AEffect* effect, VstInt32 opcode, VstInt32,
                               VstIntPtr, void* ptr, float) {
    switch (opcode) {
    case AudioMasterVersion: return 2400;
    case AudioMasterGetSampleRate: return 48000;
    case AudioMasterGetBlockSize: return 64;
    case AudioMasterGetCurrentProcessLevel: return 4;
    case AudioMasterGetAutomationState: return 4;
    case AudioMasterGetVendorVersion: return 1000;
    case AudioMasterIOChanged:
        ++gIoChangedCount;
        if (effect &&
            effect->numInputs == 0 &&
            effect->numOutputs == 1 &&
            effect->initialDelay == 37)
            gSawDynamicIo = true;
        if (effect &&
            effect->numInputs == 0 &&
            effect->numOutputs == 2 &&
            effect->initialDelay == 0)
            gSawRestoredIo = true;
        return 1;
    case AudioMasterUpdateDisplay:
        ++gUpdateDisplayCount;
        return 1;
    case AudioMasterProcessEvents: {
        auto* events = static_cast<VstEvents*>(ptr);
        if (!events || events->numEvents <= 0)
            return 0;
        auto** eventPtrs = reinterpret_cast<VstEvent**>(
            reinterpret_cast<std::uint8_t*>(events) +
            offsetof(VstEvents, events));
        auto* event = eventPtrs[0];
        if (!event || event->type != kVstMidiType || event->byteSize < 24)
            return 0;
        auto* midi = reinterpret_cast<VstMidiEvent*>(event);
        ++gPluginMidiOutputCount;
        gPluginMidiDelta = midi->deltaFrames;
        gPluginMidiStatus =
            static_cast<std::uint8_t>(midi->midiData[0]);
        gPluginMidiNote =
            static_cast<std::uint8_t>(midi->midiData[1]);
        gPluginMidiVelocity =
            static_cast<std::uint8_t>(midi->midiData[2]);
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

DWORD findHelperChildProcess() {
    const DWORD parentPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == parentPid &&
                _wcsicmp(entry.szExeFile, L"PluginScalerHelper-x86.exe") == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool killHelperChild() {
    const DWORD pid = findHelperChildProcess();
    if (!pid)
        return false;

    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!process)
        return false;

    const bool killed =
        TerminateProcess(process, 0x125A) != FALSE &&
        WaitForSingleObject(process, 2000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return killed;
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

    // Exercise the proxy in the real VST2 lifecycle order. EffOpen creates
    // the host callback marshalling window on this UI thread before any
    // editor/control operation can cause asynchronous plug-in callbacks.
    if (ok)
        ok = effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f) != 0 &&
             effect->dispatcher(effect, EffSetSampleRate, 0, 0, nullptr, 48000.0f) != 0 &&
             effect->dispatcher(effect, EffSetBlockSize, 0, 64, nullptr, 0.0f) != 0 &&
             effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f) != 0;

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
        pumpMessagesFor(200);

        const int children = childCount(editorHost);
        HWND anchor = FindWindowExW(editorHost, nullptr,
                                    L"125A_PluginScaler_ScaledSurface", nullptr);
        HWND forbiddenCrossProcessChild =
            FindWindowExW(editorHost, nullptr,
                          L"125A_MockVST2SynthEditor", nullptr);
        HWND surrogate =
            FindWindowW(L"125A_PluginScaler_EditorSurrogate", nullptr);
        HWND nativeEditor = surrogate
            ? FindWindowExW(surrogate, nullptr,
                            L"125A_MockVST2SynthEditor", nullptr)
            : nullptr;

        DWORD nativePid = 0;
        if (nativeEditor)
            GetWindowThreadProcessId(nativeEditor, &nativePid);

        COLORREF pixel = CLR_INVALID;
        if (nativeEditor) {
            RedrawWindow(nativeEditor, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            HDC dc = GetDC(nativeEditor);
            pixel = GetPixel(dc, 50, 40);
            ReleaseDC(nativeEditor, dc);
        }

        std::cout << "editor-rect=" << rectWidth << "x" << rectHeight << "\n";
        std::cout << "editor-open-result=" << openResult << "\n";
        std::cout << "editor-anchor=" << (anchor ? 1 : 0) << "\n";
        std::cout << "editor-cross-process-child="
                  << (forbiddenCrossProcessChild ? 1 : 0) << "\n";
        std::cout << "editor-native-sidecar=" << (nativeEditor ? 1 : 0) << "\n";

        ok = editorHost != nullptr &&
             rectResult != 0 &&
             rect != nullptr &&
             rectWidth == 320 &&
             rectHeight == 180 &&
             openResult != 0 &&
             children >= 1 &&
             anchor != nullptr &&
             forbiddenCrossProcessChild == nullptr &&
             nativeEditor != nullptr &&
             nativePid != 0 &&
             nativePid != GetCurrentProcessId() &&
             pixel != CLR_INVALID &&
             GetRValue(pixel) > 180 &&
             GetGValue(pixel) < 120 &&
             GetBValue(pixel) < 100;

        std::cout << "editor-no-reparent=" << (ok ? "PASS" : "FAIL") << "\n";
        std::cout << "editor-open=" << (ok ? "PASS" : "FAIL") << "\n";

        if (ok && nativeEditor) {
            SendMessageW(nativeEditor, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(50, 40));
            SendMessageW(nativeEditor, WM_LBUTTONUP, 0,
                         MAKELPARAM(50, 40));
            RedrawWindow(nativeEditor, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

            HDC dc = GetDC(nativeEditor);
            const COLORREF mappedPixel = GetPixel(dc, 50, 40);
            ReleaseDC(nativeEditor, dc);

            const bool mappingOk =
                mappedPixel != CLR_INVALID &&
                GetRValue(mappedPixel) < 100 &&
                GetGValue(mappedPixel) > 170 &&
                GetBValue(mappedPixel) < 120;
            std::cout << "editor-native-mouse="
                      << (mappingOk ? "PASS" : "FAIL") << "\n";
            ok = ok && mappingOk;

            if (ok) {
                SendMessageW(nativeEditor, WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(50, 40));
                SendMessageW(nativeEditor, WM_MOUSEMOVE, MK_LBUTTON,
                             MAKELPARAM(70, 20));
                SendMessageW(nativeEditor, WM_LBUTTONUP, 0,
                             MAKELPARAM(70, 20));
                RedrawWindow(nativeEditor, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

                HDC dragDc = GetDC(nativeEditor);
                const COLORREF dragPixel = GetPixel(dragDc, 140, 100);
                ReleaseDC(nativeEditor, dragDc);

                const bool dragOk =
                    dragPixel != CLR_INVALID &&
                    GetRValue(dragPixel) > 180 &&
                    GetGValue(dragPixel) > 170 &&
                    GetBValue(dragPixel) < 100;
                std::cout << "editor-native-drag="
                          << (dragOk ? "PASS" : "FAIL") << "\n";
                ok = ok && dragOk;
            }
        }
    }

    if (ok) {
        const bool killed = killHelperChild();
        const bool silentCrashBlock = killed && processSilence(effect, 64);
        pumpMessagesFor(150);

        HWND staleAnchor = FindWindowExW(
            editorHost, nullptr, L"125A_PluginScaler_ScaledSurface", nullptr);
        HWND staleNative =
            FindWindowW(L"125A_MockVST2SynthEditor", nullptr);

        const bool cleanupOk =
            silentCrashBlock &&
            staleAnchor == nullptr &&
            staleNative == nullptr;
        std::cout << "editor-crash-cleanup="
                  << (cleanupOk ? "PASS" : "FAIL") << "\n";
        ok = ok && cleanupOk;
    }

    if (ok) {
        VstRect* reopenedRect = nullptr;
        const auto rectResult =
            effect->dispatcher(effect, EffEditGetRect, 0, 0,
                               &reopenedRect, 0.0f);
        const auto reopenResult =
            (rectResult && reopenedRect)
                ? effect->dispatcher(effect, EffEditOpen, 0, 0,
                                     editorHost, 0.0f)
                : 0;
        pumpMessagesFor(200);

        HWND reopenedAnchor = FindWindowExW(
            editorHost, nullptr, L"125A_PluginScaler_ScaledSurface", nullptr);
        HWND reopenedSurrogate =
            FindWindowW(L"125A_PluginScaler_EditorSurrogate", nullptr);
        HWND reopenedNative = reopenedSurrogate
            ? FindWindowExW(reopenedSurrogate, nullptr,
                            L"125A_MockVST2SynthEditor", nullptr)
            : nullptr;

        const bool reopenOk =
            reopenResult != 0 &&
            reopenedAnchor != nullptr &&
            reopenedSurrogate != nullptr &&
            reopenedNative != nullptr &&
            childCount(editorHost) >= 1;
        std::cout << "editor-crash-reopen="
                  << (reopenOk ? "PASS" : "FAIL") << "\n";
        ok = ok && reopenOk;
    }

    if (ok) {
        ok = effect->dispatcher(effect, EffEditClose, 0, 0, nullptr, 0.0f) != 0;
        pumpMessagesFor(100);
        HWND remainingSurrogate =
            FindWindowW(L"125A_PluginScaler_EditorSurrogate", nullptr);
        HWND remainingNative = remainingSurrogate
            ? FindWindowExW(remainingSurrogate, nullptr,
                            L"125A_MockVST2SynthEditor", nullptr)
            : nullptr;
        ok = ok && childCount(editorHost) == 0 &&
             remainingNative == nullptr;
        std::cout << "editor-close=" << (ok ? "PASS" : "FAIL") << "\n";
    }
    if (editorHost)
        DestroyWindow(editorHost);

    if (ok)
        ok = processSilence(effect, 64);

    if (ok) {
        const bool runtimeContextOk =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1266, 0, nullptr, 0.0f) == 1;
        std::cout << "host-runtime-context="
                  << (runtimeContextOk ? "PASS" : "FAIL") << "\n";
        ok = ok && runtimeContextOk;
    }

    if (ok) {
        pumpMessagesFor(120);
        const bool legacyIdleOk =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1261, 0, nullptr, 0.0f) == 1;
        std::cout << "legacy-idle-proxy="
                  << (legacyIdleOk ? "PASS" : "FAIL") << "\n";
        ok = ok && legacyIdleOk;
    }

    if (ok) {
        const bool capabilitiesOk =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1264, 0, nullptr, 0.0f) == 1;
        std::cout << "host-capabilities="
                  << (capabilitiesOk ? "PASS" : "FAIL") << "\n";
        ok = ok && capabilitiesOk;
    }

    if (ok) {
        const auto callbackTrigger =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1262, 0, nullptr, 0.0f);
        pumpMessagesFor(120);
        const bool callbacksOk =
            callbackTrigger != 0 &&
            gIoChangedCount >= 1 &&
            gUpdateDisplayCount == 1;
        std::cout << "host-callback-trigger=" << callbackTrigger << "\n";
        std::cout << "host-callback-counts="
                  << gIoChangedCount << "," << gUpdateDisplayCount << "\n";
        std::cout << "host-callback-forwarding="
                  << (callbacksOk ? "PASS" : "FAIL") << "\n";
        ok = ok && callbacksOk;
    }

    if (ok) {
        const auto dynamicIoTrigger =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1265, 0, nullptr, 0.0f);
        pumpMessagesFor(120);
        const bool dynamicIoOk =
            dynamicIoTrigger == 1 &&
            gSawDynamicIo &&
            gSawRestoredIo &&
            effect->numInputs == 0 &&
            effect->numOutputs == 2 &&
            effect->initialDelay == 0;
        std::cout << "dynamic-io-change="
                  << (dynamicIoOk ? "PASS" : "FAIL") << "\n";
        ok = ok && dynamicIoOk;
    }

    if (ok)
        ok = processSilence(effect, 64);

    if (ok) {
        VstIntPtr slowDispatchResult = 0;
        std::thread slowOwnerCall([&] {
            slowDispatchResult =
                effect->dispatcher(effect, EffVendorSpecific,
                                   0x1263, 0, nullptr, 0.0f);
        });

        Sleep(120);
        const ULONGLONG audioStart = GetTickCount64();
        const bool silentDuringOwnerWork = processSilence(effect, 64);
        const ULONGLONG audioElapsedMs = GetTickCount64() - audioStart;
        slowOwnerCall.join();

        const bool rtContentionOk =
            slowDispatchResult == 1 &&
            silentDuringOwnerWork &&
            audioElapsedMs < 400;
        std::cout << "rt-contention-ms=" << audioElapsedMs << "\n";
        std::cout << "rt-contention-nonblocking="
                  << (rtContentionOk ? "PASS" : "FAIL") << "\n";
        ok = ok && rtContentionOk;
    }

    const float gain = effect ? effect->getParameter(effect, 0) : 0.0f;
    const bool automationOk = std::fabs(gain - 0.75f) < 0.00001f;
    std::cout << "editor-automation="
              << (automationOk ? "PASS" : "FAIL") << "\n";
    ok = ok && automationOk;

    const float expectedAmplitude =
        (60.0f / 127.0f) * (100.0f / 127.0f) * gain;

    if (ok)
        ok = sendMidi(effect, 0x90, 60, 100, 7) &&
             processNoteOnBlock(effect, 64, 7, expectedAmplitude);
    std::cout << "synth-note-on=" << (ok ? "PASS" : "FAIL") << "\n";

    const bool midiOutNoteOnOk =
        gPluginMidiOutputCount == 1 &&
        gPluginMidiDelta == 7 &&
        (gPluginMidiStatus & 0xF0u) == 0x90u &&
        gPluginMidiNote == 60 &&
        gPluginMidiVelocity == 100;
    std::cout << "plugin-midi-out-note-on="
              << (midiOutNoteOnOk ? "PASS" : "FAIL") << "\n";
    ok = ok && midiOutNoteOnOk;

    if (ok)
        ok = sendMidi(effect, 0x80, 60, 0, 11) &&
             processNoteOffBlock(effect, 64, 11, expectedAmplitude);
    std::cout << "synth-note-off=" << (ok ? "PASS" : "FAIL") << "\n";

    const bool midiOutNoteOffOk =
        gPluginMidiOutputCount == 2 &&
        gPluginMidiDelta == 11 &&
        (gPluginMidiStatus & 0xF0u) == 0x80u &&
        gPluginMidiNote == 60 &&
        gPluginMidiVelocity == 0;
    std::cout << "plugin-midi-out-note-off="
              << (midiOutNoteOffOk ? "PASS" : "FAIL") << "\n";
    ok = ok && midiOutNoteOffOk;

    if (effect) {
        effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
    }
    FreeLibrary(proxy);

    std::cout << "synth=PASS" << (ok ? "" : "-FAIL") << "\n";
    return ok ? 0 : 5;
}
