<#
.SYNOPSIS
    Build the zip a player downloads: arx.exe, its DLLs and the engine data, nothing else.

.DESCRIPTION
    Stages a built arx.exe into dist\ArxRaymix-<version>\ and zips it:

      arx.exe and every DLL next to it (SDL2, OpenAL, DXC, Streamline when fetched)
      ArxRaymix.exe         the launcher: checks GitHub for a newer release, then starts arx.exe
                            (omitted with -Standalone)
      package.txt           the version, then every packaged file (the launcher's manifest)
      data\                 arx\data\core (localisation overlays, fonts)
      LICENSE, AUTHORS, README.txt
      licenses\             one file per bundled library (SDL2, OpenAL Soft, FreeType,
                            zlib, DXC, NVIDIA Streamline / DLSS, ...), and README.txt from
                            THIRD-PARTY-NOTICES.md listing them all
      Arx Raymix - DirectX 9.bat

    Next to the zip goes <zip>.sha256, which the launcher checks before installing an update.

    arx.exe finds Arx Fatalis through the Steam / GOG registry entries on its own and
    reads data\ from its own folder, so double-clicking it is the whole install.

    Never packaged: game data, PDBs (unless -IncludePdb), data.dirs (it holds this
    machine's checkout path) and the imported PT-BR speech WAVs, which are Arkane's
    audio and not ours to redistribute.

.PARAMETER Version
    Name used for the folder and the zip. Defaults to `git describe --tags --always`.

.PARAMETER Standalone
    Leave ArxRaymix.exe out, so the package cannot update itself: the player runs arx.exe and
    nothing in the folder touches the network. The zip is named
    ArxRaymix-<version>-standalone.zip so it is never confused with the self-updating one.

    Written for mirrors that run their own malware scan, such as Nexus Mods, on the theory
    that the self-updating launcher was what they flagged. Measurement disproved that: the
    launcher scores 0/71 on VirusTotal, and the single detection anywhere (SecureAge, on the
    NSIS installer stub) fires with and without it. Nexus then quarantined this zip too, at
    0/66, so their rule reads the file type and not a scanner result. Keep the switch anyway
    -- a build that provably makes no network calls is worth being able to hand someone.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Version 1.4 -IncludePdb
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Version 1.6 -Standalone
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$Version,
    [string]$OutDir,
    [switch]$IncludePdb,
    [switch]$Standalone
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

$name = if ($Standalone) { "ArxRaymix-$Version-standalone" } else { "ArxRaymix-$Version" }
$stage = Join-Path $OutDir $name
$zip = Join-Path $OutDir "$name.zip"

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
# Absolute and without a trailing separator: the manifest below cuts this prefix off FullName.
$stage = (Resolve-Path -LiteralPath $stage).ProviderPath.TrimEnd([char]92)

# Executable and runtime DLLs: CMake already staged everything arx.exe loads next to it.
Copy-Item -LiteralPath $exe -Destination $stage
if (-not $Standalone) {
    $launcher = Join-Path $binDir 'ArxRaymix.exe'
    if (-not (Test-Path -LiteralPath $launcher)) {
        throw "ArxRaymix.exe not found next to arx.exe. Rebuild the arx target; it builds the launcher too."
    }
    Copy-Item -LiteralPath $launcher -Destination $stage
}
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
# -File: on Windows PowerShell 5.1, -Include with -LiteralPath also matches whole folders.
Get-ChildItem -LiteralPath $dataDst -Recurse -File -Include '*.wav', '*.dirs' | Remove-Item -Force
Get-ChildItem -LiteralPath $dataDst -Recurse -Directory |
    Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } |
    Remove-Item -Force

Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'arx\AUTHORS') -Destination $stage

