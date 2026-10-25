# 125A PluginScaler – jBridge release gate (2026-10-09)

## Audit result: NOT RELEASE READY

The standalone jBridge viewer can launch and its Windows mock tests pass, but that is **not** proof of reliable scaling for Pro-53, FM7, or arbitrary legacy editors. Do not label current builds release-ready.

### Confirmed from source inspection

1. **Rendering:** `MagSetWindowSource` samples a screen rectangle, not a bound HWND. Filtering some overlapping top-level windows cannot guarantee pixel isolation for DAW child windows, popups, occlusion, minimized editors, independent compositor surfaces or multiple overlapping editors.
2. **Input:** Mouse gestures are forwarded via `PostMessageW`. Editors that query real cursor coordinates, native capture, raw input, or process-local mouse state need actual input semantics. A passing mocked `WM_LBUTTONDOWN/MOVE/UP` handler does not prove this.
3. **Plugin discovery:** Enumeration includes visible auxhost windows above broad size thresholds and does not distinguish editor HWND from host container/utility windows reliably. Selection dialog is a provisional workaround, not automatic instance management.
4. **Concurrency:** Per-process globals are isolated across separate viewer EXEs, but there is no single manager that discovers, tracks and independently scales an arbitrary number of instances. HWND/PID/TID binding only partially addresses lifecycle.
5. **Tests:** Current Windows runner exercises mock GUI windows, scaled pixel sampling and synthetic `SendInput`. It does not load real 32-bit VST2 binaries under jBridge, prove real editor capture, test true plugin-native input/capture, or verify independent concurrent *viewers* across multiple plugins. CI green means only these bounded assertions passed.
6. **Start/import problems:** Earlier ComCtl32 entry-point failure demonstrated that build success alone does not prove Windows compatibility. Ensure EXE import verification and startup smoke checks.

### Required go/no-go gates

- Deterministic window-to-editor identification; independent instances; close/reopen/reuse and DAW embedded HWND verified.
- Pixel-perfect GUI at 150/200% including overlap, offscreen behavior, GUI updates, preset switches, and mixed DPI.
- Genuine click, drag, right-click, wheel, keyboard focus, tooltip/popups and host automation with no focus theft.
- No hang/crash or interference with other plugin instances, audio, MIDI, host process.
- Automated fixtures testing actual scaler end-to-end, plus final hands-on original plugin acceptance in Studio One.

### Engineering decision

**Current Magnification + synthetic posted mouse messages is a prototype, not a proven universal solution.** Do not keep patching it toward a universal compatibility claim without demonstrating an alternative native input/capture path on representative legacy GUI behaviors. Build/test jobs should be run against meaningful acceptance gates, not for trivial refactors. Pro-53 and FM7 acceptance ultimately requires their original binaries and user-side validation.

## Alternative architecture candidate: per-window Windows Graphics Capture

Microsoft-supported `IGraphicsCaptureItemInterop::CreateForWindow(HWND,...)` captures a particular window (Windows 10 1903+), unlike Magnification's desktop rectangle. Evaluate as the primary rendered-frame source, not as an automatic mouse fix. GPU D3D11/WinRT frame pool required; child HWND capture must be validated rather than assumed. Cropping, source resize, occlusion, DPI and secure/protected content require tests.

**Input acceptance remains independent:** test against a mock editor using `GetCursorPos`, `SetCapture`, keyboard focus and native hit testing. Passing posted `WM_MOUSE*` messages is explicitly insufficient. MagSetInputTransform is not a general substitute: Microsoft documents UIAccess privileges and screen-wide transformation semantics.

**Native DPI alternative:** test only when we control the 32-bit editor process *before window creation*; do not promise retroactive change to jBridge's already-running auxhost, and do not modify or redistribute jBridge binaries.

### Decision gates before building a new product path

1. Minimal WGC HWND frame acquisition test (single legacy-mock editor), including real occlusion by contrasting overlay and content changing after resize.
2. Separate mouse drag/capture test with a mock that explicitly compares real cursor coordinates against dispatched messages. Failure is expected with current PostMessage-only forwarding.
3. If window capture is successful, prototype a native-input strategy with measured behavior (not a mock that only accepts posted clicks). Maintain independent instance contexts.
4. If no reliable input strategy emerges under the single-EXE, already-open-jBridge constraint, mark that deployment mode unsupported rather than repeatedly declaring CI success.

Sources: https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow and https://learn.microsoft.com/en-us/windows/win32/api/magnification/nf-magnification-magsetinputtransform
