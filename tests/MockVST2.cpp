#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <algorithm>
#include <cstring>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct MockState {
    float sampleRate{0.0f};
    VstInt32 blockSize{0};
    bool mains{false};
};

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32, VstIntPtr value, void* ptr, float opt) {
    auto* state = static_cast<MockState*>(effect ? effect->object : nullptr);

    switch (opcode) {
    case EffOpen:
        return 1;
    case EffClose:
        delete state;
        delete effect;
        return 1;
    case EffSetSampleRate:
        if (state) state->sampleRate = opt;
        return 1;
    case EffSetBlockSize:
        if (state) state->blockSize = static_cast<VstInt32>(value);
        return 1;
    case EffMainsChanged:
        if (state) state->mains = value != 0;
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

void __cdecl processReplacing(AEffect* effect, float** inputs, float** outputs, VstInt32 frames) {
    auto* state = static_cast<MockState*>(effect ? effect->object : nullptr);
    const bool configured =
        state && state->mains && state->sampleRate == 48000.0f && state->blockSize == 64;

    for (VstInt32 ch = 0; ch < effect->numOutputs; ++ch) {
        const VstInt32 inputCh = std::min(ch, effect->numInputs - 1);
        for (VstInt32 i = 0; i < frames; ++i) {
            const float in = (configured && inputs && effect->numInputs > 0)
                ? inputs[inputCh][i]
                : 0.0f;
            outputs[ch][i] = in * 2.0f;
        }
    }
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host || host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f) < 2400)
        return nullptr;

    auto* effect = new AEffect{};
    effect->magic = kEffectMagic;
    effect->dispatcher = dispatch;
    effect->processReplacing = processReplacing;
    effect->numPrograms = 8;
    effect->numParams = 16;
    effect->numInputs = 2;
    effect->numOutputs = 2;
    effect->object = new MockState{};
    effect->uniqueId = 0x31323541;
    effect->version = 1000;
    return effect;
}
