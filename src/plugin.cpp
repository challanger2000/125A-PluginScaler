#include "scaler/bridge.hpp"
#include "scaler/editor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace scaler {
static_assert(std::atomic<float>::is_always_lock_free);
HMODULE pluginModule = nullptr;
class Plugin final : public IComponent,
                     public IAudioProcessor,
                     public IEditController {
  SCALER_REFS
  std::shared_ptr<Bridge> bridge_;
  IComponentHandler *handler_ = nullptr;
  bool initialized_ = false, active_ = false;
  std::atomic<bool> processing_{false};
  double rate_ = 48000;
  int32 block_ = maxFrames, mode_ = kRealtime;
  volatile LONG scale_ = 100;
  std::atomic<float> mirror_[maxParams]{};
  float defaults_[maxParams]{};
  LONG notified_[maxParams]{};
  bool inputActive_ = true, outputActive_ = true, eventActive_ = true;
  void notifications() {
    if (!bridge_)
      return;
    for (uint32_t i = 0; i < bridge_->shared->parameterCount; ++i) {
      LONG generation = InterlockedCompareExchange(
          &bridge_->shared->callbackGeneration[i], 0, 0);
      if (generation == notified_[i])
        continue;
      notified_[i] = generation;
      float v = paramRead(bridge_->shared, i);
      mirror_[i] = v;
      if (handler_) {
        handler_->beginEdit(i);
        handler_->performEdit(i, v);
        handler_->endEdit(i);
      }
    }
  }

public:
  ~Plugin() {
    if (handler_)
      handler_->release();
  }
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
  tresult PLUGIN_API initialize(FUnknown *) override {
    if (initialized_)
      return kResultFalse;
    try {
      auto directory = moduleDirectory(pluginModule);
      auto ini = directory + L"\\PluginScaler.ini";
      wchar_t value[32768]{};
      GetPrivateProfileStringW(L"plugin", L"path", L"scaler-fixture.dll", value,
                               32768, ini.c_str());
      std::wstring target = value;
      if (target.find(L'"') != std::wstring::npos || target.empty())
        return kInvalidArgument;
      if (target.size() < 2 || target[1] != L':')
        target = directory + L"\\" + target;
      DWORD attributes = GetFileAttributesW(target.c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES &&
          (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        auto name = target.substr(target.find_last_of(L"\\/") + 1);
        target += L"\\Contents\\x86_64-win\\" + name;
      }
      // Determine helper bitness from the actual PE header, never load the
      // hosted DLL into the DAW merely to discover its architecture.
      HANDLE file = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, 0, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        return kResultFalse;
      IMAGE_DOS_HEADER dos{};
      DWORD n = 0;
      bool ok = ReadFile(file, &dos, sizeof(dos), &n, nullptr) &&
                n == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                dos.e_lfanew >= 0;
      DWORD signature = 0;
      IMAGE_FILE_HEADER pe{};
      if (ok) {
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN);
        ok = ReadFile(file, &signature, 4, &n, nullptr) && n == 4 &&
             signature == IMAGE_NT_SIGNATURE &&
             ReadFile(file, &pe, sizeof(pe), &n, nullptr) && n == sizeof(pe);
      }
      CloseHandle(file);
      if (!ok || (pe.Machine != IMAGE_FILE_MACHINE_I386 &&
                  pe.Machine != IMAGE_FILE_MACHINE_AMD64))
        return kResultFalse;
      bridge_ = std::make_shared<Bridge>();
      if (!bridge_->open(directory, target,
                         pe.Machine == IMAGE_FILE_MACHINE_I386
                             ? L"scaler-helper32.exe"
                             : L"scaler-helper64.exe")) {
        bridge_.reset();
        return kResultFalse;
      }
      if (!bridge_->configure(rate_)) {
        bridge_.reset();
        return kResultFalse;
      }
      for (uint32_t i = 0; i < bridge_->shared->parameterCount; ++i) {
        defaults_[i] = paramRead(bridge_->shared, i);
        mirror_[i] = defaults_[i];
      }
      initialized_ = true;
      return kResultOk;
    } catch (...) {
      bridge_.reset();
      return kInternalError;
    }
  }
  tresult PLUGIN_API terminate() override {
    if (processing_)
      return kResultFalse;
    bridge_.reset();
    initialized_ = false;
    active_ = false;
    return kResultOk;
  }
  tresult PLUGIN_API getControllerClassId(TUID) override {
    return kResultFalse;
  } // Combined component/controller.
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type,
                               BusDirection direction) override {
    return type == kAudio ? 1 : (type == kEvent && direction == kInput ? 1 : 0);
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index,
                                BusInfo &info) override {
    if (index != 0 || !getBusCount(type, dir))
      return kInvalidArgument;
    info = {};
    info.mediaType = type;
    info.direction = dir;
    info.channelCount = type == kAudio ? 2 : 16;
    text16(info.name, type == kAudio ? "Stereo" : "MIDI input");
    info.busType = kMain;
    info.flags = BusInfo::kDefaultActive;
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override {
    return kNotImplemented;
  }
  tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index,
                                 TBool state) override {
    if (index != 0 || !getBusCount(type, dir))
      return kInvalidArgument;
    if (type == kAudio) {
      if (dir == kInput)
        inputActive_ = state;
      else
        outputActive_ = state;
    } else
      eventActive_ = state;
    return kResultOk;
  }
  tresult PLUGIN_API setActive(TBool state) override {
    if (!bridge_ || processing_)
      return kResultFalse;
    if (state && !bridge_->configure(rate_))
      return kResultFalse;
    if (!bridge_->activate(state))
      return kResultFalse;
    active_ = state;
    return kResultOk;
  }
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement *ins, int32 ni,
                                        SpeakerArrangement *outs,
                                        int32 no) override {
    return ni == 1 && no == 1 && ins && outs && ins[0] == SpeakerArr::kStereo &&
                   outs[0] == SpeakerArr::kStereo
               ? kResultOk
               : kResultFalse;
  }
  tresult PLUGIN_API getBusArrangement(
      BusDirection, int32 i, SpeakerArrangement &arrangement) override {
    if (i != 0)
      return kInvalidArgument;
    arrangement = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 size) override {
    return size == kSample32 ? kResultTrue : kResultFalse;
  }
  uint32 PLUGIN_API getLatencySamples() override {
    return bridge_ ? bridge_->shared->latency : 0;
  }
  uint32 PLUGIN_API getTailSamples() override {
    return kInfiniteTail;
  } // Hosted tails are not yet queried.
  tresult PLUGIN_API setupProcessing(ProcessSetup &s) override {
    if (processing_ || s.symbolicSampleSize != kSample32 ||
        s.maxSamplesPerBlock <= 0 || s.maxSamplesPerBlock > int32(maxFrames) ||
        !std::isfinite(s.sampleRate) || s.sampleRate < 8000 ||
        s.sampleRate > 384000 || s.processMode != kRealtime)
      return kResultFalse;
    rate_ = s.sampleRate;
    block_ = s.maxSamplesPerBlock;
    mode_ = s.processMode;
    return kResultOk;
  }
  tresult PLUGIN_API setProcessing(TBool state) override {
    if (state && (!active_ || !bridge_ || !bridge_->healthy()))
      return kResultFalse;
    processing_ = state;
    return kResultOk;
  }
  tresult PLUGIN_API process(ProcessData &d) override {
    auto silence = [&] {
      if (d.symbolicSampleSize == kSample32 && d.outputs && d.numOutputs > 0 &&
          d.numSamples >= 0 && d.numSamples <= int32(maxFrames)) {
        auto &o = d.outputs[0];
        if (o.channelBuffers32)
          for (int c = 0; c < std::min(2, o.numChannels); ++c)
            if (o.channelBuffers32[c])
              std::fill_n(o.channelBuffers32[c], d.numSamples, 0.f);
        o.silenceFlags = 3;
      }
    };
    if (!bridge_ || !bridge_->healthy() || !processing_) {
      silence();
      return kResultOk;
    }
    if (d.symbolicSampleSize != kSample32 || d.numSamples < 0 ||
        d.numSamples > block_ || d.numInputs < 0 || d.numInputs > 1 ||
        d.numOutputs != 1 || !d.outputs || d.outputs[0].numChannels != 2 ||
        !d.outputs[0].channelBuffers32 || !d.outputs[0].channelBuffers32[0] ||
        !d.outputs[0].channelBuffers32[1]) {
      silence();
      return kInvalidArgument;
    }
    auto *s = bridge_->shared;
    s->eventCount = 0;
    auto append = [&](scaler::Event event) {
      if (s->eventCount == maxEvents)
        return false;
      s->events[s->eventCount++] = event;
      return true;
    };
    if (d.inputParameterChanges) {
      int32 count = d.inputParameterChanges->getParameterCount();
      if (count < 0 || count > int32(maxParams)) {
        silence();
        return kInvalidArgument;
      }
      for (int32 i = 0; i < count; ++i) {
        auto *q = d.inputParameterChanges->getParameterData(i);
        if (!q || q->getParameterId() >= s->parameterCount) {
          silence();
          return kInvalidArgument;
        }
        int32 points = q->getPointCount();
        if (points < 0 || points > int32(maxEvents)) {
          silence();
          return kInvalidArgument;
        }
        for (int32 p = 0; p < points; ++p) {
          int32 offset;
          ParamValue v;
          if (q->getPoint(p, offset, v) != kResultOk || !std::isfinite(v) ||
              v < 0 || v > 1 || offset < 0 ||
              offset >= std::max(1, d.numSamples) ||
              !append({EventType::Parameter,
                       uint32_t(offset),
                       q->getParameterId(),
                       float(v),
                       {},
                       0})) {
            silence();
            return kInvalidArgument;
          }
        }
      }
    }
    if (eventActive_ && d.inputEvents) {
      int32 count = d.inputEvents->getEventCount();
      if (count < 0 || count > int32(maxEvents)) {
        silence();
        return kInvalidArgument;
      }
      for (int32 i = 0; i < count; ++i) {
        Steinberg::Vst::Event e{};
        if (d.inputEvents->getEvent(i, e) != kResultOk) {
          silence();
          return kInvalidArgument;
        }
        if (e.busIndex != 0 || e.sampleOffset < 0 ||
            e.sampleOffset >= d.numSamples) {
          silence();
          return kInvalidArgument;
        }
        scaler::Event out{};
        out.type = EventType::Midi;
        out.offset = e.sampleOffset;
        int channel, pitch;
        float velocity;
        if (e.type == Steinberg::Vst::Event::kNoteOnEvent) {
          channel = e.noteOn.channel;
          pitch = e.noteOn.pitch;
          velocity = e.noteOn.velocity;
          out.midi[0] = 0x90;
        } else if (e.type == Steinberg::Vst::Event::kNoteOffEvent) {
          channel = e.noteOff.channel;
          pitch = e.noteOff.pitch;
          velocity = e.noteOff.velocity;
          out.midi[0] = 0x80;
        } else {
          silence();
          return kNotImplemented;
        }
        if (channel < 0 || channel > 15 || pitch < 0 || pitch > 127 ||
            !std::isfinite(velocity) || velocity < 0 || velocity > 1) {
          silence();
          return kInvalidArgument;
        }
        out.midi[0] |= uint8_t(channel);
        out.midi[1] = uint8_t(pitch);
        out.midi[2] = uint8_t(std::lround(velocity * 127));
        if (!append(out)) {
          silence();
          return kInvalidArgument;
        }
      }
    }
    // Stable bounded insertion sort: preserve equal-offset event order without
    // allocating.
    for (uint32_t i = 1; i < s->eventCount; ++i) {
      auto e = s->events[i];
      uint32_t j = i;
      while (j && s->events[j - 1].offset > e.offset) {
        s->events[j] = s->events[j - 1];
        --j;
      }
      s->events[j] = e;
    }
    if (d.numInputs && inputActive_ &&
        (!d.inputs || d.inputs[0].numChannels != 2 ||
         !d.inputs[0].channelBuffers32 || !d.inputs[0].channelBuffers32[0] ||
         !d.inputs[0].channelBuffers32[1])) {
      silence();
      return kInvalidArgument;
    }
    for (int c = 0; c < 2; ++c) {
      if (d.numInputs && inputActive_ &&
          !(d.inputs[0].silenceFlags & (uint64(1) << c)))
        std::copy_n(d.inputs[0].channelBuffers32[c], d.numSamples,
                    s->inputs[c]);
      else
        std::fill_n(s->inputs[c], d.numSamples, 0.f);
      std::fill_n(s->outputs[c], d.numSamples, 0.f);
    }
    if (!bridge_->audio(uint32_t(d.numSamples), false)) {
      silence();
      return kResultOk;
    }
    for (uint32_t i = 0; i < s->eventCount; ++i)
      if (s->events[i].type == EventType::Parameter)
        mirror_[s->events[i].index] = s->events[i].value;
    d.outputs[0].silenceFlags = 0;
    for (int c = 0; c < 2; ++c) {
      if (outputActive_)
        std::copy_n(s->outputs[c], d.numSamples,
                    d.outputs[0].channelBuffers32[c]);
      else
        std::fill_n(d.outputs[0].channelBuffers32[c], d.numSamples, 0.f);
    }
    if (!outputActive_)
      d.outputs[0].silenceFlags = 3;
    return kResultOk;
  }
  int32 PLUGIN_API getParameterCount() override {
    return bridge_ ? int32(bridge_->shared->parameterCount) : 0;
  }
  tresult PLUGIN_API getParameterInfo(int32 index,
                                      ParameterInfo &info) override {
    if (index < 0 || index >= getParameterCount())
      return kInvalidArgument;
    info = {};
    info.id = index;
    text16(info.title, bridge_->shared->parameterNames[index]);
    info.defaultNormalizedValue = defaults_[index];
    info.flags = ParameterInfo::kCanAutomate;
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value,
                                           String128 out) override {
    if (id >= uint32(getParameterCount()))
      return kInvalidArgument;
    char text[64];
    snprintf(text, sizeof(text), "%.4f", value);
    text16(*reinterpret_cast<String128 *>(out), text);
    return kResultOk;
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
    return id < uint32(getParameterCount()) ? double(mirror_[id].load()) : 0;
  }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
    if (id >= uint32(getParameterCount()) || !std::isfinite(value) ||
        value < 0 || value > 1)
      return kInvalidArgument;
    mirror_[id] = float(value);
    return kResultOk;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler *h) override {
    if (h)
      h->addRef();
    if (handler_)
      handler_->release();
    handler_ = h;
    return kResultOk;
  }
  IPlugView *PLUGIN_API createView(FIDString name) override {
    if (!name || strcmp(name, ViewType::kEditor) || !bridge_ ||
        !bridge_->healthy())
      return nullptr;
    auto *s = bridge_->shared;
    InterlockedExchange(&s->editorWanted, 1);
    auto deadline = GetTickCount64() + 2000;
    while (s->editorOpen == 0 && GetTickCount64() < deadline)
      Sleep(2);
    if (s->editorOpen != 1)
      return nullptr;
    try {
      return new Editor(bridge_, this, &scale_, [this] { notifications(); });
    } catch (...) {
      return nullptr;
    }
  }
  tresult PLUGIN_API setComponentState(IBStream *) override {
    return kResultOk;
  } // State already belongs to this combined component.
  tresult PLUGIN_API getState(IBStream *stream) override {
    if (!stream || !bridge_ || processing_ || !bridge_->state(false))
      return kResultFalse;
    uint32 header[]{0x31535031, 1, uint32(scale_), bridge_->shared->stateBytes};
    int32 n;
    if (stream->write(header, sizeof(header), &n) != kResultOk ||
        n != sizeof(header) ||
        stream->write(bridge_->shared->identity, 16, &n) != kResultOk ||
        n != 16)
      return kResultFalse;
    return stream->write(bridge_->shared->state, header[3], &n) == kResultOk &&
                   n == int32(header[3])
               ? kResultOk
               : kResultFalse;
  }
  tresult PLUGIN_API setState(IBStream *stream) override {
    if (!stream || !bridge_ || processing_)
      return kResultFalse;
    uint32 header[4];
    uint8_t identity[16];
    int32 n;
    if (stream->read(header, sizeof(header), &n) != kResultOk ||
        n != sizeof(header) || header[0] != 0x31535031 || header[1] != 1 ||
        header[3] > maxState ||
        std::find(scales.begin(), scales.end(), int(header[2])) ==
            scales.end() ||
        stream->read(identity, 16, &n) != kResultOk || n != 16 ||
        memcmp(identity, bridge_->shared->identity, 16))
      return kResultFalse;
    auto bytes =
        std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[header[3]]);
    if (!bytes)
      return kOutOfMemory;
    if (stream->read(bytes.get(), header[3], &n) != kResultOk ||
        n != int32(header[3]))
      return kResultFalse;
    if (header[3])
      memcpy(bridge_->shared->state, bytes.get(), header[3]);
    bridge_->shared->stateBytes = header[3];
    if (!bridge_->state(true))
      return kResultFalse;
    InterlockedExchange(&scale_, LONG(header[2]));
    for (uint32_t i = 0; i < bridge_->shared->parameterCount; ++i)
      mirror_[i] = paramRead(bridge_->shared, i);
    return kResultOk;
  }
};
class Factory final : public IPluginFactory2 {
  SCALER_REFS
public:
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    SCALER_QUERY(IPluginFactory2);
    SCALER_QUERY(IPluginFactory);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override {
    if (!info)
      return kInvalidArgument;
    *info = {};
    strcpy(info->vendor, "125A");
    strcpy(info->url, "https://github.com/challanger2000/125A-PluginScaler");
    info->flags = PFactoryInfo::kUnicode;
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 1; }
  tresult PLUGIN_API getClassInfo(int32 i, PClassInfo *info) override {
    if (i || !info)
      return kInvalidArgument;
    *info = {};
    memcpy(info->cid, componentId, 16);
    info->cardinality = PClassInfo::kManyInstances;
    strcpy(info->category, kVstAudioEffectClass);
    strcpy(info->name, "125A PluginScaler Prototype");
    return kResultOk;
  }
  tresult PLUGIN_API getClassInfo2(int32 i, PClassInfo2 *info) override {
    if (i || !info)
      return kInvalidArgument;
    *info = {};
    memcpy(info->cid, componentId, 16);
    info->cardinality = PClassInfo::kManyInstances;
    strcpy(info->category, kVstAudioEffectClass);
    strcpy(info->name, "125A PluginScaler Prototype");
    strcpy(info->subCategories, "Fx|Tools|OnlyRT");
    strcpy(info->vendor, "125A");
    strcpy(info->version, "0.1.0");
    strcpy(info->sdkVersion, kVstVersionString);
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString iid,
                                    void **obj) override {
    if (!obj)
      return kInvalidArgument;
    *obj = nullptr;
    if (!same(cid, componentId))
      return kInvalidArgument;
    auto *plugin = new (std::nothrow) Plugin;
    if (!plugin)
      return kOutOfMemory;
    auto result = plugin->queryInterface(iid, obj);
    plugin->release();
    return result;
  }
};
} // namespace scaler
extern "C" __declspec(dllexport) Steinberg::IPluginFactory *PLUGIN_API
GetPluginFactory() {
  return new (std::nothrow) scaler::Factory;
}
extern "C" __declspec(dllexport) bool InitDll() { return true; }
extern "C" __declspec(dllexport) bool ExitDll() { return true; }
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    scaler::pluginModule = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
