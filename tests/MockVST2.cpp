#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <cstring>

using namespace pluginscaler::formats::vst2abi;

namespace {

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32, VstIntPtr, void* ptr, float) {
    switch (opcode) {
    case EffOpen:
        return 1;
    case EffClose:
        delete effect;
        return 1;
    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A Mock VST2");
        return 1;
    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A");
        return 1;
    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "MockVST2");
        return 1;
    case EffGetVendorVersion:
        return 1000;
    default:
        return 0;
    }
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host || host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f) < 2400)
        return nullptr;

    auto* effect = new AEffect{};
    effect->magic = kEffectMagic;
    effect->dispatcher = dispatch;
    effect->numPrograms = 8;
    effect->numParams = 16;
    effect->numInputs = 2;
    effect->numOutputs = 2;
    effect->uniqueId = 0x31323541; // 125A
    effect->version = 1000;
    return effect;
}
