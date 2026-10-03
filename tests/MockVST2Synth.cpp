#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>

using namespace pluginscaler::formats::vst2abi;

namespace {

struct SynthState {
    float sampleRate{0.0f};
    VstInt32 blockSize{0};
    bool mains{false};
    float gain{0.5f};

    bool pendingEvent{false};
    bool pendingNoteOn{false};
    VstInt32 pendingDelta{0};
    std::uint8_t pendingNote{60};
    std::uint8_t pendingVelocity{0};

    bool active{false};
    std::uint8_t note{60};
    std::uint8_t velocity{0};
    HWND editorWindow{nullptr};
    VstRect editorRect{0, 0, 180, 320};
};

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32,
                           VstIntPtr value, void* ptr, float opt) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
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
    case EffEditGetRect:
        if (state && ptr) {
            *static_cast<VstRect**>(ptr) = &state->editorRect;
            return 1;
        }
        return 0;
    case EffEditOpen:
        if (state && ptr) {
            static const wchar_t* kClassName = L"125A_MockVST2SynthEditor";
            static ATOM atom = 0;
            if (!atom) {
                WNDCLASSW wc{};
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = kClassName;
                atom = RegisterClassW(&wc);
                if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return 0;
            }
            HWND parent = static_cast<HWND>(ptr);
            state->editorWindow = CreateWindowExW(
                0, kClassName, L"125A Mock Synth Editor",
                WS_CHILD | WS_VISIBLE,
                0, 0, 320, 180,
                parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            return state->editorWindow ? 1 : 0;
        }
        return 0;
    case EffEditClose:
        if (state && state->editorWindow) {
            DestroyWindow(state->editorWindow);
            state->editorWindow = nullptr;
        }
        return 1;
    case EffMainsChanged:
        if (state) {
            state->mains = value != 0;
            if (!state->mains) {
                state->active = false;
                state->pendingEvent = false;
            }
        }
        return 1;
    case EffProcessEvents:
        if (!state || !ptr) return 0;
        {
            auto* events = static_cast<VstEvents*>(ptr);
            auto** eventPtrs = reinterpret_cast<VstEvent**>(
                reinterpret_cast<std::uint8_t*>(events) + offsetof(VstEvents, events));
            for (VstInt32 i = 0; i < events->numEvents; ++i) {
                auto* ev = eventPtrs[i];
                if (!ev || ev->type != kVstMidiType) continue;
                auto* midi = reinterpret_cast<VstMidiEvent*>(ev);
                const auto status = static_cast<std::uint8_t>(midi->midiData[0]) & 0xF0u;
                const auto note = static_cast<std::uint8_t>(midi->midiData[1]);
                const auto velocity = static_cast<std::uint8_t>(midi->midiData[2]);
                if (status == 0x90u && velocity > 0) {
                    state->pendingEvent = true;
                    state->pendingNoteOn = true;
                    state->pendingDelta = midi->deltaFrames;
                    state->pendingNote = note;
                    state->pendingVelocity = velocity;
                } else if (status == 0x80u || (status == 0x90u && velocity == 0)) {
                    state->pendingEvent = true;
                    state->pendingNoteOn = false;
                    state->pendingDelta = midi->deltaFrames;
                    state->pendingNote = note;
                    state->pendingVelocity = 0;
                }
            }
        }
        return 1;
    case EffGetChunk:
        if (state && ptr) {
            *static_cast<void**>(ptr) = &state->gain;
            return static_cast<VstIntPtr>(sizeof(state->gain));
        }
        return 0;
    case EffSetChunk:
        if (state && ptr && value == static_cast<VstIntPtr>(sizeof(state->gain))) {
            std::memcpy(&state->gain, ptr, sizeof(state->gain));
            state->gain = std::clamp(state->gain, 0.0f, 1.0f);
            return 1;
        }
        return 0;
    case EffGetEffectName:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A Mock Synth");
        return 1;
    case EffGetVendorString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "125A");
        return 1;
    case EffGetProductString:
        if (ptr) strcpy_s(static_cast<char*>(ptr), 256, "MockVST2Synth");
        return 1;
    case EffGetVendorVersion:
        return 1000;
    default:
        return 0;
    }
}

void __cdecl setParameter(AEffect* effect, VstInt32 index, float value) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    if (state && index == 0)
        state->gain = std::clamp(value, 0.0f, 1.0f);
}

float __cdecl getParameter(AEffect* effect, VstInt32 index) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    return (state && index == 0) ? state->gain : 0.0f;
}

void __cdecl processReplacing(AEffect* effect, float**, float** outputs, VstInt32 frames) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    if (!state || !state->mains || !outputs || frames <= 0) {
        if (outputs && effect) {
            for (VstInt32 ch = 0; ch < effect->numOutputs; ++ch)
                if (outputs[ch]) std::fill(outputs[ch], outputs[ch] + std::max<VstInt32>(frames, 0), 0.0f);
        }
        return;
    }

    const VstInt32 eventFrame = state->pendingEvent
        ? std::clamp<VstInt32>(state->pendingDelta, 0, frames)
        : frames;

    for (VstInt32 i = 0; i < frames; ++i) {
        if (state->pendingEvent && i == eventFrame) {
            if (state->pendingNoteOn) {
                state->active = true;
                state->note = state->pendingNote;
                state->velocity = state->pendingVelocity;
            } else if (state->active && state->note == state->pendingNote) {
                state->active = false;
                state->velocity = 0;
            }
        }

        const float amplitude = state->active
            ? (static_cast<float>(state->note) / 127.0f) *
              (static_cast<float>(state->velocity) / 127.0f) *
              state->gain
            : 0.0f;

        for (VstInt32 ch = 0; ch < effect->numOutputs; ++ch)
            if (outputs[ch]) outputs[ch][i] = amplitude;
    }

    if (state->pendingEvent) {
        if (eventFrame == frames) {
            if (state->pendingNoteOn) {
                state->active = true;
                state->note = state->pendingNote;
                state->velocity = state->pendingVelocity;
            } else if (state->active && state->note == state->pendingNote) {
                state->active = false;
                state->velocity = 0;
            }
        }
        state->pendingEvent = false;
    }
}

} // namespace

extern "C" __declspec(dllexport) AEffect* __cdecl VSTPluginMain(AudioMasterCallback host) {
    if (!host || host(nullptr, AudioMasterVersion, 0, 0, nullptr, 0.0f) < 2400)
        return nullptr;

    auto* effect = new AEffect{};
    effect->magic = kEffectMagic;
    effect->dispatcher = dispatch;
    effect->setParameter = setParameter;
    effect->getParameter = getParameter;
    effect->processReplacing = processReplacing;
    effect->numPrograms = 4;
    effect->numParams = 1;
    effect->numInputs = 0;
    effect->numOutputs = 2;
    effect->flags = (1 << 4) | (1 << 5) | (1 << 8); // replacing, chunks, synth
    effect->object = new SynthState{};
    effect->uniqueId = 0x53594E31; // "SYN1"
    effect->version = 1000;
    return effect;
}
