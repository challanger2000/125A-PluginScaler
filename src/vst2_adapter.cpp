#include "scaler/adapter.hpp"
#include "scaler/vst2_abi.hpp"
#include "scaler/win32.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace scaler {
namespace abi = pluginscaler::formats::vst2abi;
class Vst2 final : public Adapter {
  HMODULE module_ = nullptr;
  abi::AEffect *effect_ = nullptr;
  Shared *s_;
  bool editor_ = false;
  static Vst2 *instance_;
  static intptr_t __cdecl callback(abi::AEffect *, int32_t op, int32_t index,
                                   intptr_t, void *ptr, float value) {
    auto *self = instance_;
    switch (op) {
    case abi::AudioMasterVersion:
      return 2400;
    case abi::AudioMasterGetSampleRate:
      return self ? intptr_t(self->s_->sampleRate) : 48000;
    case abi::AudioMasterGetBlockSize:
      return maxFrames;
    case abi::AudioMasterAutomate:
      if (self && index >= 0 && uint32_t(index) < self->s_->parameterCount &&
          std::isfinite(value)) {
        paramWrite(self->s_, index, value);
        InterlockedIncrement(&self->s_->callbackGeneration[index]);
      }
      return 1;
    case abi::AudioMasterGetVendorString:
      if (ptr)
        strcpy(static_cast<char *>(ptr), "125A");
      return 1;
    case abi::AudioMasterGetProductString:
      if (ptr)
        strcpy(static_cast<char *>(ptr), "PluginScaler prototype");
      return 1;
    // No invented time/transport flags and no unsupported host capabilities.
    default:
      return 0;
    }
  }
  intptr_t dispatch(int op, int index = 0, intptr_t value = 0,
                    void *ptr = nullptr, float opt = 0) {
    return effect_->dispatcher(effect_, op, index, value, ptr, opt);
  }

public:
  Vst2(const std::wstring &path, Shared *shared) : s_(shared) {
    instance_ = this;
    module_ = LoadLibraryW(path.c_str());
    if (!module_)
      throw std::runtime_error(
          "cannot load VST2 module (path, bitness or dependency)");
    auto entry = symbol<abi::EntryProc>(module_, "VSTPluginMain");
    if (!entry)
      entry = symbol<abi::EntryProc>(module_, "main");
    if (!entry || !(effect_ = entry(callback)) ||
        effect_->magic != abi::kEffectMagic || !effect_->dispatcher ||
        !effect_->processReplacing || !effect_->setParameter ||
        !effect_->getParameter)
      throw std::runtime_error("unsupported VST2 entry/processing interface");
    effect_->user = this;
    dispatch(abi::EffOpen);
    if (effect_->numInputs < 0 || effect_->numInputs > 2 ||
        effect_->numOutputs != 2 || effect_->numParams < 0 ||
        effect_->numParams > int(maxParams))
      throw std::runtime_error(
          "prototype requires 0..2 inputs, stereo output, <=128 parameters");
    s_->parameterCount = effect_->numParams;
    memcpy(s_->identity, &effect_->uniqueId, 4);
    s_->identity[15] = 2;
    s_->latency = uint32_t(std::max(0, effect_->initialDelay));
    for (uint32_t i = 0; i < s_->parameterCount; ++i) {
      char name[256]{};
      dispatch(abi::EffGetParamName, i, 0, name);
      memcpy(s_->parameterNames[i], name, 63);
      paramWrite(s_, i, effect_->getParameter(effect_, i));
    }
  }
  ~Vst2() override {
    if (effect_) {
      closeEditor();
      dispatch(abi::EffStopProcess);
      dispatch(abi::EffMainsChanged);
      dispatch(abi::EffClose);
    }
    if (module_)
      FreeLibrary(module_);
    instance_ = nullptr;
  }
  void configure(double rate) override {
    dispatch(abi::EffMainsChanged);
    dispatch(abi::EffSetSampleRate, 0, 0, nullptr, float(rate));
    dispatch(abi::EffSetBlockSize, 0, maxFrames);
  }
  void activate(bool state) override {
    if (!state)
      dispatch(abi::EffStopProcess);
    dispatch(abi::EffMainsChanged, 0, state ? 1 : 0);
    if (state)
      dispatch(abi::EffStartProcess);
    s_->latency = uint32_t(std::max(0, effect_->initialDelay));
  }
  void process() override {
    uint32_t cursor = 0;
    auto render = [&](uint32_t end) {
      if (end == cursor)
        return;
      float *in[]{s_->inputs[0] + cursor, s_->inputs[1] + cursor};
      float *out[]{s_->outputs[0] + cursor, s_->outputs[1] + cursor};
      effect_->processReplacing(effect_, in, out, int(end - cursor));
      cursor = end;
    };
    for (uint32_t i = 0; i < s_->eventCount; ++i) {
      const auto &e = s_->events[i];
      render(e.offset);
      if (e.type == EventType::Parameter && e.index < s_->parameterCount) {
        effect_->setParameter(effect_, e.index, e.value);
        paramWrite(s_, e.index, e.value);
      } else if (e.type == EventType::Midi) {
        abi::VstMidiEvent midi{};
        midi.type = 1;
        midi.byteSize = 24;
        memcpy(midi.midiData, e.midi, 4);
        abi::VstEvents events{};
        events.numEvents = 1;
        events.events[0] = reinterpret_cast<abi::VstEvent *>(&midi);
        dispatch(abi::EffProcessEvents, 0, 0, &events);
      }
    }
    render(s_->frames);
  }
  bool getState() override {
    if (effect_->flags & abi::kEffectFlagProgramChunks) {
      void *data = nullptr;
      auto n = dispatch(abi::EffGetChunk, 0, 0, &data);
      if (n <= 0 || n > intptr_t(maxState) || !data)
        return false;
      s_->stateBytes = uint32_t(n);
      memcpy(s_->state, data, size_t(n));
    } else {
      int32_t program = int32_t(dispatch(abi::EffGetProgram));
      memcpy(s_->state, &program, 4);
      s_->stateBytes = 4 + 4 * s_->parameterCount;
      for (uint32_t i = 0; i < s_->parameterCount; ++i) {
        float p = effect_->getParameter(effect_, i);
        memcpy(s_->state + 4 + 4 * i, &p, 4);
      }
    }
    return true;
  }
  bool setState() override {
    if (effect_->flags & abi::kEffectFlagProgramChunks)
      dispatch(abi::EffSetChunk, 0, s_->stateBytes, s_->state);
    else {
      if (s_->stateBytes != 4 + 4 * s_->parameterCount)
        return false;
      int32_t program;
      memcpy(&program, s_->state, 4);
      if (program < 0 || program >= effect_->numPrograms)
        return false;
      for (uint32_t i = 0; i < s_->parameterCount; ++i) {
        float p;
        memcpy(&p, s_->state + 4 + 4 * i, 4);
        if (!std::isfinite(p) || p < 0 || p > 1)
          return false;
      }
      dispatch(abi::EffSetProgram, 0, program);
      for (uint32_t i = 0; i < s_->parameterCount; ++i) {
        float p;
        memcpy(&p, s_->state + 4 + 4 * i, 4);
        effect_->setParameter(effect_, i, p);
      }
    }
    for (uint32_t i = 0; i < s_->parameterCount; ++i)
      paramWrite(s_, i, effect_->getParameter(effect_, i));
    return true;
  }
  bool openEditor(HWND parent) override {
    if (!(effect_->flags & abi::kEffectFlagHasEditor))
      return false;
    abi::VstRect *r = nullptr;
    dispatch(abi::EffEditGetRect, 0, 0, &r);
    if (!r || r->right <= r->left || r->bottom <= r->top ||
        r->right - r->left > int(maxWidth) ||
        r->bottom - r->top > int(maxHeight))
      return false;
    s_->width = r->right - r->left;
    s_->height = r->bottom - r->top;
    dispatch(abi::EffEditOpen, 0, 0, parent);
    editor_ = true;
    return true;
  }
  void closeEditor() override {
    if (editor_) {
      dispatch(abi::EffEditClose);
      editor_ = false;
    }
  }
  void idle() override {
    // Deferred: no universal re-entrancy guarantee for legacy editor idle.
  }
};
Vst2 *Vst2::instance_ = nullptr;
std::unique_ptr<Adapter> loadVst2(const std::wstring &path, Shared *s) {
  return std::make_unique<Vst2>(path, s);
}
} // namespace scaler
