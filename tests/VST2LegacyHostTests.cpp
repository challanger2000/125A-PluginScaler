#include "pluginscaler/host/VST2LegacyHost.h"

#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

using pluginscaler::host::VST2LegacyHost;

int wmain(int argc, wchar_t** argv) {
    if (argc != 2)
        return 1;

    VST2LegacyHost host;
    std::string error;

    if (!host.start(error)) {
        std::cerr << "host-start=FAIL " << error << "\n";
        return 2;
    }
    std::cout << "host-start=PASS\n";

    if (!host.openPlugin(argv[1], 48000.0, 64, error)) {
        std::cerr << "plugin-open=FAIL " << error << "\n";
        return 3;
    }
    std::cout << "plugin-open=PASS\n";

    Sleep(160);
    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x1261) != 1) {
        std::cerr << "legacy-idle=FAIL\n";
        return 4;
    }
    Sleep(100);
    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x1261) != 1) {
        std::cerr << "legacy-idle-stop=FAIL\n";
        return 4;
    }
    std::cout << "legacy-idle=PASS\n";

    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125E) != 1) {
        std::cerr << "start-process-lifecycle=FAIL\n";
        return 4;
    }
    std::cout << "start-process-lifecycle=PASS\n";

    if (!host.reconfigure(44100.0, 128) ||
        host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125F) != 1) {
        std::cerr << "reconfigure-lifecycle=FAIL\n";
        return 5;
    }
    std::cout << "reconfigure-lifecycle=PASS\n";

    if (!host.reconfigure(48000.0, 64)) {
        std::cerr << "reconfigure-restore=FAIL\n";
        return 6;
    }

    pluginscaler::formats::vst2abi::VstTimeInfo timeInfo{};
    timeInfo.sampleRate = 48000.0;
    timeInfo.ppqPos = 12.5;
    timeInfo.tempo = 123.0;
    timeInfo.timeSigNumerator = 4;
    timeInfo.timeSigDenominator = 4;
    timeInfo.flags =
        pluginscaler::formats::vst2abi::VstTransportPlaying |
        pluginscaler::formats::vst2abi::VstPpqPosValid |
        pluginscaler::formats::vst2abi::VstTempoValid |
        pluginscaler::formats::vst2abi::VstTimeSigValid;
    host.setHostTimeInfo(timeInfo);

    std::vector<float> left(64, -1.0f);
    std::vector<float> right(64, -1.0f);
    float* outputs[2]{left.data(), right.data()};

    if (!host.processReplacing(nullptr, outputs, 64)) {
        std::cerr << "audio=FAIL\n";
        return 4;
    }
    std::cout << "audio=PASS\n";

    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x1260) != 1) {
        std::cerr << "process-level=FAIL\n";
        return 7;
    }
    std::cout << "process-level=PASS\n";

    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125A) != 1) {
        std::cerr << "time-info=FAIL\n";
        return 5;
    }
    std::cout << "time-info=PASS\n";

    (void)host.dispatchOnMainThread(
        pluginscaler::formats::vst2abi::EffSetProgram, 0, 2);
    if (host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125B) != 1) {
        std::cerr << "program-lifecycle=FAIL\n";
        return 6;
    }
    std::cout << "program-lifecycle=PASS\n";

    std::vector<std::uint8_t> chunk;
    if (!host.getChunkOnMainThread(0, chunk) || chunk.empty() ||
        !host.setChunkOnMainThread(0, chunk.data(), chunk.size()) ||
        host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125C) != 1) {
        std::cerr << "bank-chunk-lifecycle=FAIL\n";
        return 7;
    }
    std::cout << "bank-chunk-lifecycle=PASS\n";

    if (!host.setChunkOnMainThread(1, chunk.data(), chunk.size()) ||
        host.dispatchOnMainThread(
            pluginscaler::formats::vst2abi::EffVendorSpecific,
            0x125D) != 1) {
        std::cerr << "program-chunk-lifecycle=FAIL\n";
        return 8;
    }
    std::cout << "program-chunk-lifecycle=PASS\n";

    if (!host.openEditor(error)) {
        std::cerr << "editor-open=FAIL " << error << "\n";
        return 9;
    }
    std::cout << "editor-open=PASS\n";

    HWND editorHost = host.editorHostWindow();
    if (!editorHost || !IsWindow(editorHost)) {
        std::cerr << "editor-host=FAIL\n";
        return 10;
    }
    std::cout << "editor-host=PASS\n";

    HWND child = GetWindow(editorHost, GW_CHILD);
    if (!child || !IsWindow(child)) {
        std::cerr << "editor-child=FAIL\n";
        return 11;
    }
    std::cout << "editor-child=PASS\n";

    Sleep(100);

    RECT resized{};
    if (!GetClientRect(editorHost, &resized) ||
        resized.right - resized.left != 400 ||
        resized.bottom - resized.top != 220) {
        std::cerr << "editor-resize=FAIL "
                  << (resized.right - resized.left) << "x"
                  << (resized.bottom - resized.top) << "\n";
        return 12;
    }
    std::cout << "editor-resize=PASS\n";

    if (!host.closeEditor()) {
        std::cerr << "editor-close=FAIL\n";
        return 13;
    }
    std::cout << "editor-close=PASS\n";

    if (!host.closePlugin()) {
        std::cerr << "plugin-close=FAIL\n";
        return 14;
    }
    std::cout << "plugin-close=PASS\n";

    host.stop();
    std::cout << "host-stop=PASS\n";
    std::cout << "legacy-host=PASS\n";
    return 0;
}
