#include "pluginscaler/ipc/Protocol.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Bootstrap only: real helper lifecycle/IPC comes next.
    return pluginscaler::ipc::kProtocolMajor == 1 ? 0 : 1;
}
