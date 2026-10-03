# 125A PluginScaler — START HERE

## Project goal

Build one Windows product that can host and scale small/legacy plugin GUIs while keeping the third-party plugin isolated from the DAW process wherever practical.

Target scope:
- VST2 32-bit
- VST2 64-bit
- VST3 64-bit
- own 32/64-bit bridge; no jBridge dependency
- scalable GUI
- crash isolation through helper processes
- per-plugin compatibility profiles and fallbacks

## Non-negotiable architecture rules

1. The DAW-facing proxy must remain as small and stable as possible.
2. Third-party plugins should run out-of-process in a helper when isolation is enabled or required.
3. 32-bit plugins must never be loaded into a 64-bit DAW process.
4. GUI scaling, plugin-format adaptation, IPC, and crash recovery are separate layers.
5. No single GUI rendering strategy is assumed to work for every plugin.
6. Compatibility behavior is stored per plugin.
7. Audio-thread code must not block on GUI or control IPC.
8. A helper crash must degrade to silence / disconnected state instead of taking down the DAW where technically possible.
9. State restore/restart must be designed into the protocol from the beginning.
10. The Pro-53 work in 125A-Engineering is reference material only; this repository is the clean product implementation.

## Initial compatibility modes

GUI:
- Auto
- Direct/integrated
- Offscreen capture
- Separate helper rendering

Refresh:
- Auto
- Normal
- Full refresh
- Aggressive refresh

Input:
- Auto
- Absolute/scaled
- Native drag delta
- Relative mouse

Process:
- isolated helper per instance
- shared helper where proven safe

## Development rules

- Keep main releasable.
- Work on feature branches and merge through PRs.
- Do not claim runtime PASS without test evidence.
- Build success is not runtime success.
- Preserve 32-bit and 64-bit test coverage.
- Prefer deterministic diagnostics over guesswork.
- Start with VST2 reference hosting and the bridge protocol, then add VST3 adapter support.
