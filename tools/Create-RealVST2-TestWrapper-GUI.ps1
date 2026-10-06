$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

function Show-Error([string]$message) {
    [System.Windows.Forms.MessageBox]::Show(
        $message,
        "125A PluginScaler - Real VST2 Test",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
    ) | Out-Null
}

function Show-Info([string]$message) {
    [System.Windows.Forms.MessageBox]::Show(
        $message,
        "125A PluginScaler - Real VST2 Test",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Information
    ) | Out-Null
}

try {
    $root = Split-Path -Parent $MyInvocation.MyCommand.Path
    $proxy = Join-Path $root "PluginScalerVST2Proxy.dll"
    $helper = Join-Path $root "PluginScalerHelper-x86.exe"
    $builder = Join-Path $root "Create-VST2Wrapper.ps1"

    foreach ($required in @($proxy, $helper, $builder)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Fehlende TestKit-Datei: $required"
        }
    }

    $open = New-Object System.Windows.Forms.OpenFileDialog
    $open.Title = "Originales 32-Bit VST2 auswählen (z. B. Pro-53.dll oder FM7.dll)"
    $open.Filter = "VST2 DLL (*.dll)|*.dll|Alle Dateien (*.*)|*.*"
    $open.Multiselect = $false
    $open.CheckFileExists = $true

    if ($open.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        exit 0
    }

    $target = $open.FileName
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($target)
    $wrapperName = "$baseName-125A"

    $folder = New-Object System.Windows.Forms.FolderBrowserDialog
    $folder.Description = "Ordner wählen, in dem der x64-Testwrapper erstellt werden soll"
    $folder.ShowNewFolderButton = $true

    if ($folder.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
        exit 0
    }

    $outputDir = Join-Path $folder.SelectedPath $wrapperName

    $buildArgs = @{
        TargetDll = $target
        ProxyDll = $proxy
        HelperExe = $helper
        OutputDir = $outputDir
        Scale = 100
        EditorMode = "Direct"
        WrapperName = $wrapperName
    }

    & $builder @buildArgs

    if ($LASTEXITCODE -ne 0) {
        throw "Wrapper-Erzeugung fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }

    $wrapperDll = Join-Path $outputDir "$wrapperName.dll"
    $manifest = Join-Path $outputDir "$wrapperName.pluginscaler.txt"
    $config = Join-Path $outputDir "$wrapperName.pluginscaler.ini"
    $wrapperHelper = Join-Path $outputDir "PluginScalerHelper-x86.exe"

    foreach ($created in @($wrapperDll, $manifest, $config, $wrapperHelper)) {
        if (-not (Test-Path -LiteralPath $created -PathType Leaf)) {
            throw "Erwartete Ausgabedatei fehlt: $created"
        }
    }

    @"
125A PluginScaler - Real VST2 Feldtest

Plugin:
$target

In Studio One laden:
$wrapperDll

Testreihenfolge:
1. Plugin scannen und laden.
2. MIDI spielen: Ton muss sofort kommen und Note-Off muss sauber stoppen.
3. Mehrere Regler bewegen: GUI und Klang müssen reagieren.
4. Preset wechseln und einige Parameter erneut bewegen.
5. GUI mindestens 5x öffnen und schließen.
6. Projekt speichern, Studio One schließen, Projekt neu laden.
7. Prüfen, ob Preset/Parameterzustand korrekt wiederhergestellt ist.
8. Plugin deaktivieren/aktivieren und erneut MIDI spielen.
9. Erst wenn 100% Direct stabil ist, GUI-Skalierung testen.

Bei einem Fehler bitte nicht weiterprobieren:
- merken, welcher Schritt fehlschlug,
- Studio One Verhalten nennen (Freeze/Crash/Stille/falsche GUI),
- wenn vorhanden PluginScaler-Diagnosedatei schicken.
"@ | Set-Content -LiteralPath (Join-Path $outputDir "TEST-CHECKLIST.txt") -Encoding UTF8

    $nl = [Environment]::NewLine
    Show-Info ("Wrapper erstellt." + $nl + $nl +
        "In Studio One laden:" + $nl + $wrapperDll + $nl + $nl +
        "Zuerst nur bei 100% / Direct testen.")
}
catch {
    Show-Error $_.Exception.Message
    exit 1
}
