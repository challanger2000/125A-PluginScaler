# Real VST2 field test

This kit tests the stabilized x64-to-x86 VST2 host core with real legacy 32-bit plugins before GUI scaling work continues.

## User workflow

1. Double-click START-REAL-VST2-TEST.vbs.
2. Select the original 32-bit VST2 DLL, for example Pro-53 or FM7.
3. Select an output folder.
4. The kit creates a 100% / Direct x64 wrapper in a plugin-specific subfolder.
5. Load the generated *-125A.dll in Studio One.
6. Follow TEST-CHECKLIST.txt in the generated folder.

No command line is required.

The original 32-bit VST2 DLL is never modified.

## What this test proves

- x64 host to x86 helper startup
- correct VST2 mains/start/stop lifecycle
- MIDI note-on/note-off
- audio processing
- parameter changes
- preset/program handling
- state restore after project reload
- repeated editor open/close
- basic Direct editor interaction at 100%

Do not evaluate 125%, 150% or 200% scaling in this field test. Scaling comes only after the 100% Direct host path is stable with the real plugin.

Create-VST2Wrapper.ps1 remains included for engineering and automation use.
