#include "pluginterfaces/vst/ivstmessage.h"
#include "scaler/adapter.hpp"
#include "scaler/stream.hpp"
#include "scaler/win32.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace scaler {
class Host final : public IHostApplication {
public:
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IHostApplication);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API getName(String128 name) override {
    text16(*reinterpret_cast<String128 *>(name), "125A PluginScaler");
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(TUID, TUID, void **obj) override {
    if (obj)
      *obj = nullptr;
    return kNotImplemented;
  }
};
class Events final : public IEventList {
public:
  Steinberg::Vst::Event events[maxEvents]{};
  int32 count = 0;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IEventList);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  int32 PLUGIN_API getEventCount() override { return count; }
  tresult PLUGIN_API getEvent(int32 i, Steinberg::Vst::Event &e) override {
    if (i < 0 || i >= count)
      return kInvalidArgument;
    e = events[i];
    return kResultOk;
  }
  tresult PLUGIN_API addEvent(Steinberg::Vst::Event &e) override {
    if (count == int(maxEvents))
      return kResultFalse;
    events[count++] = e;
    return kResultOk;
  }
};
class Queue final : public IParamValueQueue {
public:
  ParamID id = 0;
  int32 count = 0;
  int32 offsets[maxEvents]{};
  ParamValue values[maxEvents]{};
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IParamValueQueue);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  ParamID PLUGIN_API getParameterId() override { return id; }
  int32 PLUGIN_API getPointCount() override { return count; }
  tresult PLUGIN_API getPoint(int32 i, int32 &offset, ParamValue &v) override {
    if (i < 0 || i >= count)
      return kInvalidArgument;
    offset = offsets[i];
    v = values[i];
    return kResultOk;
  }
  tresult PLUGIN_API addPoint(int32 offset, ParamValue v,
                              int32 &index) override {
    if (count == int(maxEvents))
      return kResultFalse;
    index = count;
    offsets[count] = offset;
    values[count++] = v;
    return kResultOk;
  }
};
class Changes final : public IParameterChanges {
public:
  Queue queues[maxParams];
  int32 count = 0;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IParameterChanges);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  int32 PLUGIN_API getParameterCount() override { return count; }
  IParamValueQueue *PLUGIN_API getParameterData(int32 i) override {
    return i >= 0 && i < count ? &queues[i] : nullptr;
  }
  IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id,
                                                int32 &index) override {
    for (int32 i = 0; i < count; ++i)
      if (queues[i].id == id) {
        index = i;
        return &queues[i];
      }
    if (count == int(maxParams))
      return nullptr;
    index = count++;
    queues[index].id = id;
    queues[index].count = 0;
    return &queues[index];
  }
};
class Vst3 final : public Adapter {
  struct Handler final : IComponentHandler {
    Vst3 &owner;
    explicit Handler(Vst3 &o) : owner(o) {}
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
      if (!obj)
        return kInvalidArgument;
      *obj = nullptr;
      SCALER_QUERY(IComponentHandler);
      SCALER_QUERY(FUnknown);
      return kNoInterface;
    }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override {
      return kNotImplemented;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
      if (!std::isfinite(value) || value < 0 || value > 1)
        return kInvalidArgument;
      for (uint32_t i = 0; i < owner.s_->parameterCount; ++i)
        if (owner.ids_[i] == id) {
          paramWrite(owner.s_, i, float(value));
          InterlockedExchange(&owner.guiDirty_[i], 1);
          InterlockedIncrement(&owner.s_->callbackGeneration[i]);
          return kResultOk;
        }
      return kInvalidArgument;
    }
  } handler_{*this};
  HMODULE module_ = nullptr;
  IPluginFactory *factory_ = nullptr;
  IComponent *component_ = nullptr;
  IAudioProcessor *processor_ = nullptr;
  IEditController *controller_ = nullptr;
  IPlugView *view_ = nullptr;
  IConnectionPoint *componentConnection_ = nullptr,
                   *controllerConnection_ = nullptr;
  volatile LONG guiDirty_[maxParams]{}, controllerDirty_[maxParams]{};
  bool separateController_ = false, active_ = false;
  Shared *s_;
  Host host_;
  Events events_;
  Changes changes_;
  ParamID ids_[maxParams]{};
  int inputChannels_ = 0;

