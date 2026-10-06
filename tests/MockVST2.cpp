#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <algorithm>
#include <cstring>
#include <cstddef>
#include <cstdint>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct MockChunkState {
    VstInt32 currentProgram{0};
    float parameters[16]{};
};

struct MockState {
    float sampleRate{0.0f};
    VstInt32 blockSize{0};
    bool mains{false};
    bool midiSeen{false};
    bool hangNextProcess{false};
    std::uint8_t lastMidiNote{0};
    VstInt32 currentProgram{0};
    float parameters[16]{0.125f};
    MockChunkState chunkState{};
};

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index, VstIntPtr value, void* ptr, float opt) {
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
    case EffSetProgram:
        if (state && value >= 0 && value < 8) {
            state->currentProgram = static_cast<VstInt32>(value);
            return 1;
        }
        return 0;
    case EffGetProgram:
        return state ? state->currentProgram : 0;
    case EffGetChunk:
#ifndef PLUGINSCALER_MOCK_NO_CHUNK
        if (state && ptr) {
            state->chunkState.currentProgram = state->currentProgram;
            std::memcpy(state->chunkState.parameters,
                        state->parameters, sizeof(state->parameters));
            *static_cast<void**>(ptr) = &state->chunkState;
            return static_cast<VstIntPtr>(sizeof(state->chunkState));
        }
#endif
        return 0;
    case EffSetChunk:
#ifndef PLUGINSCALER_MOCK_NO_CHUNK
        if (state && ptr &&
            value == static_cast<VstIntPtr>(sizeof(state->chunkState))) {
            std::memcpy(&state->chunkState, ptr, sizeof(state->chunkState));
            state->currentProgram = state->chunkState.currentProgram;
            std::memcpy(state->parameters,
                        state->chunkState.parameters, sizeof(state->parameters));
            return 1;
        }
#endif
        return 0;
    case EffVendorSpecific:
        if (state && index == 0x1267) {
            state->hangNextProcess = true;
            return 1;
        }
        return 0;
    case EffProcessEvents:
        if (state && ptr) {
            auto* events = static_cast<VstEvents*>(ptr);
            auto** eventPtrs = reinterpret_cast<VstEvent**>(
                reinterpret_cast<std::uint8_t*>(events) + offsetof(VstEvents, events));
            for (VstInt32 i = 0; i < events->numEvents; ++i) {
                auto* ev = eventPtrs[i];
                if (!ev || ev->type != kVstMidiType) continue;
                auto* midi = reinterpret_cast<VstMidiEvent*>(ev);
                const auto status = static_cast<std::uint8_t>(midi->midiData[0]) & 0xF0u;
                const auto velocity = static_cast<std::uint8_t>(midi->midiData[2]);
                if (status == 0x90u && velocity > 0) {
                    state->midiSeen = true;
                    state->lastMidiNote = static_cast<std::uint8_t>(midi->midiData[1]);
                } else if (status == 0x80u || (status == 0x90u && velocity == 0)) {
                    const auto note = static_cast<std::uint8_t>(midi->midiData[1]);
                    if (state->midiSeen && state->lastMidiNote == note)
                        state->midiSeen = false;
                }
            }
            return 1;
        }
        return 0;
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
    if (state && state->hangNextProcess) {
        state->hangNextProcess = false;
        Sleep(5000);
    }

    const bool configured =
        state && state->mains && state->sampleRate > 0.0f &&
        state->blockSize == frames;

    for (VstInt32 ch = 0; ch < effect->numOutputs; ++ch) {
        const VstInt32 inputCh = std::min(ch, effect->numInputs - 1);
        for (VstInt32 i = 0; i < frames; ++i) {
            const float in = (configured && inputs && effect->numInputs > 0)
                ? inputs[inputCh][i]
                : 0.0f;
            const float midiOffset =
                (state && state->midiSeen) ? static_cast<float>(state->lastMidiNote) / 1000.0f : 0.0f;
            const float parameterOffset = state ? state->parameters[0] : 0.0f;
#if defined(PLUGINSCALER_MOCK_LEGACY_PROCESS)
            outputs[ch][i] += in * 2.0f + midiOffset + parameterOffset;
#else
            outputs[ch][i] = in * 2.0f + midiOffset + parameterOffset;
#endif
        }
    }
}

void __cdecl setParameter(AEffect* effect, VstInt32 index, float value) {
    auto* state = static_cast<MockState*>(effect ? effect->object : nullptr);
    if (state && index >= 0 && index < 16)
        state->parameters[index] = value;
}

float __cdecl getParameter(AEffect* effect, VstInt32 index) {
    auto* state = static_cast<MockState*>(effect ? effect->object : nullptr);
    if (!state || index < 0 || index >= 16) return 0.0f;
    return state->parameters[index];
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host || host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f) < 2400)
        return nullptr;

    auto* effect = new AEffect{};
    effect->magic = kEffectMagic;
    effect->dispatcher = dispatch;
#if defined(PLUGINSCALER_MOCK_LEGACY_PROCESS)
    effect->process = processReplacing;
    effect->processReplacing = nullptr;
#else
    effect->processReplacing = processReplacing;
#endif
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->numPrograms = 8;
    effect->numParams = 16;
    effect->numInputs = 2;
    effect->numOutputs = 2;
#if defined(PLUGINSCALER_MOCK_LEGACY_PROCESS)
    effect->flags = (1 << 8);
#else
    effect->flags = (1 << 4) | (1 << 8);
#endif
#ifndef PLUGINSCALER_MOCK_NO_CHUNK
    effect->flags |= (1 << 5);
#endif
    effect->object = new MockState{};
    effect->uniqueId = 0x31323541;
    effect->version = 1000;
    return effect;
}
