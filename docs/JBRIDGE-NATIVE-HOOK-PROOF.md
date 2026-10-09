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


## Follow-up: attaching to an already-open editor and restoring it (2026-10-09)

**Confirmed by Windows x86 test:** https://github.com/challanger2000/125A-PluginScaler/actions/runs/37986216686

The parent launches **two separate 32-bit GUI processes**, each loading an unmodified standalone renderer DLL. Each child creates and paints its 100% editor HWND **before** the controller installs any hook.

The controller learns the native HWND/TID, creates a per-target command mapping named by target PID and GUI TID, installs a thread-targeted \`WH_GETMESSAGE\` hook, and posts \`WM_NULL\` to the existing window. The hook executes in the target GUI thread, validates the HWND's ownership and class, identifies the renderer image through the window procedure, patches its \`BeginPaint\` import, resizes the **real HWND** and invalidates it. No launch-time environment variable is required for the injected attach path. (The test mock separately uses an environment variable for result reporting.)

**Observed test output:**
\`\`\`
in-process-already-open-gdi-150=PASS ... attached=1 ... pixels=1 down=1 move=1 up=1 mismatch=0
already-open-detach-restore-150=PASS status=2 originalPixels=1 originalMouse=1 down=2 up=2 mismatch=0
in-process-already-open-gdi-200=PASS ... attached=1 ... pixels=1 down=1 move=1 up=1 mismatch=0
already-open-detach-restore-200=PASS status=2 originalPixels=1 originalMouse=1 down=2 up=2 mismatch=0
already-open-injected-gdi-150-200-native-mouse=PASS
\`\`\`

**Crucial process-safety step:** Before the controller unhooks, a control message restores the original renderer DLL's imported \`BeginPaint\` pointer, resets its window to the original client size, and repaints. Subsequent native 100% mouse clicks were tested **while the original GUI process remained running**. This prevents a known risk of leaving an import pointer into an unloaded hook DLL.

**Strict scope:** This is demonstrated with a deliberately simple x86 GDI renderer DLL and 32-bit mock host processes only. jBridge \`auxhost.exe\` itself, the real Pro-53 and FM7 GUI classes, live editor ownership, \`BitBlt\`/\`GetDC\` drawing modes, scroll wheels, right clicks, popups and other plugin-specific constraints remain untested. Current attach implementation deliberately accepts **only** the mock's editor class; it is not an end-user jBridge executable. A real editor must never be patched based solely on its process name without confirming its rendering path and stable teardown behavior. 


## Pro-53 native renderer fingerprint and DIB proof (2026-10-09)

The **original Pro-53.dll supplied by the user in their Library**, SHA-256
\`bc86da0cd9528368ed1616e495bff26f71f8dec1cceba7993b390ac6cc49fa10\`,
was analyzed **offline and locally only** using PE import inspection. Neither
the proprietary binary nor any of its bytes were added to this repository.

Observed 32-bit PE import entries of direct relevance:
- \`USER32.dll\`: \`BeginPaint\`, \`EndPaint\`, \`GetDC\`, \`ReleaseDC\`, \`ScreenToClient\`, \`SetCapture\`, \`ReleaseCapture\`, \`CreateWindowExA\`, \`RegisterClassA\`, \`SetWindowsHookExA\`.
- \`GDI32.dll\`: \`SetDIBitsToDevice\`, \`CreateCompatibleDC\`, \`DeleteDC\`, \`SelectObject\`, \`GetClipBox\`, \`CreatePen\`, \`CreateSolidBrush\`, \`GetStockObject\`. There are no imported \`BitBlt\`/\`StretchBlt\` entrypoints in this specific binary's static IAT.

**Critical Windows semantics:** \`SetDIBitsToDevice\` doesn't stretch DIB data just because the destination DC maps logical coordinates to a larger viewport; actual bitmap scaling needs an adapted \`StretchDIBits\` path (for supported full-frame DIBs). This is separate from scaling ordinary GDI rectangles.

New experimental 32-bit in-process native hook modifies imports in the original editor's renderer module: \`BeginPaint\`, \`GetDC\`, \`SetDIBitsToDevice\`. For a complete uncompressed RGB/bitfields DIB, the \`SetDIBitsToDevice\` hook dispatches to \`StretchDIBits\` on the already transformed DC. Other/banded/compressed DIB modes fall back unchanged rather than guessing. The render and input hooks can be undone while the GUI process is still running.

**Windows CI proof** (two already-open, independent 32-bit GUI hosts using a separate mock renderer DLL): https://github.com/challanger2000/125A-PluginScaler/actions/runs/38001261322

\`\`\`
in-process-already-open-gdi-150=PASS ... pixels=1 ... mismatch=0
DIB_IMPORTS zoom=150 begin=1 dib=1
already-open-detach-restore-150=PASS ... originalPixels=1 originalMouse=1
in-process-already-open-gdi-200=PASS ... pixels=1 ... mismatch=0
DIB_IMPORTS zoom=200 begin=1 dib=1
already-open-detach-restore-200=PASS ... originalPixels=1 originalMouse=1
already-open-injected-dib-gdi-150-200-native-mouse=PASS
\`\`\`

**NOT TESTED:** actual Pro-53 plugin execution in real jBridge/Studio One. A matching import table by itself is not proof of runtime call paths or correct editor lifecycle. Before release: real jBridge HWND discovery with precise target validation; per-instance state within shared auxhost; tooltip/popup/right-click/wheel; real renderer image orientation/partial updates; robust failure rollback and unhook; single-EXE packaging; real Pro-53 final acceptance.