public:
  Vst3(const std::wstring &path, Shared *s) : s_(s) {
    module_ = LoadLibraryW(path.c_str());
    if (!module_)
      throw std::runtime_error("cannot load VST3 binary");
    auto entry = symbol<bool (*)()>(module_, "InitDll");
    if (entry && !entry())
      throw std::runtime_error("VST3 InitDll failed");
    auto get =
        symbol<IPluginFactory *(PLUGIN_API *)()>(module_, "GetPluginFactory");
    if (!get || !(factory_ = get()))
      throw std::runtime_error("no VST3 factory");
    for (int32 i = 0; i < factory_->countClasses(); ++i) {
      PClassInfo info{};
      if (factory_->getClassInfo(i, &info) != kResultOk ||
          strcmp(info.category, kVstAudioEffectClass))
        continue;
      if (factory_->createInstance(info.cid, INLINE_UID_OF(IComponent),
                                   reinterpret_cast<void **>(&component_)) ==
          kResultOk) {
        memcpy(s_->identity, info.cid, 16);
        break;
      }
    }
    if (!component_ || component_->initialize(&host_) != kResultOk ||
        component_->queryInterface(INLINE_UID_OF(IAudioProcessor),
                                   reinterpret_cast<void **>(&processor_)) !=
            kResultOk)
      throw std::runtime_error("VST3 initialization/processor failed");
    component_->queryInterface(INLINE_UID_OF(IEditController),
                               reinterpret_cast<void **>(&controller_));
    if (!controller_) {
      TUID cid{};
      if (component_->getControllerClassId(cid) == kResultOk &&
          factory_->createInstance(cid, INLINE_UID_OF(IEditController),
                                   reinterpret_cast<void **>(&controller_)) ==
              kResultOk) {
        separateController_ = true;
        if (controller_->initialize(&host_) != kResultOk)
          throw std::runtime_error("VST3 controller initialization failed");
      }
    }
    if (!controller_)
      throw std::runtime_error("VST3 controller required by prototype");
    controller_->setComponentHandler(&handler_);
    if (separateController_) {
      component_->queryInterface(
          INLINE_UID_OF(IConnectionPoint),
          reinterpret_cast<void **>(&componentConnection_));
      controller_->queryInterface(
          INLINE_UID_OF(IConnectionPoint),
          reinterpret_cast<void **>(&controllerConnection_));
      if (bool(componentConnection_) != bool(controllerConnection_))
        throw std::runtime_error("incomplete VST3 connection points");
      if (componentConnection_ &&
          (componentConnection_->connect(controllerConnection_) != kResultOk ||
           controllerConnection_->connect(componentConnection_) != kResultOk))
        throw std::runtime_error("VST3 connection failed");
    }
    if (component_->getBusCount(kAudio, kOutput) != 1 ||
        component_->getBusCount(kAudio, kInput) > 1 ||
        processor_->canProcessSampleSize(kSample32) != kResultTrue)
      throw std::runtime_error("unsupported VST3 topology/precision");
    BusInfo info{};
    component_->getBusInfo(kAudio, kOutput, 0, info);
    if (info.channelCount != 2)
      throw std::runtime_error("stereo VST3 output required");
    if (component_->getBusCount(kAudio, kInput)) {
      component_->getBusInfo(kAudio, kInput, 0, info);
      inputChannels_ = info.channelCount;
      if (inputChannels_ != 2)
        throw std::runtime_error("stereo VST3 input required");
    }
    int32 n = controller_->getParameterCount();
    if (n < 0 || n > int(maxParams))
      throw std::runtime_error("VST3 parameter limit exceeded");
    s_->parameterCount = n;
    for (int32 i = 0; i < n; ++i) {
      ParameterInfo p{};
      if (controller_->getParameterInfo(i, p) != kResultOk)
        throw std::runtime_error("VST3 parameter enumeration failed");
      ids_[i] = p.id;
      for (size_t c = 0; c < 63 && p.title[c]; ++c)
        s_->parameterNames[i][c] = p.title[c] < 128 ? char(p.title[c]) : '?';
      paramWrite(s_, i, float(controller_->getParamNormalized(p.id)));
    }
  }
  ~Vst3() override {
    closeEditor();
    if (processor_)
      processor_->setProcessing(false);
    if (component_ && active_)
      component_->setActive(false);
    if (controller_) {
      controller_->setComponentHandler(nullptr);
      if (componentConnection_ && controllerConnection_) {
        componentConnection_->disconnect(controllerConnection_);
        controllerConnection_->disconnect(componentConnection_);
      }
      if (componentConnection_)
        componentConnection_->release();
      if (controllerConnection_)
        controllerConnection_->release();
      if (separateController_)
        controller_->terminate();
      controller_->release();
    }
    if (processor_)
      processor_->release();
    if (component_) {
      component_->terminate();
      component_->release();
    }
    if (factory_)
      factory_->release();
    if (module_) {
      auto exit = symbol<bool (*)()>(module_, "ExitDll");
      if (exit)
        exit();
      FreeLibrary(module_);
    }
  }
  void configure(double rate) override {
    if (active_)
      activate(false);
    ProcessSetup setup{kRealtime, kSample32, int32(maxFrames), rate};
    if (processor_->setupProcessing(setup) != kResultOk)
      throw std::runtime_error("VST3 setupProcessing failed");
    component_->activateBus(kAudio, kOutput, 0, true);
    if (inputChannels_)
      component_->activateBus(kAudio, kInput, 0, true);
    if (component_->getBusCount(kEvent, kInput))
      component_->activateBus(kEvent, kInput, 0, true);
    s_->latency = processor_->getLatencySamples();
  }
  void activate(bool state) override {
    if (!state)
      processor_->setProcessing(false);
    if (component_->setActive(state) != kResultOk ||
        (state && processor_->setProcessing(true) != kResultOk))
      throw std::runtime_error("VST3 activation failed");
    active_ = state;
    s_->latency = processor_->getLatencySamples();
  }
  void process() override {
    events_.count = 0;
    changes_.count = 0;
    for (uint32_t i = 0; i < s_->parameterCount; ++i)
      if (InterlockedExchange(&guiDirty_[i], 0)) {
        int32 qi, pi;
        auto *q = changes_.addParameterData(ids_[i], qi);
        if (q)
          q->addPoint(0, paramRead(s_, i), pi);
      }
    for (uint32_t i = 0; i < s_->eventCount; ++i) {
      const auto &in = s_->events[i];
      if (in.type == EventType::Parameter && in.index < s_->parameterCount) {
        int32 qi, pi;
        auto *q = changes_.addParameterData(ids_[in.index], qi);
        if (q)
          q->addPoint(in.offset, in.value, pi);
        paramWrite(s_, in.index, in.value);
        InterlockedExchange(&controllerDirty_[in.index], 1);
      } else if (in.type == EventType::Midi) {
        Steinberg::Vst::Event e{};
        e.sampleOffset = in.offset;
        e.busIndex = 0;
        int status = in.midi[0] & 0xf0, channel = in.midi[0] & 15;
        if (status == 0x90 && in.midi[2]) {
          e.type = Steinberg::Vst::Event::kNoteOnEvent;
          e.noteOn.channel = int16(channel);
          e.noteOn.pitch = in.midi[1];
          e.noteOn.velocity = in.midi[2] / 127.f;
          e.noteOn.noteId = -1;
        } else if (status == 0x80 || status == 0x90) {
          e.type = Steinberg::Vst::Event::kNoteOffEvent;
          e.noteOff.channel = int16(channel);
          e.noteOff.pitch = in.midi[1];
          e.noteOff.velocity = in.midi[2] / 127.f;
          e.noteOff.noteId = -1;
        } else
          continue;
        events_.addEvent(e);
      }
    }
    float *ins[]{s_->inputs[0], s_->inputs[1]},
        *outs[]{s_->outputs[0], s_->outputs[1]};
    AudioBusBuffers in{}, out{};
    in.numChannels = inputChannels_;
    in.channelBuffers32 = ins;
    out.numChannels = 2;
    out.channelBuffers32 = outs;
    ProcessData data;
    data.numSamples = s_->frames;
    data.numInputs = inputChannels_ ? 1 : 0;
    data.inputs = &in;
    data.numOutputs = 1;
    data.outputs = &out;
    data.inputEvents = &events_;
    data.inputParameterChanges = &changes_;
    if (processor_->process(data) != kResultOk)
      throw std::runtime_error("VST3 processing failed");
  }
  bool getState() override {
    Stream stream;
    if (component_->getState(&stream) != kResultOk ||
        stream.bytes.size() > maxState)
      return false;
    s_->stateBytes = uint32_t(stream.bytes.size());
    memcpy(s_->state, stream.bytes.data(), stream.bytes.size());
    return true;
  }
  bool setState() override {
    Stream stream;
    stream.bytes.assign(s_->state, s_->state + s_->stateBytes);
    if (component_->setState(&stream) != kResultOk)
      return false;
    stream.seek(0, IBStream::kIBSeekSet, nullptr);
    controller_->setComponentState(&stream);
    for (uint32_t i = 0; i < s_->parameterCount; ++i)
      paramWrite(s_, i, float(controller_->getParamNormalized(ids_[i])));
    return true;
  }
  bool openEditor(HWND parent) override {
    view_ = controller_->createView(ViewType::kEditor);
    if (!view_)
      return false;
    ViewRect r{};
    if (view_->getSize(&r) != kResultOk || r.getWidth() <= 0 ||
        r.getHeight() <= 0 || r.getWidth() > int(maxWidth) ||
        r.getHeight() > int(maxHeight) ||
        view_->attached(parent, kPlatformTypeHWND) != kResultOk) {
      view_->release();
      view_ = nullptr;
      return false;
    }
    s_->width = r.getWidth();
    s_->height = r.getHeight();
    return true;
  }
  void closeEditor() override {
    if (view_) {
      view_->removed();
      view_->release();
      view_ = nullptr;
    }
  }
  void idle() override {
    for (uint32_t i = 0; i < s_->parameterCount; ++i)
      if (InterlockedExchange(&controllerDirty_[i], 0))
        controller_->setParamNormalized(ids_[i], paramRead(s_, i));
  }
};
std::unique_ptr<Adapter> loadVst3(const std::wstring &path, Shared *s) {
  return std::make_unique<Vst3>(path, s);
}
} // namespace scaler
