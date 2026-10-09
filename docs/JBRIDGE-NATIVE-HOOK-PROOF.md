# jBridge native HWND hook: verified feasibility (2026-10-09)

## Proven, not inferred

Windows CI run https://github.com/challanger2000/125A-PluginScaler/actions/runs/37977919537 confirms an x86 **thread-targeted WH_CBT hook DLL** was loaded and invoked **inside each of two different 32-bit editor processes**, without patching their EXEs or proxying their mouse input:

- Editor A: `hook-matched-editor=PASS pid=7736 callbackPid=7736 events=7 matched=1 hwnd=196788 expected=196788`
- Editor B: `hook-matched-editor=PASS pid=1512 callbackPid=1512 events=6 matched=1 hwnd=524334 expected=524334`
- `two-independent-32bit-hooked-editors=PASS`

The two independent target processes explicitly create real HWNDs after their GUI threads establish message queues. The controller installs a **WH_CBT hook on each specific GUI thread**, never as a system-wide/global hook. The injected DLL observes `HCBT_CREATEWND`, identifies the actual editor window by its class name, and records the invoking target PID/TID and the native HWND in a separate per-target shared mapping. Both are checked against values reported independently by the target processes.

Code: `src/jbridge_hook/{cbt_hook.cpp,hook_smoke.cpp,smoke_shared.h,CMakeLists.txt}`.

## What this demonstrates

- Attaching a same-bitness DLL to an original 32-bit window process **is technically achievable** on the test system via supported Windows hooks.
- The code can execute in the original GUI thread/process and identify its real HWND.
- Multiple independent x86 GUI instances can be hooked and distinguished without cross-talk.

## What it does not demonstrate

- That `auxhost.exe`/jBridge exposes every needed editor HWND or permits the hook under the user's Windows integrity level / process constraints.
- That a `WH_CBT` hook alone scales fixed-pixel bitmap or GDI output (it does **not**).
- That jBridge's separated GUI mode supports arbitrary 150/200% content zoom; that native mouse/capture or VST audio/MIDI remains stable under changes.
- That automatic attachment to an *already created* native editor suffices; `HCBT_CREATEWND` must be installed **before** that window is created. Existing HWNDs require discovery and separate handling.

## Architectural guardrails

1. Hook only the selected GUI TID, after confirming a permitted process name, bitness, PID/TID, HWND class and ownership. Never install a global hook.
2. Keep the hook lightweight, reentrancy-safe and passive until actual editor behavior is measured. Support clean detach on close/crash.
3. Use a 32-bit helper + 32-bit DLL for x86 auxhost and separate 64-bit binaries for x64, if needed.
4. Test real jBridge separated GUI with the original Pro-53/FM7 only as the final user acceptance gate.
5. Before adding rendering code, define **one test** that verifies enlarged original pixels at 150% and fully native mouse-capture. No build SUCCESS without these criteria is a release PASS.

Microsoft primary reference: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw

**Decision:** Keep the old Magnifier clone unmodified. Targeted in-process hooks are a viable access mechanism, **not yet a scaling solution**.

## Update: Real native GDI scaling inside hooked x86 renderer DLL — PASS

CI: https://github.com/challanger2000/125A-PluginScaler/actions/runs/37980741128

Two separate 32-bit test hosts loaded the **same unmodified GDI renderer DLL**. The 125A hook DLL was installed on the original editor threads, recognized the editor during `HCBT_CREATEWND`, resolved its registered window-procedure image using `GetClassInfoExW` and `VirtualQuery`, and modified that module's `BeginPaint` import to configure a 150% or 200% GDI transform. A separate `WH_GETMESSAGE` hook translated native queued client mouse coordinates into the original logical coordinates. The actual original HWNDs, including native `SetCapture` behavior, remained in use; no clone or external bitmap forwarding was involved.

CI observed:

```
in-process-unmodified-gdi-150=PASS hwnd=262578 hook=1 iat=1 pixels=1 down=1 move=1 up=1 mismatch=0
in-process-unmodified-gdi-200=PASS hwnd=262524 hook=1 iat=1 pixels=1 down=1 move=1 up=1 mismatch=0
injected-gdi-150-200-native-mouse=PASS
```

Code: `src/jbridge_hook/native_scale_hook.cpp`, `native_scale_hook_smoke.cpp`, `mock_legacy_renderer.cpp`, `native_scale_shared.h`. The renderer itself contains no scaling logic; painting occurs in its own DLL rather than the host EXE. Independent original HWNDs and processes were exercised at two different scaling factors.

### Limits still blocking release

- **No original jBridge/Pro-53/FM7 testing yet.** The renderer and window-class identification are mock-specific; attaching to an already-open editor is also not proved. This design currently intercepts creation-time `HCBT_CREATEWND`.
- **Rendering coverage is narrow:** GDI `BeginPaint` imported by the module containing the window procedure. `GetDC`, `SetDIBitsToDevice`, DirectDraw/OpenGL, other DLLs and custom renderer paths remain unsupported and need identification and separate proofs.
- **Native mouse input is only partly covered:** `WM_LBUTTONDOWN/MOVE/UP` delivered through the Windows queue works. Direct `GetCursorPos`, raw input, wheel, right-click, keyboard focus, popups, mouse capture edge cases and mixed DPI remain unproved.
- Robust unload, multiple editor windows in one process, x64 coordination, jBridge process discovery and real DAW audio/MIDI stability remain untested.

**Decision:** Positive proof for 32-bit GDI bitmap/control renderers via an in-process native hook, **not a universal product release**. Do not modify or redistribute jBridge binaries, and do not call the existing Magnifier clone finished.
