#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace pluginscaler::formats::vst2abi;

namespace {

VstIntPtr __cdecl hostCallback(AEffect*, VstInt32 opcode, VstInt32,
                               VstIntPtr, void*, float) {
    switch (opcode) {
    case AudioMasterVersion: return 2400;
    case AudioMasterGetSampleRate: return 48000;
    case AudioMasterGetBlockSize: return 64;
    case AudioMasterGetVendorVersion: return 1000;
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
    if (argc != 5) return 1;

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

    bool ok =
        effect->numInputs == 0 &&
        effect->numOutputs == 2 &&
        effect->numParams == 1 &&
        effect->numPrograms == 4 &&
        effect->uniqueId == 0x53594E31 &&
        (effect->flags & (1 << 8)) != 0;
    std::cout << "synth-metadata=" << (ok ? "PASS" : "FAIL") << "\n";

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
