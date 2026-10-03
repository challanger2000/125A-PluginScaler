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
    effect->dispatcher(effect, EffSetSampleRate, 0, 0, nullptr, 48000.0f);
    effect->dispatcher(effect, EffSetBlockSize, 0, 64, nullptr, 0.0f);

    if (!effect->dispatcher(effect, EffMainsChanged, 0, 1, nullptr, 0.0f)) {
        effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
        FreeLibrary(proxy);
        return 5;
    }

    std::vector<float> inL(64);
    std::vector<float> inR(64);
    std::vector<float> outL(64, 0.0f);
    std::vector<float> outR(64, 0.0f);

    for (int i = 0; i < 64; ++i) {
        inL[static_cast<std::size_t>(i)] = static_cast<float>(i + 1) / 100.0f;
        inR[static_cast<std::size_t>(i)] = static_cast<float>((i + 1) * 2) / 100.0f;
    }

    float* inputs[2]{inL.data(), inR.data()};
    float* outputs[2]{outL.data(), outR.data()};
    effect->processReplacing(effect, inputs, outputs, 64);

    const bool ok =
        std::fabs(outL[0] - 0.02f) < 0.00001f &&
        std::fabs(outR[0] - 0.04f) < 0.00001f &&
        std::fabs(outL[63] - 1.28f) < 0.00001f &&
        std::fabs(outR[63] - 2.56f) < 0.00001f;

    std::cout
        << "outL0=" << outL[0] << "\n"
        << "outR0=" << outR[0] << "\n"
        << "outL63=" << outL[63] << "\n"
        << "outR63=" << outR[63] << "\n";

    effect->dispatcher(effect, EffMainsChanged, 0, 0, nullptr, 0.0f);
    effect->dispatcher(effect, EffClose, 0, 0, nullptr, 0.0f);
    FreeLibrary(proxy);

    return ok ? 0 : 6;
}
