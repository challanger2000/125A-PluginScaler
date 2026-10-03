#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <windows.h>

#include <cmath>
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

bool processAndCheck(AEffect* effect, VstInt32 frames, float seed) {
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
        const float expectL = inL[static_cast<std::size_t>(i)] * 2.0f;
        const float expectR = inR[static_cast<std::size_t>(i)] * 2.0f;
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

bool configure(AEffect* effect, float sampleRate, VstInt32 blockSize) {
    return effect->dispatcher(effect, EffSetSampleRate, 0, 0, nullptr, sampleRate) != 0 &&
           effect->dispatcher(effect, EffSetBlockSize, 0, blockSize, nullptr, 0.0f) != 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::cerr << "usage: VST2ProxyHost <proxy.dll> <x86-helper.exe> <x86-plugin.dll>\n";
        return 1;
    }

    SetEnvironmentVariableW(L"PLUGINSCALER_HELPER_X86", argv[2]);
    SetEnvironmentVariableW(L"PLUGINSCALER_TARGET_VST2", argv[3]);

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

    effect->dispatcher(effect, EffOpen, 0, 0, nullptr, 0.0f);

    if (!configure(effect, 48000.0f, 64) ||
        !effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f)) {
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
        FreeLibrary(proxy);
        return 5;
    }

    bool ok = true;

    // Repeated blocks at the same configuration.
    for (int n = 0; n < 8 && ok; ++n)
        ok = processAndCheck(effect, 64, static_cast<float>(n) * 0.01f);

    // Change both sample rate and block size while active.
    if (ok)
        ok = configure(effect, 44100.0f, 128) &&
             processAndCheck(effect, 128, 0.25f);

    if (ok)
        ok = configure(effect, 96000.0f, 32) &&
             processAndCheck(effect, 32, 0.5f);

    // Stop transport/plugin processing and restart the helper path.
    if (ok)
        ok = effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f) != 0;

    if (ok)
        ok = configure(effect, 48000.0f, 64) &&
             effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f) != 0 &&
             processAndCheck(effect, 64, 0.75f);

    effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(proxy);

    std::cout << "torture=" << (ok ? "PASS" : "FAIL") << "\n";
    return ok ? 0 : 6;
}
