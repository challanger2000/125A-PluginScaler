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


## Multi-instance and vendor-neutral discovery

The scaler must not identify plug-ins by vendor-specific window classes. Pro-53/FM7 are reference targets only.

Current jBridge viewer rules:
- discover windows by jBridge auxhost process ownership/topology, not NI class names
- group editor candidates by their compact editor/wrapper hierarchy
- create one viewer process per discovered editor source
- keep each viewer bound to its original HWND; never switch to "the largest window"
- if one source disappears, only that viewer closes
- if an editor is recreated, the manager may attach a new viewer to the new HWND
- manually closing a viewer restores that original editor and suppresses automatic respawn until that source HWND disappears
- host-driven source loss must not restore an obsolete wrapper onscreen
- multiple simultaneous plug-in instances are expected and must remain independent

### Required real-host lifecycle matrix

Before calling the jBridge GUI scaler runtime-ready, exercise at least:
1. open one plug-in -> interact
2. minimize/hide -> restore
3. switch track/instance
4. open a second bridged plug-in from the same or another vendor
5. interact with both viewers independently
6. close one editor while the other remains open
7. reopen the closed editor
8. remove one plug-in instance
9. close project / close DAW
10. verify all viewer/helper processes terminate or remain resident only by explicit product design

Build success is not runtime PASS for this matrix.
