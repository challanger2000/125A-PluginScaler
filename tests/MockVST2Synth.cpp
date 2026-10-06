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
bool gTimeInfoVerified = false;
bool gMidiProcessLevelVerified = false;
bool gParameterProcessLevelVerified = false;
VstIntPtr gObservedProcessLevel = 0;
VstIntPtr gObservedAutomationState = 0;
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
            if (gEffectForCallback && gEffectForCallback->setParameter)
                gEffectForCallback->setParameter(gEffectForCallback, 0, 0.75f);
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
        if (gDragArmed && (wp & MK_LBUTTON) != 0) {
            if (x == 70 && y == 20)
                gNativeDragReceived = true;

            if (gEffectForCallback && gEffectForCallback->setParameter &&
                gHostCallback) {
                const float value = std::clamp(
                    0.20f + static_cast<float>(120 - y) / 200.0f,
                    0.0f, 1.0f);
                gEffectForCallback->setParameter(
                    gEffectForCallback, 0, value);
                (void)gHostCallback(
                    gEffectForCallback, AudioMasterAutomate,
                    0, 0, nullptr, value);
                (void)gHostCallback(
                    gEffectForCallback, AudioMasterUpdateDisplay,
                    0, 0, nullptr, 0.0f);
            }
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
    bool resizeRequested{false};
    int beginSetProgramCount{0};
    int endSetProgramCount{0};
    int beginLoadBankCount{0};
    int beginLoadProgramCount{0};
    int mainsOnCount{0};
    int mainsOffCount{0};
    int startProcessCount{0};
    int stopProcessCount{0};
    int legacyIdleCount{0};
    int setParameterCalls{0};
    bool editorParentReadyAtOpen{false};
};

