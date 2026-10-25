# jBridge GUI Scaler path

## Goal

Use jBridge only for the already-working 32-bit/64-bit plug-in bridge and let 125A handle GUI scaling separately.

The 125A component must remain independently implemented and must not patch, replace or redistribute jBridge binaries.

## Architecture boundary

jBridge remains responsible for:
- VST2 hosting/bridging
- audio
- MIDI
- automation
- program/state transport
- legacy plug-in lifecycle

125A is responsible only for:
- discovering the real legacy editor window
- rendering/scaling that editor
- translating input coordinates
- focus/capture behavior required by the scaled surface

## First measurement gate

Before selecting a rendering strategy, determine the actual jBridge window topology used by the real targets.

The probe executable:
- finds running `auxhost.exe` / `auxhost64.exe`
- enumerates their top-level and child HWNDs
- records parent/owner/root ownership
- records class/title/visibility/geometry
- identifies whether the visible editor is a top-level auxhost window or embedded under another process

This matters because cross-process scaling options differ materially for a top-level source versus a child window embedded into the DAW.

No scaling implementation is accepted before this topology is measured with real FM7 and Pro-53.

## Architecture finding: native jBridge separated GUI (2026-10-09)

The official jBridge author documents a separated-GUI mode controlled by `USE_SEPARATED_GUI 1` in `default_auxhost_settings.txt` (global) or the individual `<plugin>.jBridge` settings. Source: https://jstuff.wordpress.com/2009/09/08/jbridge-1-1-beta4-released/ . This is a **real jBridge setting**, not a proof that the GUI is DPI-scalable; verify against the installed version and real editor topology before enabling in production. Back up per-plugin settings; never silently modify them or jBridge binaries.

### Decision table for manipulating jBridge's original HWND

| Operation | What it changes | Can make fixed-pixel plugin artwork 150%? | Risk / acceptance criterion |
| --- | --- | --- | --- |
| `SetWindowPos` or `MoveWindow` | Frame/client dimensions and coordinates | No automatic bitmap enlargement | Can expose empty margins or trigger legacy GUI redraw/size bugs. |
| `SetParent` / style changes | Ownership and HWND tree | No | Cross-process DPI-awareness mismatch may forcibly reset child's process awareness; focus, popups and host lifecycle risk. Do not change parent before original HWND hierarchy is measured. |
| Separated jBridge GUI | jBridge-managed presentation topology | Not by itself | Promising isolation of GUI from host embedding; requires measurement of the actual source HWND, plugin surface size and DPI mode. |
| Pre-creation DPI-unaware context at >100% desktop scale | Windows can bitmap-stretch original DPI-unaware top-level window | Yes in compatible configurations, not arbitrary per-HWND zoom | Existing auxhost DPI manifest/context, actual monitor DPI and timing determine result. No promise of independent 150% and 200% in one monitor. |
| DWM/window capture with 1.5x display | Clone display without altering original | Yes, if capture supports HWND | Does not preserve native mouse semantics; requires independent solution. |

Microsoft references: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent and https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows .

**Next gate before implementation:** obtain a **non-invasive HWND topology comparison** for the same original jBridge plugin in integrated versus separated-GUI mode. Measure window owner/parent/root, process/TID, DPI context, class, styles, client dimensions and visible content bounds. Only then decide whether a safe DPI-aware original-window method exists; avoid another viewer patch and avoid changing the user's jBridge settings automatically.
