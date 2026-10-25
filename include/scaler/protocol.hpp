#pragma once
#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace scaler {
inline constexpr uint32_t protocolVersion = 1, maxFrames = 2048,
                          maxEvents = 256, maxParams = 128;
inline constexpr uint32_t maxWidth = 1024, maxHeight = 768,
                          maxState = 1024 * 1024;
enum class Op : uint32_t {
  None,
  Configure,
  Audio,
  GetState,
  SetState,
  Activate,
  Stop
};
enum class EventType : uint32_t { Midi, Parameter };
#pragma pack(push, 8)
struct Event {
  EventType type;
  uint32_t offset, index;
  float value;
  uint8_t midi[4];
  uint32_t reserved;
};
struct Input {
  uint32_t message, keys;
  int32_t x, y;
};
struct Shared {
  uint32_t version, bytes;
  volatile LONG ready, result, operation, stopping;
  double sampleRate;
  uint32_t frames, eventCount, offline, parameterCount, latency;
  uint8_t identity[16];
  Event events[maxEvents];
  float inputs[2][maxFrames], outputs[2][maxFrames];
  volatile LONG parameterBits[maxParams];
  volatile LONG callbackGeneration[maxParams];
  char parameterNames[maxParams][64];
  uint32_t stateBytes;
  uint8_t state[maxState];
  volatile LONG editorWanted, editorOpen, width, height, frameLock,
      frameGeneration;
  volatile LONG inputWrite, inputRead, inputDrops;
  Input input[512];
  uint32_t pixels[maxWidth * maxHeight];
};
#pragma pack(pop)
static_assert(sizeof(Event) == 24 && sizeof(Input) == 16);
static_assert(offsetof(Shared, sampleRate) == 24);
static_assert(sizeof(Shared) == 4250736,
              "wire layout must be identical in x86/x64");
inline float paramRead(Shared *s, uint32_t i) {
  LONG bits = InterlockedCompareExchange(&s->parameterBits[i], 0, 0);
  float v;
  memcpy(&v, &bits, 4);
  return v;
}
inline void paramWrite(Shared *s, uint32_t i, float v) {
  LONG bits;
  memcpy(&bits, &v, 4);
  InterlockedExchange(&s->parameterBits[i], bits);
}
} // namespace scaler
