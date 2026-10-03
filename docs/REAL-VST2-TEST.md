# Real VST2 wrapper setup

Create a test wrapper for a real 32-bit VST2 DLL:

```powershell
powershell -ExecutionPolicy Bypass -File tools/Create-VST2Wrapper.ps1 `
  -TargetDll "C:\Music\VST\Instruments\Pro-53\Pro-53.dll" `
  -ProxyDll ".\PluginScalerVST2Proxy.dll" `
  -HelperExe ".\PluginScalerHelper-x86.exe" `
  -OutputDir ".\Pro53-Test" `
  -Scale 200 `
  -WrapperName "Pro-53-125A"
```

The output directory contains:

- `Pro-53-125A.dll` — x64 VST2 proxy to scan in the 64-bit host.
- `PluginScalerHelper-x86.exe` — 32-bit helper process.
- `Pro-53-125A.pluginscaler.txt` — metadata/parameter manifest generated from the real target DLL.
- `Pro-53-125A.pluginscaler.ini` — sidecar configuration pointing to the original 32-bit DLL.

The original VST2 DLL is not modified.
