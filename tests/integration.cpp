#include "scaler/bridge.hpp"
#include "scaler/geometry.hpp"
#include "scaler/stream.hpp"
#include "scaler/win32.hpp"
#include "test_lists.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <tlhelp32.h>
#include <windowsx.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x))                                                                  \
      throw std::runtime_error(std::string(#x) + " at line " +                 \
                               std::to_string(__LINE__));                      \
  } while (false)
using namespace scaler;
void pump(int milliseconds) {
  auto end = GetTickCount64() + milliseconds;
  do {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(1);
  } while (GetTickCount64() < end);
}
void screenshot(HWND window, const char *filename) {
  RECT r{};
  GetClientRect(window, &r);
  int w = r.right, h = r.bottom;
  HDC source = GetDC(window), dc = CreateCompatibleDC(source);
  HBITMAP bitmap = CreateCompatibleBitmap(source, w, h);
  auto old = SelectObject(dc, bitmap);
  CHECK(BitBlt(dc, 0, 0, w, h, source, 0, 0, SRCCOPY));
  SelectObject(dc, old);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = w;
  info.bmiHeader.biHeight = -h;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<uint32_t> pixels(size_t(w) * h);
  CHECK(GetDIBits(dc, bitmap, 0, h, pixels.data(), &info, DIB_RGB_COLORS) == h);
  BITMAPFILEHEADER header{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
  header.bfSize = header.bfOffBits + DWORD(pixels.size() * 4);
  std::ofstream file(filename, std::ios::binary);
  file.write(reinterpret_cast<const char *>(&header), sizeof(header));
  file.write(reinterpret_cast<const char *>(&info.bmiHeader),
             sizeof(BITMAPINFOHEADER));
  file.write(reinterpret_cast<const char *>(pixels.data()), pixels.size() * 4);
  CHECK(file.good());
  DeleteObject(bitmap);
  DeleteDC(dc);
  ReleaseDC(window, source);
}
int helperCount() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  CHECK(snapshot != INVALID_HANDLE_VALUE);
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  int count = 0;
  if (Process32FirstW(snapshot, &entry))
    do {
      if (!_wcsicmp(entry.szExeFile, L"scaler-helper64.exe") ||
          !_wcsicmp(entry.szExeFile, L"scaler-helper32.exe"))
        ++count;
    } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot);
  return count;
}
struct Frame final : IPlugFrame {
  HWND parent;
  bool reject = false;
  int calls = 0;
  explicit Frame(HWND p) : parent(p) {}
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    *obj = nullptr;
    SCALER_QUERY(IPlugFrame);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *rect) override {
    ++calls;
    if (reject)
      return kResultFalse;
    RECT r{0, 0, rect->getWidth(), rect->getHeight()};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(parent, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    return view->onSize(rect);
  }
};
struct Handler final : IComponentHandler {
  int begins = 0, values = 0, ends = 0;
  double last = -1;
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    *obj = nullptr;
    SCALER_QUERY(IComponentHandler);
    SCALER_QUERY(FUnknown);
    return kNoInterface;
  }
  tresult PLUGIN_API beginEdit(ParamID) override {
    ++begins;
    return kResultOk;
  }
  tresult PLUGIN_API performEdit(ParamID, ParamValue value) override {
    ++values;
    last = value;
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(ParamID) override {
    ++ends;
    return kResultOk;
  }
  tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
};
struct Loaded {
  HMODULE dll = nullptr;
  IPluginFactory *factory = nullptr;
  IComponent *component = nullptr;
  IAudioProcessor *audio = nullptr;
  IEditController *controller = nullptr;
  explicit Loaded(const wchar_t *path) {
    dll = LoadLibraryW(path);
    CHECK(dll);
    auto get =
        symbol<IPluginFactory *(PLUGIN_API *)()>(dll, "GetPluginFactory");
    CHECK(get);
    factory = get();
    CHECK(factory && factory->countClasses() == 1);
    PClassInfo info{};
    CHECK(factory->getClassInfo(0, &info) == kResultOk);
    CHECK(!strcmp(info.category, kVstAudioEffectClass));
    CHECK(factory->createInstance(info.cid, INLINE_UID_OF(IComponent),
                                  reinterpret_cast<void **>(&component)) ==
          kResultOk);
    CHECK(component->initialize(nullptr) == kResultOk);
    CHECK(component->queryInterface(INLINE_UID_OF(IAudioProcessor),
                                    reinterpret_cast<void **>(&audio)) ==
          kResultOk);
    CHECK(component->queryInterface(INLINE_UID_OF(IEditController),
                                    reinterpret_cast<void **>(&controller)) ==
          kResultOk);
  }
  ~Loaded() {
    if (audio)
      audio->setProcessing(false);
    if (component)
      component->setActive(false);
    if (controller)
      controller->release();
    if (audio)
      audio->release();
    if (component) {
      component->terminate();
      component->release();
    }
    if (factory)
      factory->release();
    if (dll)
      FreeLibrary(dll);
  }
  void start(double rate = 48000, int block = 2048) {
    ProcessSetup setup{kRealtime, kSample32, block, rate};
    CHECK(audio->setupProcessing(setup) == kResultOk);
    CHECK(component->setActive(true) == kResultOk);
    CHECK(audio->setProcessing(true) == kResultOk);
  }
  void stop() {
    CHECK(audio->setProcessing(false) == kResultOk);
    CHECK(component->setActive(false) == kResultOk);
  }
};
struct Block {
  float in[2][2048]{}, out[2][2048]{};
  float *inputs[2]{in[0], in[1]}, *outputs[2]{out[0], out[1]};
  AudioBusBuffers input, output;
  ProcessData data;
  explicit Block(int frames = 64) {
    input.numChannels = 2;
    input.channelBuffers32 = inputs;
    output.numChannels = 2;
    output.channelBuffers32 = outputs;
    data.numInputs = 1;
    data.inputs = &input;
    data.numOutputs = 1;
    data.outputs = &output;
    data.numSamples = frames;
  }
  void render(Loaded &p) { CHECK(p.audio->process(data) == kResultOk); }
};
void automate(Loaded &p, float value, ParamID id = 0) {
  Block b;
  test::Changes c;
  c.q.id = id;
  c.q.points = {{0, value}};
  b.data.inputParameterChanges = &c;
  b.render(p);
}
int wmain(int argc, wchar_t **argv) {
  try {
    CHECK(argc == 2);
    const int baseline = helperCount();
    WNDCLASSW wc{};
    wc.lpszClassName = L"125A.TestHost";
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    CHECK(RegisterClassW(&wc));
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"125A test host",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 350,
                                  260, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(window);
    Frame frame(window);
    Handler handler;
    {
      Loaded p(argv[1]);
      CHECK(p.controller->getParameterCount() == 2);
      p.controller->setComponentHandler(&handler);
      SpeakerArrangement in = SpeakerArr::kMono, out = SpeakerArr::kStereo;
      CHECK(p.audio->setBusArrangements(&in, 1, &out, 1) == kResultFalse);
      in = SpeakerArr::kStereo;
      CHECK(p.audio->setBusArrangements(&in, 1, &out, 1) == kResultOk);
      CHECK(p.audio->canProcessSampleSize(kSample64) == kResultFalse);
      for (double rate : {44100., 48000., 96000.}) {
        p.start(rate);
        for (int frames : {1, 31, 64, 257, 1024, 2048}) {
          Block b(frames);
          for (int i = 0; i < frames; ++i) {
            b.in[0][i] = float(i - 17) / 2048;
            b.in[1][i] = -float(i + 13) / 4096;
          }
          b.render(p);
          for (int c = 0; c < 2; ++c)
            for (int i = 0; i < frames; ++i)
              CHECK(b.out[c][i] == b.in[c][i] * 0.5f);
        }
        p.stop();
      }
      std::cout << "PASS actual VST3 -> helper -> synthetic VST2: stereo PCM, "
                   "3 rates, 6 block sizes\n";
      p.start();
      Block b;
      test::Events events;
      events.note(17, true);
      events.note(43, false);
      b.data.inputEvents = &events;
      b.render(p);
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 64; ++i)
          CHECK(b.out[c][i] == (i >= 17 && i < 43 ? 0.125f : 0.f));
      test::Changes changes;
      changes.q.points = {{19, 0.75}, {47, 0.25}};
      Block automation;
      std::fill_n(automation.in[0], 64, 1.f);
      std::fill_n(automation.in[1], 64, -1.f);
      automation.data.inputParameterChanges = &changes;
      automation.render(p);
      for (int i = 0; i < 64; ++i) {
        float expected = i < 19 ? 0.5f : i < 47 ? 0.75f : 0.25f;
        CHECK(automation.out[0][i] == expected &&
              automation.out[1][i] == -expected);
      }
      automate(p, 0.5f);
      {
        test::Changes invalid;
        invalid.q.points = {{0, std::numeric_limits<double>::quiet_NaN()}};
        Block bad;
        bad.data.inputParameterChanges = &invalid;
        std::fill_n(bad.out[0], 64, 1.f);
        CHECK(p.audio->process(bad.data) == kInvalidArgument);
        for (int i = 0; i < 64; ++i)
          CHECK(bad.out[0][i] == 0);
        invalid.q.points.assign(257, {0, 0.5});
        CHECK(p.audio->process(bad.data) == kInvalidArgument);
        automate(p, 0.5f); // Invalid packets must not poison the live bridge.
      }
      p.stop();
      Stream saved;
      CHECK(p.component->getState(&saved) == kResultOk);
      p.start();
      automate(p, 0.9f);
      p.stop();
      saved.seek(0, IBStream::kIBSeekSet, nullptr);
      CHECK(p.component->setState(&saved) == kResultOk);
      Stream bad;
      bad.bytes = saved.bytes;
      bad.bytes.resize(34);
      CHECK(p.component->setState(&bad) != kResultOk);
      CHECK(p.controller->getParamNormalized(0) == 0.5);
      p.start();
      Block recall;
      std::fill_n(recall.in[0], 64, 0.25f);
      recall.render(p);
      for (int i = 0; i < 64; ++i)
        CHECK(recall.out[0][i] == 0.125f);
      std::cout << "PASS note on/off sample offsets, sample-offset automation, "
                   "state recall PCM, truncated-state rollback\n";
      auto *view = p.controller->createView(ViewType::kEditor);
      CHECK(view);
      view->setFrame(&frame);
      CHECK(view->attached(window, kPlatformTypeHWND) == kResultOk);
      pump(100);
      HWND surface =
          FindWindowExW(window, nullptr, L"125A.ScaledSurface", nullptr);
      CHECK(surface);
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      CHECK(GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                            &monitor));
      Size available{monitor.rcWork.right - monitor.rcWork.left - 32,
                     monitor.rcWork.bottom - monitor.rcWork.top - 28 - 64};
      int testedScales = 0;
      for (int scale : scales) {
        if (!Geometry({320, 180}).fits(scale, available)) {
          CHECK(SendMessageW(surface, WM_COMMAND, scale, 0) == 0);
          continue;
        }
        ++testedScales;
        CHECK(SendMessageW(surface, WM_COMMAND, scale, 0) == 1);
        pump(35);
        ViewRect rect;
        CHECK(view->getSize(&rect) == kResultOk);
        CHECK(rect.getWidth() == 320 * scale / 100 &&
              rect.getHeight() == 180 * scale / 100 + 28);
        int x = 232 * scale / 100, y = 90 * scale / 100 + 28;
        SendMessageW(surface, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        CHECK(GetCapture() == surface);
        SendMessageW(surface, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        pump(50);
        CHECK(std::abs(p.controller->getParamNormalized(0) - 0.75) < 0.004);
        CHECK(GetCapture() != surface);
        HDC painted = GetDC(surface);
        COLORREF fill =
            GetPixel(painted, 25 * scale / 100, 28 + 82 * scale / 100);
        ReleaseDC(surface, painted);
        CHECK(GetGValue(fill) > 150 && GetRValue(fill) < 60);
        if (scale == 150)
          screenshot(surface, "editor-150.bmp");
      }
      ViewRect before;
      view->getSize(&before);
      frame.reject = true;
      CHECK(SendMessageW(surface, WM_COMMAND, 100, 0) == 0);
      ViewRect after;
      view->getSize(&after);
      CHECK(before.getWidth() == after.getWidth());
      frame.reject = false;
      CHECK(SendMessageW(surface, WM_COMMAND, 100, 0) == 1);
      pump(30);
      SendMessageW(surface, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(160, 118));
      SendMessageW(surface, WM_MOUSEMOVE, MK_LBUTTON,
                   MAKELPARAM(short(-80), 118));
      SendMessageW(surface, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(400, 118));
      SendMessageW(surface, WM_LBUTTONUP, 0, MAKELPARAM(400, 118));
      pump(50);
      CHECK(p.controller->getParamNormalized(0) == 1);
      SendMessageW(surface, WM_KEYDOWN, VK_DOWN, 1);
      pump(50);
      CHECK(std::abs(p.controller->getParamNormalized(0) - 0.95) < 0.00001);
      POINT pt{160, 118};
      ClientToScreen(surface, &pt);
      SendMessageW(surface, WM_MOUSEWHEEL, MAKEWPARAM(0, short(-WHEEL_DELTA)),
                   MAKELPARAM(pt.x, pt.y));
      pump(50);
      CHECK(std::abs(p.controller->getParamNormalized(0) - 0.9) < 0.00001);
      CHECK(handler.values > 0 && handler.begins == handler.ends &&
            handler.values == handler.begins);
      // Inspect a known colored source pixel in the actual proxy window.
      HDC dc = GetDC(surface);
      COLORREF pixel = GetPixel(dc, 25, 110);
      ReleaseDC(surface, dc);
      std::cout << "Displayed fixture pixel R/G/B=" << int(GetRValue(pixel))
                << '/' << int(GetGValue(pixel)) << '/' << int(GetBValue(pixel))
                << '\n';
      CHECK(GetGValue(pixel) > 150 && GetRValue(pixel) < 60);
      std::cout << "PASS attached scaled editor: " << testedScales
                << " fitting zoom commands, screen-fit rejection, mapped "
                   "clicks, signed captured drag, keyboard/wheel, host edit "
                   "callbacks, real displayed pixels, resize rejection\n";
      std::atomic<bool> finished{false};
      int badAudio = 0, overruns = 0;
      std::vector<double> times;
      std::thread audio([&] {
        auto next = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
          Block block(1024);
          std::fill_n(block.in[0], 1024, 0.25f);
          std::fill_n(block.in[1], 1024, -0.5f);
          auto start = std::chrono::steady_clock::now();
          auto result = p.audio->process(block.data);
          auto end = std::chrono::steady_clock::now();
          double ms =
              std::chrono::duration<double, std::milli>(end - start).count();
          times.push_back(ms);
          if (ms > 1024. / 48.)
            ++overruns;
          if (result != kResultOk)
            ++badAudio;
          for (int j = 0; j < 1024; ++j)
            if (!(block.out[0][j] > 0) ||
                block.out[1][j] != -2 * block.out[0][j]) {
              ++badAudio;
              break;
            }
          next += std::chrono::microseconds(21333);
          std::this_thread::sleep_until(next);
        }
        finished = true;
      });
      int iteration = 0;
      while (!finished) {
        int x = 200 + (iteration++ % 80);
        SendMessageW(surface, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 118));
        SendMessageW(surface, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + 1, 118));
        SendMessageW(surface, WM_LBUTTONUP, 0, MAKELPARAM(x + 1, 118));
        pump(8);
      }
      audio.join();
      std::sort(times.begin(), times.end());
      std::cout << "MEASURED Wine/Windows API paced 48k/1024, 100 blocks + "
                   "scaled GUI: p95="
                << times[94] << "ms p99=" << times[98]
                << "ms max=" << times.back() << "ms overruns=" << overruns
                << " bad=" << badAudio << '\n';
      CHECK(badAudio == 0 && overruns == 0);
      view->removed();
      view->release();
      pump(50);
      for (int i = 0; i < 3; ++i) {
        view = p.controller->createView(ViewType::kEditor);
        CHECK(view);
        view->setFrame(&frame);
        CHECK(view->attached(window, kPlatformTypeHWND) == kResultOk);
        pump(30);
        view->removed();
        view->release();
        pump(40);
      }
      p.controller->setComponentHandler(nullptr);
      std::cout << "PASS editor close/reopen x3\n";
    }
    pump(100);
    CHECK(helperCount() == baseline);
    {
      Loaded p(argv[1]);
      auto *view = p.controller->createView(ViewType::kEditor);
      CHECK(view);
      view->setFrame(&frame);
      CHECK(view->attached(window, kPlatformTypeHWND) == kResultOk);
      pump(30);
      CHECK(p.component->terminate() == kResultOk);
      pump(20);
      CHECK(view->removed() == kResultOk);
      view->release();
    }
    CHECK(helperCount() == baseline);
    std::cout << "PASS module reload and view lifetime beyond component "
                 "termination\n";
    for (float fault : {1.f, 0.5f}) {
      Loaded p(argv[1]);
      p.start();
      auto start = GetTickCount64();
      automate(p, fault, 1);
      CHECK(GetTickCount64() - start < 100);
      Block b;
      std::fill_n(b.in[0], 64, 1.f);
      b.render(p);
      for (float v : b.out[0])
        CHECK(v == 0); // Disconnected, no poison replay.
    }
    pump(100);
    CHECK(helperCount() == baseline);
    DestroyWindow(window);
    std::cout << "PASS helper crash/hang -> bounded disconnected silence; "
                 "clean process teardown, no helper leaks\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
