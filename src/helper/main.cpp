#include "pluginscaler/ipc/Protocol.h"

int main() {
    // Bootstrap only: real helper lifecycle/IPC comes next.
    return pluginscaler::ipc::kProtocolMajor == 1 ? 0 : 1;
}
