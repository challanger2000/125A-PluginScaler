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
