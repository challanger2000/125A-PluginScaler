#pragma once

// Minimal independently maintained VST2 interoperability ABI used only for
// loading/hosting legacy binaries. No Steinberg VST2 SDK headers are shipped.

#include <cstdint>

namespace pluginscaler::formats::vst2abi {

using VstInt32 = std::int32_t;
using VstIntPtr = std::intptr_t;

struct AEffect;

#pragma pack(push, 8)
struct VstRect {
    std::int16_t top;
    std::int16_t left;
    std::int16_t bottom;
    std::int16_t right;
};
#pragma pack(pop)

#pragma pack(push, 8)
struct VstEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    char data[16];
};

struct VstMidiEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstEvents {
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent* events[2];
};

struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    VstInt32 timeSigNumerator;
    VstInt32 timeSigDenominator;
    VstInt32 smpteOffset;
    VstInt32 smpteFrameRate;
    VstInt32 samplesToNextClock;
    VstInt32 flags;
};
#pragma pack(pop)

inline constexpr VstInt32 kVstMidiType = 1;

enum VstTimeInfoFlags : VstInt32 {
    VstTransportChanged = 1 << 0,
    VstTransportPlaying = 1 << 1,
    VstTransportCycleActive = 1 << 2,
    VstTransportRecording = 1 << 3,
    VstAutomationWriting = 1 << 6,
    VstAutomationReading = 1 << 7,
    VstNanosValid = 1 << 8,
    VstPpqPosValid = 1 << 9,
    VstTempoValid = 1 << 10,
    VstBarsValid = 1 << 11,
    VstCyclePosValid = 1 << 12,
    VstTimeSigValid = 1 << 13,
    VstSmpteValid = 1 << 14,
    VstClockValid = 1 << 15
};

using AudioMasterCallback = VstIntPtr (__cdecl *)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
using DispatcherProc = VstIntPtr (__cdecl *)(AEffect*, VstInt32, VstInt32, VstIntPtr, void*, float);
using ProcessProc = void (__cdecl *)(AEffect*, float**, float**, VstInt32);
using SetParameterProc = void (__cdecl *)(AEffect*, VstInt32, float);
using GetParameterProc = float (__cdecl *)(AEffect*, VstInt32);
using ProcessDoubleProc = void (__cdecl *)(AEffect*, double**, double**, VstInt32);
using EntryProc = AEffect* (__cdecl *)(AudioMasterCallback);

#pragma pack(push, 8)
struct AEffect {
    VstInt32 magic;
    DispatcherProc dispatcher;
    ProcessProc process;
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;
    VstIntPtr reserved1;
    VstIntPtr reserved2;
    VstInt32 initialDelay;
    VstInt32 realQualities;
    VstInt32 offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueId;
    VstInt32 version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};
#pragma pack(pop)

inline constexpr VstInt32 kEffectMagic = 0x56737450; // VstP

enum DispatcherOpcode : VstInt32 {
    EffOpen = 0,
    EffClose = 1,
    EffGetProgram = 3,
    EffSetSampleRate = 10,
    EffSetBlockSize = 11,
    EffMainsChanged = 12,
    EffEditGetRect = 13,
    EffEditOpen = 14,
    EffEditClose = 15,
    EffEditIdle = 19,
    EffGetChunk = 23,
    EffSetChunk = 24,
    EffProcessEvents = 25,
    EffGetPlugCategory = 35,
    EffGetEffectName = 45,
    EffGetVendorString = 47,
    EffGetProductString = 48,
    EffGetVendorVersion = 49,
    EffCanDo = 51,
    EffGetVstVersion = 58,
    EffGetNumMidiInputChannels = 78,
    EffGetNumMidiOutputChannels = 79
};

enum VstPlugCategory : VstInt32 {
    PlugCategUnknown = 0,
    PlugCategEffect = 1,
    PlugCategSynth = 2
};

inline constexpr VstInt32 kEffectFlagHasEditor = 1 << 0;
inline constexpr VstInt32 kEffectFlagCanReplacing = 1 << 4;
inline constexpr VstInt32 kEffectFlagProgramChunks = 1 << 5;
inline constexpr VstInt32 kEffectFlagIsSynth = 1 << 8;

enum AudioMasterOpcode : VstInt32 {
    AudioMasterAutomate = 0,
    AudioMasterVersion = 1,
    AudioMasterGetTime = 7,
    AudioMasterProcessEvents = 8,
    AudioMasterGetSampleRate = 16,
    AudioMasterGetBlockSize = 17,
    AudioMasterGetVendorString = 32,
    AudioMasterGetProductString = 33,
    AudioMasterGetVendorVersion = 34,
    AudioMasterCanDo = 37
};

} // namespace pluginscaler::formats::vst2abi
