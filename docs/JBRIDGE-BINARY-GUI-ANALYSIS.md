# jBridge native GUI implementation: static binary analysis (2026-10-09)

## Evidence and scope
Analyzed user-provided **JBridge(3).zip** statically. No jBridge binary was executed or modified.

| File | Architecture | SHA-256 |
|---|---|---|
| `auxhost.exe` | x86 PE32 (2016-12-08) | `9c066687a0b3632d7414cf98609b8c3416fa29ed61f51e32de3b8e7b7b6c0092` |
| `auxhost64.exe` | x64 PE32+ (2016-12-08) | `c3bc45eabb25bc0e09cffe384f79560782f738bc730acb4cb89d372d1e9a57ab` |
| `Bridger32.dll`, `Bridger64.dll` | corresponding native architectures | contain `SetParent` imports and verified call sites |

## Findings directly demonstrated by disassembly

1. **GUI mode is an actual configurable jBridge feature.** ASCII configuration key `USE_SEPARATED_GUI` occurs in `auxhost.exe` at file offset `0x32db4` (VA `0x4341b4`), referenced in parsing code at VA `0x4055b5`. Its own embedded help text explicitly describes opening the plugin GUI in a separate window.
2. **auxhost32 actually calls `SetParent`, not merely imports it.** At VA `0x405030` there is `call dword ptr [0x43120c]`, the import of `USER32!SetParent`. The child HWND is read from the auxhost/editor state (`[edi+0x440]`), and a parent HWND from global host window state (`[global+0xc]`). This call is conditional (including plugin-type checks and a mode byte at offset `0x3f82`). The precise semantic mapping of that byte requires further proof; do not label it as the separate-mode setting without tracing the parser storage.
3. **auxhost64 has the corresponding mechanism.** At VA `0x140006110` it calls `USER32!SetParent` through import address `0x140038420`. This is likewise conditional, with a mode byte at `+0x3f82`.
4. **Both Bridger DLLs also reparent GUIs.** `Bridger32.dll` calls `SetParent` at VA `0x1003505a` (IAT `0x1003c284`). `Bridger64.dll` calls it at VAs `0x180046efa` and `0x180047004` (IAT `0x18004c510`). The x64 implementation branches between using the existing window and creating a container window, then reparents the GUI into the chosen parent.
5. **Window positioning/topmost is separate from GUI reparenting.** `auxhost.exe` calls `SetWindowPos` at `0x40c2fe`, `0x40c31c`, `0x40c35e`. The code conditionally supplies `HWND_TOPMOST` (`-1`) or `HWND_NOTOPMOST` (`-2`), controlled by byte `+0x3f84`. This is a z-order operation, **not a pixel-scale transform**.
6. **No direct DPI-scaling primitive was identified in these import tables.** In particular, the above window management uses `SetParent`, `SetWindowPos`, `CreateWindowExA`, `GetWindowRect`, and message APIs. This does **not** exclude dynamically resolved APIs, but is no evidence of built-in 150/200% content scaling.

## Mechanism most strongly supported by the binary evidence

jBridge presents the **actual original plugin GUI HWND**, reparenting it into either a bridge/host-managed container or another presentation window depending on configuration and plugin behavior. It does not appear to implement its separate GUI mode by capturing the editor into a bitmap and forwarding mouse clicks into a clone. Native interaction is preserved because the real plugin window receives native Windows messages and retains its own input semantics.

### Crucial consequence for PluginScaler
`SetParent`, `CreateWindowEx`, `SetWindowPos`, `MoveWindow`, and style changes do **not** scale an old fixed-resolution plugin's internally drawn pixels. We can learn how jBridge preserves native input, but we cannot obtain 150% content zoom by copying its parent/owner changes alone.

**Not proven:** which specific on-disk setting controls each internal conditional; exact HWND topology for real Pro-53/FM7; 150/200% scaling compatibility; DPI virtualization of the original auxhost/plugin window; independent per-plugin scaling.

## Next technical gate: original-window scaling, not clone

- Compare the same editor HWND before/after jBridge's built-in separated-GUI switch: HWND, PID/TID, WS_CHILD/WS_POPUP, parent/owner/root, DPI context, client size and cursor/capture behavior.
- Determine whether Windows per-monitor DPI virtualization can operate **before the original editor window is created** and whether this alters the real pixel bounds. Do not confuse changing DPI awareness with setting arbitrary per-window DPI.
- If content scaling cannot be made native, do not claim the separate-GUI mode alone solves zoom. The fallback window-bound capture path requires an independently validated input solution.

## Testing policy
Static disassembly proves call sites and control flow, **not actual behavior on the user's machine**. This note is a reverse-engineering finding, **not release approval**. Do not modify the user-supplied jBridge binaries, patch instructions, or redistribute them.
