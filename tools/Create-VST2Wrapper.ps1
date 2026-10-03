param(
    [Parameter(Mandatory = $true)]
    [string]$TargetDll,

    [Parameter(Mandatory = $true)]
    [string]$ProxyDll,

    [Parameter(Mandatory = $true)]
    [string]$HelperExe,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [ValidateRange(100, 400)]
    [int]$Scale = 200,

    [ValidateSet("Capture", "Direct", "Mag")]
    [string]$EditorMode = "Capture",

    [string]$WrapperName
)

$ErrorActionPreference = "Stop"

$target = (Resolve-Path -LiteralPath $TargetDll).Path
$proxy = (Resolve-Path -LiteralPath $ProxyDll).Path
$helper = (Resolve-Path -LiteralPath $HelperExe).Path

if ([string]::IsNullOrWhiteSpace($WrapperName)) {
    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($target)
    $WrapperName = "$baseName-125A"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$outDir = (Resolve-Path -LiteralPath $OutputDir).Path

$wrapperDll = Join-Path $outDir "$WrapperName.dll"
$wrapperHelper = Join-Path $outDir "PluginScalerHelper-x86.exe"
$manifest = Join-Path $outDir "$WrapperName.pluginscaler.txt"
$config = Join-Path $outDir "$WrapperName.pluginscaler.ini"

Copy-Item -LiteralPath $proxy -Destination $wrapperDll -Force
Copy-Item -LiteralPath $helper -Destination $wrapperHelper -Force

& $wrapperHelper --write-vst2-manifest $target $manifest
if ($LASTEXITCODE -ne 0) {
    throw "Manifest generation failed with exit code $LASTEXITCODE"
}

@"
[PluginScaler]
helper=PluginScalerHelper-x86.exe
target=$target
manifest=$([System.IO.Path]::GetFileName($manifest))
scale=$Scale
editor=$($EditorMode.ToLowerInvariant())
"@ | Set-Content -LiteralPath $config -Encoding Unicode

Write-Host "wrapper=$wrapperDll"
Write-Host "helper=$wrapperHelper"
Write-Host "manifest=$manifest"
Write-Host "config=$config"
Write-Host "target=$target"
Write-Host "scale=$Scale"
Write-Host "editor=$EditorMode"