VstIntPtr __cdecl dispatch(AEffect* effect, VstInt32 opcode, VstInt32 index,
                           VstIntPtr value, void* ptr, float opt) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    switch (opcode) {
    case EffOpen:
        if (gHostCallback)
            (void)gHostCallback(
                effect, AudioMasterNeedIdle, 0, 0, nullptr, 0.0f);
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
        if (state) ++state->beginSetProgramCount;
        return 1;
    case EffEndSetProgram:
        if (state) ++state->endSetProgramCount;
        return 1;
    case EffStartProcess:
        if (state) ++state->startProcessCount;
        return 1;
    case EffStopProcess:
        if (state) ++state->stopProcessCount;
        return 1;
    case EffBeginLoadBank:
    case EffBeginLoadProgram:
        if (ptr && state) {
            const auto* info = static_cast<const VstPatchChunkInfo*>(ptr);
            if (info->pluginUniqueID != effect->uniqueId)
                return -1;
            if (opcode == EffBeginLoadBank)
                ++state->beginLoadBankCount;
            else
                ++state->beginLoadProgramCount;
            return 1;
        }
        return 0;
    case EffSetProcessPrecision:
        return (value == 0 || value == 1) ? 1 : 0;
    case EffGetTailSize:
        return 1;
    case EffIdle:
        if (!state)
            return 0;
        ++state->legacyIdleCount;
        return state->legacyIdleCount < 3 ? 1 : 0;
    case EffVendorSpecific:
        if (index == 0x125A)
            return gTimeInfoVerified ? 1 : 0;
        if (index == 0x1260)
            return gObservedProcessLevel == 2 ? 1 : 0;
        if (index == 0x1266)
            return gObservedProcessLevel == 4 &&
                   gObservedAutomationState == 4 ? 1 : 0;
        if (index == 0x1268)
            return gMidiProcessLevelVerified &&
                   gParameterProcessLevelVerified ? 1 : 0;
        if (index == 0x1269)
            return state && state->editorParentReadyAtOpen ? 1 : 0;
        if (index == 0x126A)
            return state ? state->setParameterCalls : -1;
        if (index == 0x1261)
            return state && state->legacyIdleCount == 3 ? 1 : 0;
        if (index == 0x1262) {
            if (!gHostCallback)
                return 0;
            const auto ioChanged = gHostCallback(
                effect, AudioMasterIOChanged, 0, 0, nullptr, 0.0f);
            const auto updateDisplay = gHostCallback(
                effect, AudioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
            return (ioChanged != 0 && updateDisplay != 0) ? 1 : 0;
        }
        if (index == 0x1265) {
            if (!gHostCallback || !effect)
                return 0;

            effect->numOutputs = 1;
            effect->initialDelay = 37;
            const auto changed = gHostCallback(
                effect, AudioMasterIOChanged, 0, 0, nullptr, 0.0f);

            effect->numOutputs = 2;
            effect->initialDelay = 0;
            const auto restored = gHostCallback(
                effect, AudioMasterIOChanged, 0, 0, nullptr, 0.0f);

            return changed != 0 && restored != 0 ? 1 : 0;
        }
        if (index == 0x1264) {
            if (!gHostCallback)
                return 0;
            char sendEvents[] = "sendVstEvents";
            char sendMidi[] = "sendVstMidiEvent";
            char recvEvents[] = "receiveVstEvents";
            char recvMidi[] = "receiveVstMidiEvent";
            char sizeWindow[] = "sizeWindow";
            char unknown[] = "125AUnknownCapability";
            return gHostCallback(effect, AudioMasterCanDo, 0, 0, sendEvents, 0.0f) == 0 &&
                   gHostCallback(effect, AudioMasterCanDo, 0, 0, sendMidi, 0.0f) == 1 &&
                   gHostCallback(effect, AudioMasterCanDo, 0, 0, recvEvents, 0.0f) == 0 &&
                   gHostCallback(effect, AudioMasterCanDo, 0, 0, recvMidi, 0.0f) == 1 &&
                   gHostCallback(effect, AudioMasterCanDo, 0, 0, sizeWindow, 0.0f) == 1 &&
                   gHostCallback(effect, AudioMasterCanDo, 0, 0, unknown, 0.0f) == 0
                ? 1 : 0;
        }
        if (index == 0x1263) {
            Sleep(800);
            return 1;
        }
        if (!state)
            return 0;
        if (index == 0x125B)
            return state->beginSetProgramCount == 1 &&
                   state->endSetProgramCount == 1 &&
                   state->currentProgram == 2
                ? 1 : 0;
        if (index == 0x125C)
            return state->beginLoadBankCount == 1 &&
                   state->beginSetProgramCount == 2 &&
                   state->endSetProgramCount == 2
                ? 1 : 0;
        if (index == 0x125D)
            return state->beginLoadProgramCount == 1 &&
                   state->beginSetProgramCount == 3 &&
                   state->endSetProgramCount == 3
                ? 1 : 0;
        if (index == 0x125E)
            return state->mainsOnCount == 1 &&
                   state->startProcessCount == 1 &&
                   state->mainsOffCount == 0 &&
                   state->stopProcessCount == 0
                ? 1 : 0;
        if (index == 0x125F)
            return state->mainsOnCount == 2 &&
                   state->startProcessCount == 2 &&
                   state->mainsOffCount == 1 &&
                   state->stopProcessCount == 1
                ? 1 : 0;
        return 0;
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
            RECT parentClient{};
            state->editorParentReadyAtOpen =
                IsWindowVisible(parent) != FALSE &&
                GetClientRect(parent, &parentClient) != FALSE &&
                (parentClient.right - parentClient.left) >= 320 &&
                (parentClient.bottom - parentClient.top) >= 180;

            state->editorWindow = CreateWindowExW(
                0, kClassName, L"125A Mock Synth Editor",
                WS_CHILD | WS_VISIBLE,
                0, 0, 320, 180,
                parent, nullptr, GetModuleHandleW(nullptr), nullptr);

            // Legacy regression: editor exists although effEditOpen returns 0.
            return 0;
        }
        return 0;
    case EffEditIdle:
        if (state && state->editorWindow && !state->resizeRequested &&
            gHostCallback) {
            state->resizeRequested = true;
            SetWindowPos(state->editorWindow, nullptr, 0, 0, 400, 220,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return gHostCallback(effect, AudioMasterSizeWindow,
                                 400, 220, nullptr, 0.0f);
        }
        return 1;
    case EffEditClose:
        if (state && state->editorWindow) {
            DestroyWindow(state->editorWindow);
            state->editorWindow = nullptr;
        }
        return 1;
    case EffMainsChanged:
        if (state) {
            state->mains = value != 0;
            if (state->mains)
                ++state->mainsOnCount;
            else
                ++state->mainsOffCount;
            if (!state->mains) {
                state->active = false;
                state->pendingEvent = false;
            }
        }
        return 1;
    case EffProcessEvents:
        if (!state || !ptr) return 0;
        if (gHostCallback && effect) {
            gMidiProcessLevelVerified =
                gHostCallback(effect, AudioMasterGetCurrentProcessLevel,
                              0, 0, nullptr, 0.0f) == 4;
        }
        {
            auto* events = static_cast<VstEvents*>(ptr);
            auto** eventPtrs = reinterpret_cast<VstEvent**>(
                reinterpret_cast<std::uint8_t*>(events) + offsetof(VstEvents, events));
            for (VstInt32 i = 0; i < events->numEvents; ++i) {
                auto* ev = eventPtrs[i];
                if (!ev || ev->type != kVstMidiType) continue;
                if (ev->byteSize != static_cast<VstInt32>(sizeof(VstMidiEvent)))
                    return 0;
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
    case EffGetVstVersion:
        return 2400;
    case EffGetNumMidiInputChannels:
        return 1;
    case EffGetNumMidiOutputChannels:
        return 1;
    case EffCanDo:
        if (!ptr)
            return 0;
        if (std::strcmp(static_cast<const char*>(ptr), "receiveVstMidiEvent") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "sendVstMidiEvent") == 0)
            return 1;
        if (std::strcmp(static_cast<const char*>(ptr), "receiveVstEvents") == 0 ||
            std::strcmp(static_cast<const char*>(ptr), "sendVstEvents") == 0)
            return 0;
        return 0;
    default:
        return 0;
    }
}

void __cdecl setParameter(AEffect* effect, VstInt32 index, float value) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    if (gHostCallback && effect) {
        gParameterProcessLevelVerified =
            gHostCallback(effect, AudioMasterGetCurrentProcessLevel,
                          0, 0, nullptr, 0.0f) == 4;
    }
    if (state && index == 0) {
        ++state->setParameterCalls;
        state->gain = std::clamp(value, 0.0f, 1.0f);
    }
}

