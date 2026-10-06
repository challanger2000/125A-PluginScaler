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
