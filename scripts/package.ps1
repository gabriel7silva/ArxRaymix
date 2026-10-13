<#
.SYNOPSIS
    Build the zip a player downloads: arx.exe, its DLLs and the engine data, nothing else.

.DESCRIPTION
    Stages a built arx.exe into dist\ArxRaymix-<version>\ and zips it:

      arx.exe and every DLL next to it (SDL2, OpenAL, DXC, Streamline when fetched)
      data\                 arx\data\core (localisation overlays, fonts)
      LICENSE, AUTHORS, README.txt
      Arx Raymix - DirectX 9.bat

    arx.exe finds Arx Fatalis through the Steam / GOG registry entries on its own and
    reads data\ from its own folder, so double-clicking it is the whole install.

    Never packaged: game data, PDBs (unless -IncludePdb), data.dirs (it holds this
    machine's checkout path) and the imported PT-BR speech WAVs, which are Arkane's
    audio and not ours to redistribute.

.PARAMETER Version
    Name used for the folder and the zip. Defaults to `git describe --tags --always`.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Version 1.4 -IncludePdb
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$Version,
    [string]$OutDir,
    [switch]$IncludePdb
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-ArxFatalis.ps1')

$root = Get-ArxRepoRoot
$exe = Get-ArxExecutable -Config $Config
if (-not (Test-Path -LiteralPath $exe)) {
    throw "arx.exe not found at $exe. Build first:`n" +
          "  cmake --build build --config $Config --target arx --parallel"
}
$binDir = Split-Path -Parent $exe

if (-not $Version) {
    $Version = (& git -C $root describe --tags --always 2>$null)
    if (-not $Version) { $Version = 'dev' }
}
if (-not $OutDir) {
    $OutDir = Join-Path $root 'dist'
}

$name = "ArxRaymix-$Version"
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# Executable and runtime DLLs: CMake already staged everything arx.exe loads next to it.
Copy-Item -LiteralPath $exe -Destination $stage
$dlls = Get-ChildItem -LiteralPath $binDir -Filter '*.dll'
foreach ($dll in $dlls) {
    Copy-Item -LiteralPath $dll.FullName -Destination $stage
}
if ($IncludePdb) {
    $pdb = [System.IO.Path]::ChangeExtension($exe, '.pdb')
    if (Test-Path -LiteralPath $pdb) {
        Copy-Item -LiteralPath $pdb -Destination $stage
    }
}

$missing = @()
foreach ($required in 'SDL2.dll', 'OpenAL32.dll', 'dxcompiler.dll', 'dxil.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $required))) { $missing += $required }
}
if ($missing) {
    throw "Missing next to arx.exe: $($missing -join ', '). Rebuild the arx target so CMake stages them."
}
$haveStreamline = Test-Path -LiteralPath (Join-Path $stage 'sl.interposer.dll')
if (-not $haveStreamline) {
    Write-Warning 'sl.interposer.dll is not next to arx.exe: this package has no DLSS / Frame Generation. Run scripts/fetch-streamline.ps1, reconfigure and rebuild to include it.'
}

# Engine data, minus the speech WAVs (game audio, not ours to ship).
$dataSrc = Join-Path $root 'arx\data\core'
$dataDst = Join-Path $stage 'data'
Copy-Item -LiteralPath $dataSrc -Destination $dataDst -Recurse
Get-ChildItem -LiteralPath $dataDst -Recurse -Include '*.wav', '*.dirs' | Remove-Item -Force
Get-ChildItem -LiteralPath $dataDst -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } |
    Remove-Item -Force

Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'arx\AUTHORS') -Destination $stage

$readme = @"
Arx Raymix $Version
===================

Arx Fatalis on the Arx Libertatis engine, with a Direct3D 12 renderer, DXR ray tracing,
DLSS and Frame Generation. You need your own copy of Arx Fatalis (Steam or GOG).

Install
  1. Install Arx Fatalis from Steam or GOG and run it once.
  2. Extract this folder anywhere. It does not go inside the game folder.
  3. Run arx.exe.

If the game is not found, point at the folder that holds data.pak:
  arx.exe --data-dir "D:\Games\Arx Fatalis"

"Arx Raymix - DirectX 9.bat" starts the Direct3D 9 fallback for one session.

Settings live in Options -> Video, Options -> Ray tracing and Options -> Render.
Saves, cfg.ini and arx.log are usually in Documents\My Games\Arx Libertatis. Attach arx.log to
any bug report; it is overwritten on every launch.

Uninstall: delete this folder.

Source and license (GPLv3): https://github.com/gabriel7silva/ArxRaymix
Not affiliated with or endorsed by Arkane Studios or ZeniMax.
"@
Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Value $readme -Encoding UTF8

$bat = @"
@echo off
rem Starts Arx Raymix on the Direct3D 9 fallback for this session only; cfg.ini is not changed.
set ARX_RENDERER=d3d9
start "" "%~dp0arx.exe" %*
"@
Set-Content -LiteralPath (Join-Path $stage 'Arx Raymix - DirectX 9.bat') -Value $bat -Encoding ASCII

if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $zip).Length

Write-Host "folder : $stage"
Write-Host "zip    : $zip"
Write-Host ("size   : {0:N1} MB" -f ($size / 1MB))
Write-Host "sha256 : $hash"
Write-Host "DLSS   : $(if ($haveStreamline) { 'included' } else { 'not included' })"
