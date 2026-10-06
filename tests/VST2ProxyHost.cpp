#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {

VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32,
                               VstIntPtr, void*, float) {
    switch (opcode) {
    case AudioMasterVersion:
        return 2400;
    case AudioMasterGetSampleRate:
        return 48000;
    case AudioMasterGetBlockSize:
        return 64;
    case AudioMasterGetVendorVersion:
        return 1000;
    default:
        return 0;
    }
}

bool processAndCheck(AEffect* effect, VstInt32 frames, float seed, float expectedOffset = 0.125f) {
    std::vector<float> inL(static_cast<std::size_t>(frames));
    std::vector<float> inR(static_cast<std::size_t>(frames));
    std::vector<float> outL(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> outR(static_cast<std::size_t>(frames), 0.0f);

    for (VstInt32 i = 0; i < frames; ++i) {
        inL[static_cast<std::size_t>(i)] = seed + static_cast<float>(i + 1) / 100.0f;
        inR[static_cast<std::size_t>(i)] = seed + static_cast<float>((i + 1) * 2) / 100.0f;
    }

    float* inputs[2]{inL.data(), inR.data()};
    float* outputs[2]{outL.data(), outR.data()};
    effect->processReplacing(effect, inputs, outputs, frames);

    for (VstInt32 i = 0; i < frames; ++i) {
        const float expectL = inL[static_cast<std::size_t>(i)] * 2.0f + expectedOffset;
        const float expectR = inR[static_cast<std::size_t>(i)] * 2.0f + expectedOffset;
        if (std::fabs(outL[static_cast<std::size_t>(i)] - expectL) > 0.00001f ||
            std::fabs(outR[static_cast<std::size_t>(i)] - expectR) > 0.00001f)
            return false;
    }

    std::cout << "block=" << frames
              << " seed=" << seed
              << " outL0=" << outL[0]
              << " outRlast=" << outR.back() << "\n";
    return true;
}

bool sendMidi(AEffect* effect, std::uint8_t status, std::uint8_t note,
              std::uint8_t velocity) {
    VstMidiEvent midi{};
    midi.type = kVstMidiType;
    midi.byteSize = sizeof(VstMidiEvent);
    midi.deltaFrames = 7;
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

bool sendNoteOn(AEffect* effect, std::uint8_t note, std::uint8_t velocity) {
    return sendMidi(effect, 0x90u, note, velocity);
}

bool sendNoteOff(AEffect* effect, std::uint8_t note) {
    return sendMidi(effect, 0x80u, note, 0);
}

bool configure(AEffect* effect, float sampleRate, VstInt32 blockSize) {
    return effect->dispatcher(effect, EffSetSampleRate, 0, 0, nullptr, sampleRate) != 0 &&
           effect->dispatcher(effect, EffSetBlockSize, 0, blockSize, nullptr, 0.0f) != 0;
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
    const bool terminated = TerminateProcess(process, 0x125A) != FALSE &&
                            WaitForSingleObject(process, 2000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return terminated;
}

void pumpMessagesFor(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    MSG msg{};
    while (GetTickCount64() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

bool processAndExpectSilence(AEffect* effect, VstInt32 frames,
                             ULONGLONG& elapsedMs) {
    std::vector<float> inL(static_cast<std::size_t>(frames), 0.25f);
    std::vector<float> inR(static_cast<std::size_t>(frames), 0.5f);
    std::vector<float> outL(static_cast<std::size_t>(frames), 1.0f);
    std::vector<float> outR(static_cast<std::size_t>(frames), 1.0f);
    float* inputs[2]{inL.data(), inR.data()};
    float* outputs[2]{outL.data(), outR.data()};

    const ULONGLONG start = GetTickCount64();
    effect->processReplacing(effect, inputs, outputs, frames);
    elapsedMs = GetTickCount64() - start;

    for (VstInt32 i = 0; i < frames; ++i) {
        if (outL[static_cast<std::size_t>(i)] != 0.0f ||
            outR[static_cast<std::size_t>(i)] != 0.0f)
            return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 5) {
        std::cerr << "usage: VST2ProxyHost <proxy.dll> <x86-helper.exe> <x86-plugin.dll> <manifest.txt>\n";
        return 1;
    }

    SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86", argv[2]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2", argv[3]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_MANIFEST", argv[4]);

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

    bool metadataOk =
        effect->numPrograms == 8 &&
        effect->numParams == 16 &&
        effect->numInputs == 2 &&
        effect->numOutputs == 2 &&
        effect->uniqueId == 0x31323541 &&
        effect->version == 1000 &&
        (effect->flags & kEffectFlagIsSynth) != 0 &&
        (effect->flags & kEffectFlagProgramChunks) != 0;
    std::cout << "metadata=" << (metadataOk ? "PASS" : "FAIL") << "\n";
    if (!metadataOk) {
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
        FreeLibrary(proxy);
        return 7;
    }

    effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f);

    if (!configure(effect, 48000.0f, 64) ||
        !effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f)) {
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
        FreeLibrary(proxy);
        return 5;
    }

    bool ok = true;

    // Repeated blocks at the same configuration, using the default parameter
    // value recovered by the x86 pre-scan manifest.
    for (int n = 0; n < 8 && ok; ++n)
        ok = processAndCheck(effect, 64, static_cast<float>(n) * 0.01f);

    // Verify set/getParameter crosses the bridge and changes audio.
    if (ok) {
        effect->setParameter(effect, 0, 0.25f);
        const float cached = effect->getParameter(effect, 0);
        ok = std::fabs(cached - 0.25f) < 0.00001f &&
             processAndCheck(effect, 64, 0.15f, 0.25f);
        std::cout << "parameter=" << (ok ? "PASS" : "FAIL") << "\n";
    }

    // Verify a MIDI Note On crosses x64 -> x86 and combines with the
    // parameter state already applied to the real 32-bit plugin.
    if (ok)
        ok = sendNoteOn(effect, 60, 100) &&
             processAndCheck(effect, 64, 0.20f, 0.310f);

    if (ok) {
        const auto mainsTid = effect->dispatcher(
            effect, EffVendorSpecific, 0x126C, 0, nullptr, 0.0f);
        const auto midiTid = effect->dispatcher(
            effect, EffVendorSpecific, 0x126D, 0, nullptr, 0.0f);
        const auto processTid = effect->dispatcher(
            effect, EffVendorSpecific, 0x126E, 0, nullptr, 0.0f);
        const bool affinity =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x126B, 0, nullptr, 0.0f) == 1;
        std::cout << "realtime-thread-ids mains=" << mainsTid
                  << " midi=" << midiTid
                  << " process=" << processTid << "\n";
        std::cout << "realtime-thread-affinity="
                  << (affinity ? "PASS" : "FAIL") << "\n";
        ok = ok && affinity;
    }

    // Explicitly release the MIDI note. effMainsChanged(false) suspends
    // processing but does not imply that a VST2 instrument forgets note state.
    if (ok)
        ok = sendNoteOff(effect, 60) &&
             processAndCheck(effect, 64, 0.22f, 0.25f);

    // Exercise mains-off -> configuration change -> mains-on without
    // destroying/reloading the bridged plugin instance.
    if (ok)
        ok = effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f) != 0 &&
             configure(effect, 44100.0f, 128) &&
             effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f) != 0 &&
             processAndCheck(effect, 128, 0.25f, 0.25f);


    // Verify wrapper project state restores both preset/program selection and
    // parameter values. This same test is run against native-chunk and
    // no-chunk targets.
    if (ok) {
        ok = effect->dispatcher(effect, EffSetProgram, 0, 3, nullptr, 0.0f) != 0 &&
             effect->dispatcher(effect, EffGetProgram, 0, 0, nullptr, 0.0f) == 3;
    }

    if (ok) {
        void* chunkPtr = nullptr;
        const auto chunkBytes = effect->dispatcher(
            effect, EffGetChunk, 0, 0, &chunkPtr, 0.0f);
        std::vector<std::uint8_t> saved;
        if (chunkBytes > 0 && chunkPtr) {
            const auto* first = static_cast<const std::uint8_t*>(chunkPtr);
            saved.assign(first, first + static_cast<std::size_t>(chunkBytes));
        } else {
            ok = false;
        }

        if (ok) {
            effect->setParameter(effect, 0, 0.75f);
            ok = processAndCheck(effect, 128, 0.40f, 0.75f);
        }

        if (ok) {
            ok = effect->dispatcher(effect, EffSetProgram, 0, 5, nullptr, 0.0f) != 0 &&
                 effect->dispatcher(effect, EffGetProgram, 0, 0, nullptr, 0.0f) == 5 &&
                 std::fabs(effect->getParameter(effect, 0) - 0.75f) < 0.00001f;
        }

        if (ok) {
            ok = effect->dispatcher(effect, EffSetChunk, 0,
                                    static_cast<VstIntPtr>(saved.size()),
                                    saved.data(), 0.0f) != 0 &&
                 effect->dispatcher(effect, EffGetProgram, 0, 0, nullptr, 0.0f) == 3 &&
                 std::fabs(effect->getParameter(effect, 0) - 0.25f) < 0.00001f &&
                 processAndCheck(effect, 128, 0.45f, 0.25f);
        }
        std::cout << "state=" << (ok ? "PASS" : "FAIL") << "\n";
    }

    if (ok) {
        const bool killed = killHelperChild();
        ULONGLONG crashBlockMs = 0;
        const bool silentFast =
            killed && processAndExpectSilence(effect, 128, crashBlockMs) &&
            crashBlockMs < 250;
        std::cout << "helper-crash-detect-ms=" << crashBlockMs << "\n";
        std::cout << "helper-crash-isolation="
                  << (silentFast ? "PASS" : "FAIL") << "\n";
        ok = ok && silentFast;
    }

    if (ok) {
        // Recovery is intentionally owner/UI-thread work. Pump the proxy's
        // message-only callback window before asking the realtime path to
        // process again.
        pumpMessagesFor(250);
        const bool recovered =
            processAndCheck(effect, 128, 0.48f, 0.25f) &&
            effect->dispatcher(effect, EffGetProgram, 0, 0, nullptr, 0.0f) == 3 &&
            std::fabs(effect->getParameter(effect, 0) - 0.25f) < 0.00001f;
        std::cout << "helper-recovery-offthread="
                  << (recovered ? "PASS" : "FAIL") << "\n";
        std::cout << "helper-restart-state="
                  << (recovered ? "PASS" : "FAIL") << "\n";
        ok = ok && recovered;
    }

    if (ok) {
        const bool armed =
            effect->dispatcher(effect, EffVendorSpecific,
                               0x1267, 0, nullptr, 0.0f) == 1;
        ULONGLONG hangBlockMs = 0;
        const bool isolated =
            armed &&
            processAndExpectSilence(effect, 128, hangBlockMs) &&
            hangBlockMs < 250;
        std::cout << "helper-hang-detect-ms=" << hangBlockMs << "\n";
        std::cout << "helper-hang-isolation="
                  << (isolated ? "PASS" : "FAIL") << "\n";
        ok = ok && isolated;
    }

    if (ok) {
        pumpMessagesFor(300);
        const bool hangRecovered =
            processAndCheck(effect, 128, 0.49f, 0.25f) &&
            effect->dispatcher(effect, EffGetProgram, 0, 0, nullptr, 0.0f) == 3 &&
            std::fabs(effect->getParameter(effect, 0) - 0.25f) < 0.00001f;
        std::cout << "helper-hang-recovery="
                  << (hangRecovered ? "PASS" : "FAIL") << "\n";
        ok = ok && hangRecovered;
    }

    if (ok)
        ok = configure(effect, 96000.0f, 32) &&
             processAndCheck(effect, 32, 0.5f, 0.25f);

    // Stop transport/plugin processing and restart the helper path.
    if (ok)
        ok = effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f) != 0;

    if (ok)
        ok = configure(effect, 48000.0f, 64) &&
             effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f) != 0 &&
             processAndCheck(effect, 64, 0.75f, 0.25f);

    effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(proxy);

    std::cout << "torture=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 6;
}
