# 125A Legacy VST2 Host Core

Status: rewrite branch `rewrite/vst2-host-core`.

## Purpose

The x86 side is a small, conservative 32-bit VST2 host. It is not a collection
of bridge callbacks. Legacy plugins such as FM7 and Pro-53 should see behaviour
that resembles a normal native 32-bit VST2 DAW.

GUI scaling is explicitly out of scope until the 100% host path is stable.

## Non-negotiable architecture

1. The x86 host owns plugin lifetime.
2. Entry point, `effOpen`, sample-rate/block-size configuration, mains,
   program/bank/state lifecycle, editor open/close/idle and `effClose` stay
   local to x86 and are marshalled to the owner/GUI thread where required.
3. The editor receives a same-process host HWND at `effEditOpen`.
4. The plugin editor HWND is never reparented after `effEditOpen`.
5. If the editor must later be embedded in a 64-bit DAW, only a bridge-owned
   container may cross the process boundary; the plugin's own HWND hierarchy
   remains untouched.
6. The x86 host owns a real Win32 message loop and calls `effEditIdle` while
   the editor exists.
7. Audio processing does not use the GUI/control message queue.
8. Audio-thread host callbacks must never wait for the x64 DAW.
9. Program/bank/chunk calls and other legacy-sensitive dispatcher operations
   remain local to x86 and are routed to the owner thread.
10. IPC is transport. It is not the VST2 host implementation.

## Compatibility gate before scaling

At 100% the host must prove:

- load / entry / `effOpen` / `effClose`;
- sample rate and variable block size;
- repeated mains on/off;
- float `processReplacing`;
- zero-input instruments;
- VST MIDI input, note-off and deltaFrames;
- `audioMasterGetTime` requested-flag semantics;
- parameters and automation gestures;
- programs, banks and chunks;
- state save/restore;
- native editor open/close/reopen;
- Win32 message pumping and `effEditIdle`;
- plugin-initiated size/update notifications;
- clean shutdown with editor open;
- no helper leak;
- FM7 and Pro-53 runtime regression in Studio One.

Only after this matrix is stable may scaling return.

## Evidence policy

The historical Steinberg VST2.4 ABI is the compatibility baseline. Maintained
or inspectable host implementations such as JUCE, Carla and yabridge may be
used for behavioural cross-checking where licensing permits. jBridge is a
runtime behavioural reference only; its proprietary implementation is not
copied.

Every confirmed FM7/Pro-53 failure should become a deterministic mock or an
explicit runtime QA case before the corresponding fix is considered complete.
