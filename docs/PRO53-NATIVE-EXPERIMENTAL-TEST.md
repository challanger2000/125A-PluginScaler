# 125A PluginScaler – Pro-53 experimental native test (2026-10-10)

## Distribution and safety

- One native Windows x86 GUI executable: `125A-PluginScaler-Pro53.exe`.
- The project-owned 32-bit hook DLL is embedded as an executable resource and unpacked temporarily by the EXE; no jBridge or Pro-53 binaries are modified, copied, or redistributed.
- The test build was compiled on Windows and its embedded native DLL passed a no-jBridge self-test. Do not confuse this with an acceptance test on the real Pro-53.
- This is explicitly **an experimental original-plugin candidate**, not a finished universal PluginScaler release. Cross-process graphics hooking can destabilize an old host and its DAW.

## User scenario

1. Save the Studio One project, preferably try the candidate in an empty disposable project first.
2. Open the original 32-bit Pro-53 through jBridge, preferably with `Switch to separate GUI mode` active.
3. Launch the single `125A-PluginScaler-Pro53.exe` normally (double click; no terminal and no file selection).
4. Click `150 %` or `200 %`. If the native Pro-53 editor cannot be identified with certainty, the manager reports unsupported and makes no changes.
5. Use `100 % / Beenden` before closing the original plugin or Studio One. A failed rollback deliberately keeps the helper alive instead of knowingly unloading a patched module.
6. For first real-world acceptance, check that the full UI is visible (not clipped by the jBridge host frame), potentiometer drag, MIDI, preset switches, keyboard/mouse focus, right-click and reopening/closing all continue working.

## Verified Windows automated tests

- Build and embedded-resource extraction self-test
- Original 32-bit HWND hook attachment in two independent mock processes
- Native renderer GDI and bitmap `SetDIBitsToDevice` zoom to 150/200 percent
- Original native mouse clicks/drags and exact coordinate mapping
- Attach after the native GUI already exists
- Restore previous renderer imports and window geometry
- Restore a *separate jBridge-like containing frame* and its child
- Two independent mock editors with no cross-talk

Evidence: https://github.com/challanger2000/125A-PluginScaler/actions/runs/38006969081

## Technical acceptance limits

- Strict in-process validation: only 32-bit `auxhost.exe`/`gauxhost.exe`; window procedure must map to a module whose loaded filename is `Pro-53.dll` and whose GDI imports include `BeginPaint` and `SetDIBitsToDevice`. No arbitrary window hooking allowed.
- The mock GUI demonstrates behavior of a subset of Windows GDI bitmap renderers, **not runtime compatibility with Native Instruments Pro-53**.
- The Pro-53's actual window topology, plugin-specific drawing shortcuts, popup/mouse wheel, its MIDI/audio integrity, jBridge lifecycle and more than one plugin editor sharing one auxhost are **unverified**.
- FM7 and x64 auxhost support are out of scope for this test build.
- The first full Pro-53/Studio One test is the release gate. A successful GitHub Actions run does not waive it.