float __cdecl getParameter(AEffect* effect, VstInt32 index) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);
    return (state && index == 0) ? state->gain : 0.0f;
}

void __cdecl processReplacing(AEffect* effect, float**, float** outputs, VstInt32 frames) {
    auto* state = static_cast<SynthState*>(effect ? effect->object : nullptr);

    if (gHostCallback && effect) {
        gObservedProcessLevel =
            gHostCallback(effect, AudioMasterGetCurrentProcessLevel,
                          0, 0, nullptr, 0.0f);
        gObservedAutomationState =
            gHostCallback(effect, AudioMasterGetAutomationState,
                          0, 0, nullptr, 0.0f);

        const VstIntPtr requested =
            VstPpqPosValid | VstTempoValid | VstSmpteValid;
        auto* info = reinterpret_cast<VstTimeInfo*>(
            gHostCallback(effect, AudioMasterGetTime, 0,
                          requested, nullptr, 0.0f));
        if (info) {
            constexpr VstInt32 validityMask =
                VstNanosValid | VstPpqPosValid | VstTempoValid |
                VstBarsValid | VstCyclePosValid | VstTimeSigValid |
                VstSmpteValid | VstClockValid;
            const auto returnedValidity = info->flags & validityMask;
            gTimeInfoVerified =
                returnedValidity == (VstPpqPosValid | VstTempoValid) &&
                (info->flags & VstTransportPlaying) != 0 &&
                info->ppqPos == 12.5 &&
                info->tempo == 123.0;
        }
    }
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

    if (state->pendingEvent && gHostCallback && effect) {
        VstMidiEvent outgoing{};
        outgoing.type = kVstMidiType;
        outgoing.byteSize = 24;
        outgoing.deltaFrames = state->pendingDelta;
        outgoing.midiData[0] = static_cast<char>(
            state->pendingNoteOn ? 0x90u : 0x80u);
        outgoing.midiData[1] = static_cast<char>(state->pendingNote);
        outgoing.midiData[2] = static_cast<char>(
            state->pendingNoteOn ? state->pendingVelocity : 0);

        struct OneEventList {
            VstInt32 numEvents;
            VstIntPtr reserved;
            VstEvent* events[2];
        } list{};
        list.numEvents = 1;
        list.events[0] = reinterpret_cast<VstEvent*>(&outgoing);
        (void)gHostCallback(
            effect, AudioMasterProcessEvents, 0, 0, &list, 0.0f);
    }

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
    effect->flags =
        (1 << 4) | (1 << 5) | (1 << 8) | (1 << 12);
    // replacing, chunks, synth, and intentionally advertised double replacing
    effect->object = new SynthState{};
    gHostCallback = host;
    gEffectForCallback = effect;
    effect->uniqueId = 0x53594E31; // "SYN1"
    effect->version = 1000;
    return effect;
}
