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

    std::vector<float> left(64, -1.0f);
    std::vector<float> right(64, -1.0f);
    float* outputs[2]{left.data(), right.data()};

    if (!host.processReplacing(nullptr, outputs, 64)) {
        std::cerr << "audio=FAIL\n";
        return 4;
    }
    std::cout << "audio=PASS\n";

    if (!host.openEditor(error)) {
        std::cerr << "editor-open=FAIL " << error << "\n";
        return 5;
    }
    std::cout << "editor-open=PASS\n";

    HWND editorHost = host.editorHostWindow();
    if (!editorHost || !IsWindow(editorHost)) {
        std::cerr << "editor-host=FAIL\n";
        return 6;
    }
    std::cout << "editor-host=PASS\n";

    HWND child = GetWindow(editorHost, GW_CHILD);
    if (!child || !IsWindow(child)) {
        std::cerr << "editor-child=FAIL\n";
        return 7;
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
        return 8;
    }
    std::cout << "editor-resize=PASS\n";

    if (!host.closeEditor()) {
        std::cerr << "editor-close=FAIL\n";
        return 9;
    }
    std::cout << "editor-close=PASS\n";

    if (!host.closePlugin()) {
        std::cerr << "plugin-close=FAIL\n";
        return 10;
    }
    std::cout << "plugin-close=PASS\n";

    host.stop();
    std::cout << "host-stop=PASS\n";
    std::cout << "legacy-host=PASS\n";
    return 0;
}
