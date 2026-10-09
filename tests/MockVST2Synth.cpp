#include "pluginscaler/formats/vst2/VST2LegacyABI.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <windowsx.h>

using namespace pluginscaler::formats::vst2abi;

namespace {

bool gMappedClickReceived = false;
bool gNativeDragReceived = false;
bool gDragArmed = false;
AudioMasterCallback gHostCallback = nullptr;
AEffect* gEffectForCallback = nullptr;

LRESULT CALLBACK mockEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (x == 50 && y == 40) {
            gMappedClickReceived = true;
            gDragArmed = true;
            if (gHostCallback && gEffectForCallback)
                (void)gHostCallback(gEffectForCallback, AudioMasterAutomate,
                                    0, 0, nullptr, 0.75f);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 1;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (gDragArmed && (wp & MK_LBUTTON) != 0 &&
            x == 70 && y == 20) {
            gNativeDragReceived = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 1;
    }
    case WM_LBUTTONUP:
        gDragArmed = false;
        return 1;
    case WM_PAINT:
    case WM_PRINT:
    case WM_PRINTCLIENT: {
        HDC dc = nullptr;
        PAINTSTRUCT ps{};
        bool paint = msg == WM_PAINT;
        if (paint)
            dc = BeginPaint(hwnd, &ps);
        else
            dc = reinterpret_cast<HDC>(wp);
        if (dc) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH background = CreateSolidBrush(RGB(24, 96, 208));
            FillRect(dc, &rc, background);
            DeleteObject(background);
            RECT marker{20, 20, 80, 60};
            HBRUSH accent = CreateSolidBrush(
                gMappedClickReceived ? RGB(32, 220, 64) : RGB(240, 64, 32));
            FillRect(dc, &marker, accent);
            DeleteObject(accent);

            RECT dragMarker{120, 80, 180, 120};
            HBRUSH dragBrush = CreateSolidBrush(
                gNativeDragReceived ? RGB(240, 220, 32) : RGB(48, 48, 48));
            FillRect(dc, &dragMarker, dragBrush);
            DeleteObject(dragBrush);
        }
        if (paint)
            EndPaint(hwnd, &ps);
        return 1;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

struct SynthState {
    float sampleRate{0.0f};
    VstInt32 blockSize{0};
    bool mains{false};
    float gain{0.5f};
    VstInt32 currentProgram{0};

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

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index,
                           VstIntPtr value, void* ptr, float opt) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    switch (opcode) {
    case EffOpen:
        return 1;
    case EffClose:
        delete state;
        delete effect;
        gEffectForCallback = nullptr;
        return 1;
    case EffSetProgram:
        if (!state || value < 0 || value >= effect->numPrograms)
            return 0;
        state->currentProgram = static_cast<VstInt32>(value);
        state->gain = 0.25f + 0.1f * static_cast<float>(state->currentProgram);
        if (gHostCallback)
            (void)gHostCallback(effect, AudioMasterAutomate, 0, 0, nullptr,
                                state->gain);
        return 1;
    case EffGetProgram:
        return state ? state->currentProgram : 0;
    case EffSetProgramName:
        return ptr ? 1 : 0;
    case EffGetProgramName:
        if (ptr && state) {
            char name[24]{};
            sprintf_s(name, "Program %d", static_cast<int>(state->currentProgram));
            std::memcpy(ptr, name, sizeof(name));
            return 1;
        }
        return 0;
    case EffGetProgramNameIndexed:
        if (ptr && index >= 0 && index < effect->numPrograms) {
            char name[24]{};
            sprintf_s(name, "Program %d", static_cast<int>(index));
            std::memcpy(ptr, name, sizeof(name));
            return 1;
        }
        return 0;
    case EffGetParamName:
        if (ptr && index == 0) {
            char text[8]{"Gain"};
            std::memcpy(ptr, text, sizeof(text));
            return 1;
        }
        return 0;
    case EffGetParamDisplay:
        if (ptr && state && index == 0) {
            char text[8]{};
            sprintf_s(text, "%.2f", static_cast<double>(state->gain));
            std::memcpy(ptr, text, sizeof(text));
            return 1;
        }
        return 0;
    case EffGetParamLabel:
        if (ptr && index == 0) {
            char text[8]{"lin"};
            std::memcpy(ptr, text, sizeof(text));
            return 1;
        }
        return 0;
    case EffCanBeAutomated:
        return index == 0 ? 1 : 0;
    case EffGetParameterProperties:
        if (ptr && index == 0) {
            auto* props = static_cast<VstParameterProperties*>(ptr);
            *props = {};
            props->smallStepFloat = 0.01f;
            props->largeStepFloat = 0.1f;
            strcpy_s(props->label, sizeof(props->label), "Gain");
            strcpy_s(props->shortLabel, sizeof(props->shortLabel), "Gain");
            return 1;
        }
        return 0;
    case EffBeginSetProgram:
    case EffEndSetProgram:
    case EffStartProcess:
    case EffStopProcess:
        return 1;
    case EffBeginLoadBank:
    case EffBeginLoadProgram:
        if (ptr) {
            const auto* info = static_cast<const VstPatchChunkInfo*>(ptr);
            return info->pluginUniqueID == effect->uniqueId ? 1 : -1;
        }
        return 0;
    case EffSetProcessPrecision:
        return value == 0 ? 1 : 0;
    case EffGetTailSize:
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
                wc.lpfnWndProc = mockEditorProc;
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
    gHostCallback = host;
    gEffectForCallback = effect;
    effect->uniqueId = 0x53594E31; // "SYN1"
    effect->version = 1000;
    return effect;
}
