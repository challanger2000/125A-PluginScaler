# 125A PluginScaler architecture

## Phase 1 priority: 32-bit VST2

The first functional milestone is replacing the essential bridge/hosting path for a 32-bit VST2 plug-in:

64-bit DAW proxy -> IPC -> x86 helper -> 32-bit VST2

The x86 helper owns the third-party DLL, AEffect instance and editor. The DAW process must never load a 32-bit plug-in.

The initial probe milestone deliberately validates module loading and the VST2 open/close lifecycle before real-time audio transport is introduced.

## Process model

- 64-bit DAW -> 64-bit proxy -> x64 helper -> 64-bit VST2/VST3
- 64-bit DAW -> 64-bit proxy -> x86 helper -> 32-bit VST2
- GUI transport/scaling is independent from audio/MIDI/control IPC.

A helper failure must be treated as a recoverable remote endpoint failure. The proxy must never dereference helper-owned memory.

## Layers

1. **Proxy**
   - host-facing lifetime
   - parameter/state facade
   - fail-safe silence/disconnected behavior

2. **IPC**
   - versioned protocol
   - control/state messages
   - heartbeat/liveness
   - later: lock-free/shared-memory real-time audio transport

3. **Helper**
   - native-bitness process
   - owns third-party plugin module
   - owns plugin editor window

4. **Format adapters**
   - VST3 adapter uses pinned official Steinberg VST3 SDK
   - VST2 adapter uses a minimal independently maintained interoperability ABI
   - no legacy Steinberg VST2 SDK headers are redistributed

5. **GUI compatibility**
   - Auto
   - Direct/integrated
   - Offscreen capture
   - Separate helper rendering
   - per-plugin refresh/input profiles

## Real-time rule

The audio callback must not wait on ordinary GUI/control IPC. Real-time audio transport will use preallocated shared memory and bounded synchronization primitives; this is intentionally not faked in the bootstrap.
