#pragma once
// Minimal, ABI-compatible declarations for VST 2.x editor hosting.
// Deliberately no Steinberg VST2 SDK or redistribution of third-party source.
#include <cstdint>
#include <cstddef>

namespace scaler::vst2 {
struct AEffect;
using Int = std::int32_t;
using Ptr = std::intptr_t;
using Host = Ptr (__cdecl *)(AEffect*, Int, Int, Ptr, void*, float);
using Dispatcher = Ptr (__cdecl *)(AEffect*, Int, Int, Ptr, void*, float);
using Process = void (__cdecl *)(AEffect*, float**, float**, Int);
using SetParameter = void (__cdecl *)(AEffect*, Int, float);
using GetParameter = float (__cdecl *)(AEffect*, Int);

struct AEffect {
    Int magic{};
    Dispatcher dispatcher{};
    Process process{};
    SetParameter setParameter{};
    GetParameter getParameter{};
    Int numPrograms{};
    Int numParams{};
    Int numInputs{};
    Int numOutputs{};
    Int flags{};
    Ptr resvd1{};
    Ptr resvd2{};
    Int initialDelay{};
    Int realQualities{};
    Int offQualities{};
    float ioRatio{};
    void* object{};
    void* user{};
    Int uniqueID{};
    Int version{};
    Process processReplacing{};
    void* processDoubleReplacing{};
    char future[56]{};
};
struct ERect {
    std::int16_t top, left, bottom, right;
};
constexpr Int kMagic = 0x56737450;
constexpr Int kHasEditor = 1;
constexpr Int kEffOpen = 0;
constexpr Int kEffClose = 1;
constexpr Int kEffSetSampleRate = 10;
constexpr Int kEffSetBlockSize = 11;
constexpr Int kEffEditGetRect = 13;
constexpr Int kEffEditOpen = 14;
constexpr Int kEffEditClose = 15;
constexpr Int kEffEditIdle = 19;
constexpr Int kHostVersion = 1;
constexpr Int kHostGetSampleRate = 16;
constexpr Int kHostGetBlockSize = 17;
using Entry = AEffect* (__cdecl *)(Host);
static_assert(sizeof(AEffect) >= 192, "Unexpected x64 VST2 ABI");
}  // namespace scaler::vst2
