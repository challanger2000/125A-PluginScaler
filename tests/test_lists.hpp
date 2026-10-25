#pragma once
#include "scaler/vst3.hpp"
#include <vector>
namespace test {
using namespace scaler;
struct Queue : IParamValueQueue {
  ParamID id = 0;
  std::vector<std::pair<int32, ParamValue>> points;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID, void **obj) override {
    *obj = nullptr;
    return kNoInterface;
  }
  ParamID PLUGIN_API getParameterId() override { return id; }
  int32 PLUGIN_API getPointCount() override { return int32(points.size()); }
  tresult PLUGIN_API getPoint(int32 i, int32 &offset, ParamValue &v) override {
    if (i < 0 || i >= int32(points.size()))
      return kInvalidArgument;
    offset = points[i].first;
    v = points[i].second;
    return kResultOk;
  }
  tresult PLUGIN_API addPoint(int32 offset, ParamValue v, int32 &i) override {
    i = int32(points.size());
    points.emplace_back(offset, v);
    return kResultOk;
  }
};
struct Changes : IParameterChanges {
  Queue q;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID, void **obj) override {
    *obj = nullptr;
    return kNoInterface;
  }
  int32 PLUGIN_API getParameterCount() override { return 1; }
  IParamValueQueue *PLUGIN_API getParameterData(int32 i) override {
    return i == 0 ? &q : nullptr;
  }
  IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id,
                                                int32 &i) override {
    q.id = id;
    i = 0;
    return &q;
  }
};
struct Events : IEventList {
  std::vector<Steinberg::Vst::Event> events;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID, void **obj) override {
    *obj = nullptr;
    return kNoInterface;
  }
  int32 PLUGIN_API getEventCount() override { return int32(events.size()); }
  tresult PLUGIN_API getEvent(int32 i, Steinberg::Vst::Event &out) override {
    if (i < 0 || i >= int32(events.size()))
      return kInvalidArgument;
    out = events[i];
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Steinberg::Vst::Event &e) override {
    events.push_back(e);
    return kResultOk;
  }
  void note(int offset, bool on) {
    Steinberg::Vst::Event e{};
    e.sampleOffset = offset;
    e.type = on ? Steinberg::Vst::Event::kNoteOnEvent
                : Steinberg::Vst::Event::kNoteOffEvent;
    if (on) {
      e.noteOn.pitch = 60;
      e.noteOn.velocity = 1;
      e.noteOn.noteId = -1;
    } else {
      e.noteOff.pitch = 60;
      e.noteOff.noteId = -1;
    }
    events.push_back(e);
  }
};
} // namespace test
