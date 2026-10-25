// Independent synthetic VST3 fixture; intentionally no GUI, external assets or
// SDK helpers.
#include "scaler/stream.hpp"
#include <algorithm>
#include <cmath>
using namespace scaler;
namespace {
constexpr TUID fixtureId =
    INLINE_UID(0x125A7003, 0xA111B222, 0xC333D444, 0xE555F666);
class Fixture final : public IComponent,
                      public IAudioProcessor,
                      public IEditController {
  SCALER_REFS
  std::atomic<float> gain_{0.5f};
  float note_ = 0;
  bool active_ = false, processing_ = false;

public:
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IComponent);
    SCALER_QUERY(IAudioProcessor);
    SCALER_QUERY(IEditController);
    if (same(iid, INLINE_UID_OF(FUnknown)) ||
        same(iid, INLINE_UID_OF(IPluginBase))) {
      *obj = static_cast<IComponent *>(this);
      addRef();
      return kResultOk;
    }
    return kNoInterface;
  }
  tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
  tresult PLUGIN_API terminate() override { return kResultOk; }
  tresult PLUGIN_API getControllerClassId(TUID) override {
    return kResultFalse;
  }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType t, BusDirection d) override {
    return t == kAudio ? 1 : (d == kInput ? 1 : 0);
  }
  tresult PLUGIN_API getBusInfo(MediaType t, BusDirection d, int32 i,
                                BusInfo &b) override {
    if (i || !getBusCount(t, d))
      return kInvalidArgument;
    b = {};
    b.mediaType = t;
    b.direction = d;
    b.channelCount = t == kAudio ? 2 : 16;
    b.busType = kMain;
    b.flags = BusInfo::kDefaultActive;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override {
    return kNotImplemented;
  }
  tresult PLUGIN_API activateBus(MediaType, BusDirection, int32,
                                 TBool) override {
    return kResultOk;
  }
  tresult PLUGIN_API setActive(TBool v) override {
    active_ = v;
    if (!v)
      note_ = 0;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream *stream) override {
    float v;
    int32 n;
    if (!stream || stream->read(&v, 4, &n) != kResultOk || n != 4 ||
        !std::isfinite(v) || v < 0 || v > 1)
      return kResultFalse;
    gain_ = v;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream *stream) override {
    float v = gain_;
    int32 n;
    return stream && stream->write(&v, 4, &n) == kResultOk && n == 4
               ? kResultOk
               : kResultFalse;
  }
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement *in, int32 ni,
                                        SpeakerArrangement *out,
                                        int32 no) override {
    return ni == 1 && no == 1 && in[0] == SpeakerArr::kStereo &&
                   out[0] == SpeakerArr::kStereo
               ? kResultOk
               : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection, int32 i,
                                       SpeakerArrangement &a) override {
    if (i)
      return kInvalidArgument;
    a = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override {
    return s == kSample32 ? kResultTrue : kResultFalse;
  }
  uint32 PLUGIN_API getLatencySamples() override { return 0; }
  uint32 PLUGIN_API getTailSamples() override { return 0; }
  tresult PLUGIN_API setupProcessing(ProcessSetup &s) override {
    return s.symbolicSampleSize == kSample32 && s.sampleRate >= 8000 &&
                   s.maxSamplesPerBlock <= 2048
               ? kResultOk
               : kResultFalse;
  }
  tresult PLUGIN_API setProcessing(TBool v) override {
    if (v && !active_)
      return kResultFalse;
    processing_ = v;
    return kResultOk;
  }
  tresult PLUGIN_API process(ProcessData &d) override {
    if (!active_ || !processing_)
      return kResultFalse;
    for (int sample = 0; sample < std::max(1, d.numSamples); ++sample) {
      if (d.inputParameterChanges)
        for (int q = 0; q < d.inputParameterChanges->getParameterCount(); ++q) {
          auto *queue = d.inputParameterChanges->getParameterData(q);
          if (queue->getParameterId() != 42)
            continue;
          for (int p = 0; p < queue->getPointCount(); ++p) {
            int32 offset;
            ParamValue value;
            if (queue->getPoint(p, offset, value) == kResultOk &&
                offset == sample)
              gain_ = float(value);
          }
        }
      if (d.inputEvents)
        for (int i = 0; i < d.inputEvents->getEventCount(); ++i) {
          Steinberg::Vst::Event event{};
          if (d.inputEvents->getEvent(i, event) != kResultOk ||
              event.sampleOffset != sample)
            continue;
          if (event.type == Steinberg::Vst::Event::kNoteOnEvent)
            note_ = event.noteOn.velocity * 0.25f;
          else if (event.type == Steinberg::Vst::Event::kNoteOffEvent)
            note_ = 0;
        }
      if (sample < d.numSamples)
        for (int c = 0; c < 2; ++c)
          d.outputs[0].channelBuffers32[c][sample] =
              (d.inputs[0].channelBuffers32[c][sample] + note_) * gain_;
    }
    return kResultOk;
  }
  tresult PLUGIN_API setComponentState(IBStream *) override {
    return kResultOk;
  }
  int32 PLUGIN_API getParameterCount() override { return 1; }
  tresult PLUGIN_API getParameterInfo(int32 i, ParameterInfo &p) override {
    if (i)
      return kInvalidArgument;
    p = {};
    p.id = 42;
    text16(p.title, "Fixture Gain");
    p.defaultNormalizedValue = 0.5;
    p.flags = ParameterInfo::kCanAutomate;
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue,
                                           String128) override {
    return kNotImplemented;
  }
  tresult PLUGIN_API getParamValueByString(ParamID, TChar *,
                                           ParamValue &) override {
    return kNotImplemented;
  }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override {
    return v;
  }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override {
    return v;
  }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
    return id == 42 ? double(gain_) : 0;
  }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override {
    if (id != 42)
      return kInvalidArgument;
    gain_ = float(v);
    return kResultOk;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler *) override {
    return kResultOk;
  }
  IPlugView *PLUGIN_API createView(FIDString) override { return nullptr; }
};
class Factory final : public IPluginFactory {
  SCALER_REFS
public:
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IPluginFactory);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo *p) override {
    if (!p)
      return kInvalidArgument;
    *p = {};
    strcpy(p->vendor, "125A synthetic test fixture");
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 1; }
  tresult PLUGIN_API getClassInfo(int32 i, PClassInfo *p) override {
    if (i || !p)
      return kInvalidArgument;
    *p = {};
    memcpy(p->cid, fixtureId, 16);
    p->cardinality = PClassInfo::kManyInstances;
    strcpy(p->category, kVstAudioEffectClass);
    strcpy(p->name, "Synthetic Fixture VST3");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid,
                                    void **obj) override {
    if (!same(cid, fixtureId))
      return kInvalidArgument;
    auto *f = new Fixture;
    auto result = f->queryInterface(iid, obj);
    f->release();
    return result;
  }
};
} // namespace
extern "C" __declspec(dllexport) IPluginFactory *PLUGIN_API GetPluginFactory() {
  return new Factory;
}
extern "C" __declspec(dllexport) bool InitDll() { return true; }
extern "C" __declspec(dllexport) bool ExitDll() { return true; }