# Third-party licences: every library whose code or DLL ends up in the zip.
$licDir = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Force -Path $licDir | Out-Null
$licenses = [ordered]@{
    'README.txt'         = 'THIRD-PARTY-NOTICES.md'
    'SDL2.txt'           = 'libs\sdl\COPYING.txt'
    'OpenAL-Soft.txt'    = 'libs\openal\COPYING'
    'FreeType.txt'       = 'libs\freetype\LICENSE'
    'zlib.txt'           = 'libs\zlib\README.txt'
    'Boost.txt'          = 'libs\boost\LICENSE_1_0.txt'
    'GLM.txt'            = 'libs\glm\copying.txt'
    'libepoxy.txt'       = 'libs\libepoxy\COPYING'
    'DejaVu-fonts.txt'   = 'arx\LICENSE.DejaVu'
    'fast_float.txt'     = 'arx\LICENSE.fast_float'
    'stb.txt'            = 'arx\LICENSE.stb'
}
foreach ($entry in $licenses.GetEnumerator()) {
    $src = Join-Path $root $entry.Value
    if (-not (Test-Path -LiteralPath $src)) {
        throw "Licence file missing: $($entry.Value)"
    }
    Copy-Item -LiteralPath $src -Destination (Join-Path $licDir $entry.Key)
}
$dxcNotice = @"
dxcompiler.dll and dxil.dll are the Microsoft DirectX Shader Compiler redistributables from the
Windows 10 SDK, shipped under the redistribution terms of that SDK.
Source and licence of the compiler: https://github.com/microsoft/DirectXShaderCompiler
"@
Set-Content -LiteralPath (Join-Path $licDir 'DirectXShaderCompiler.txt') -Value $dxcNotice -Encoding UTF8
if ($haveStreamline) {
    # Written by scripts/fetch-streamline.ps1 from the official SDK zip.
    $slLicenses = Join-Path $root 'arx\third_party\streamline\licenses'
    $slDst = Join-Path $licDir 'NVIDIA-Streamline'
    New-Item -ItemType Directory -Force -Path $slDst | Out-Null
    $slFiles = @()
    if (Test-Path -LiteralPath $slLicenses) {
        $slFiles = @(Get-ChildItem -LiteralPath $slLicenses -File)
    }
    foreach ($file in $slFiles) {
        Copy-Item -LiteralPath $file.FullName -Destination $slDst
    }
    if ($slFiles.Count -eq 0) {
        Write-Warning 'No Streamline licence files (re-run scripts/fetch-streamline.ps1); shipping a pointer instead.'
    }
    $slNotice = @"
sl.*.dll and nvngx_*.dll are NVIDIA Streamline 2.12.0 and DLSS binaries, (c) NVIDIA Corporation,
redistributed unmodified under NVIDIA's terms. They are not covered by the GPLv3.
Streamline SDK and its licence: https://github.com/NVIDIA-RTX/Streamline
"@
    Set-Content -LiteralPath (Join-Path $slDst 'README.txt') -Value $slNotice -Encoding UTF8
}

if ($Standalone) {
    $runStep = @'
  3. Run arx.exe.
'@
    $updates = @'
Updates
  This package cannot update itself: it ships without the ArxRaymix.exe launcher, so nothing
  here contacts the network to look for a new version. Download a new release by hand from
  https://github.com/gabriel7silva/ArxRaymix/releases and extract it over this folder, or use
  the self-updating package from that page. The main menu still says when a new version is out;
  Options -> Interface -> Check for updates turns that off.
'@
} else {
    $runStep = @'
  3. Run ArxRaymix.exe. It checks for a newer Arx Raymix, offers to install it, then starts
     the game. arx.exe starts the game without that check.
'@
    $updates = @'
Updates
  ArxRaymix.exe downloads the new release from GitHub, checks its SHA-256 and replaces only the
  files listed in package.txt. The previous version goes to the backup folder, and
  "ArxRaymix.exe --rollback" puts it back. cfg.ini, saves and the Arx Fatalis files are never
  touched. The main menu also says when a new version is out; Options -> Interface ->
  Check for updates turns both checks off. "check_for_updates=0" in cfg.ini, or the
  ARX_NO_UPDATE_CHECK environment variable, also stops the launcher's check.
'@
}

$readme = @"
Arx Raymix $Version
===================

Arx Fatalis on the Arx Libertatis engine, with a Direct3D 12 renderer, DXR ray tracing,
DLSS and Frame Generation. You need your own copy of Arx Fatalis (Steam or GOG).

Install
  1. Install Arx Fatalis from Steam or GOG and run it once.
  2. Extract this folder anywhere. It does not go inside the game folder.
$runStep
$updates

If the game is not found, point at the folder that holds data.pak:
  arx.exe --data-dir "D:\Games\Arx Fatalis"

"Arx Raymix - DirectX 9.bat" starts the Direct3D 9 fallback for one session.

Settings live in Options -> Video, Options -> Ray tracing and Options -> Render.
Saves, cfg.ini and arx.log are in Saved Games\Arx Libertatis (or Documents\My Games\Arx Libertatis
if that folder already exists). Attach arx.log to
any bug report; it is overwritten on every launch.

Uninstall: delete this folder.

Source and license (GPLv3): https://github.com/gabriel7silva/ArxRaymix
Bundled libraries and NVIDIA DLSS keep their own licences: see the licenses folder.
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

# The launcher's manifest: the version, then every file it may replace on update.
$files = Get-ChildItem -LiteralPath $stage -Recurse -File |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace([char]92, [char]47) } |
    Sort-Object
$manifest = @($Version) + $files
[System.IO.File]::WriteAllText((Join-Path $stage 'package.txt'), (($manifest -join "`r`n") + "`r`n"))

if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$size = (Get-Item -LiteralPath $zip).Length
[System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n")

Write-Host "folder : $stage"
Write-Host "zip    : $zip"
Write-Host ("size   : {0:N1} MB" -f ($size / 1MB))
Write-Host "sha256 : $hash"
Write-Host "DLSS   : $(if ($haveStreamline) { 'included' } else { 'not included' })"
Write-Host "updater: $(if ($Standalone) { 'omitted (-Standalone): run arx.exe' } else { 'ArxRaymix.exe' })"
