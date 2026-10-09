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
