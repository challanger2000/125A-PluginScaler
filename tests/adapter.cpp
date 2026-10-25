#include "scaler/bridge.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(#x);                                            \
  } while (false)
int wmain(int argc, wchar_t **argv) {
  try {
    CHECK(argc == 2);
    scaler::Bridge bridge;
    auto dir = scaler::moduleDirectory(nullptr);
    CHECK(bridge.open(dir, argv[1], L"scaler-helper64.exe"));
    CHECK(bridge.shared->parameterCount == 1);
    CHECK(bridge.configure(48000));
    CHECK(bridge.activate(true));
    auto *s = bridge.shared;
    for (int i = 0; i < 64; ++i) {
      s->inputs[0][i] = 0.5f;
      s->inputs[1][i] = -0.5f;
    }
    s->eventCount = 3;
    s->events[0] = {scaler::EventType::Midi, 8, 0, 0, {0x90, 60, 127, 0}, 0};
    s->events[1] = {scaler::EventType::Parameter, 16, 0, 1.f, {}, 0};
    s->events[2] = {scaler::EventType::Midi, 32, 0, 0, {0x80, 60, 0, 0}, 0};
    CHECK(bridge.audio(64, false));
    for (int i = 0; i < 64; ++i) {
      float note = i >= 8 && i < 32 ? 0.25f : 0.f;
      float gain = i < 16 ? 0.5f : 1.f;
      CHECK(s->outputs[0][i] == (0.5f + note) * gain);
      CHECK(s->outputs[1][i] == (-0.5f + note) * gain);
    }
    CHECK(bridge.activate(false));
    CHECK(bridge.state(false));
    CHECK(s->stateBytes == 4);
    float saved;
    memcpy(&saved, s->state, 4);
    CHECK(saved == 1.f);
    float restored = 0.25f;
    memcpy(s->state, &restored, 4);
    CHECK(bridge.state(true));
    CHECK(bridge.activate(true));
    s->eventCount = 0;
    CHECK(bridge.audio(64, false));
    for (int i = 0; i < 64; ++i)
      CHECK(s->outputs[0][i] == 0.125f && s->outputs[1][i] == -0.125f);
    std::cout << "PASS helper -> real VST3 ABI synthetic fixture: nonordinal "
                 "parameter ID 42, sample-offset notes/automation, exact "
                 "stereo PCM, state -> audible recall\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
