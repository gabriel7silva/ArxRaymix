<#
.SYNOPSIS
    Launch Arx Raymix with the RTX Remix runtime as the renderer.

.DESCRIPTION
    Resolves the three things a run needs and checks each one before starting:

        arx.exe --user-dir runtime\user --data-dir <Arx Fatalis> `
                --remix-dll runtime\remix-extract\.trex\d3d9.dll [--loadlevel N]

    The Arx Fatalis install is located automatically from the Steam and GOG
    registry entries. Pass -DataDir to override that, for a portable install or
    a second copy.

    The log is kept in runtime\user\ instead of the user's Saved Games folder, so
    a run is self-contained and can be thrown away.

    The Remix runtime is not in Git. Download a release from
    https://github.com/NVIDIAGameWorks/rtx-remix/releases and extract it into
    runtime\remix-extract\, then use the x64 renderer at .trex\d3d9.dll - not the
    d3d9.dll at the root of the zip, which is the 32-bit bridge.

.PARAMETER DataDir
    Arx Fatalis install directory (the one containing data.pak). Detected from
    Steam and GOG when omitted.

.PARAMETER DebugMask
    Bitmask passed to --remix-debug. Most bits drive the Remix C API scene path,
    which the D3D9 renderer does not run. Bit 128 forces the Remix developer UI
    on. See RemixConvert.h for the full list.

.PARAMETER Probe
    Run the standalone probe (--remix-probe) instead of the game: a triangle
    through the runtime with no game state, which separates a broken integration
    from a broken runtime or driver.

.EXAMPLE
    .\scripts\run-remix-scene.ps1 -LoadLevel 1
    .\scripts\run-remix-scene.ps1 -DataDir 'D:\Games\Arx Fatalis'
    .\scripts\run-remix-scene.ps1 -Probe
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$DebugMask = 0,
    [int]$LoadLevel = 0,
    [switch]$Probe
)

$ErrorActionPreference = 'Stop'

function Test-ArxDataDir {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    return (Test-Path (Join-Path $Path 'data.pak'))
}

function Get-SteamLibraryRoots {
    # SteamPath is written with forward slashes; Join-Path copes, but normalise
    # it anyway so error messages read like Windows paths.
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
    } catch {
        return @()
    }
    if ([string]::IsNullOrWhiteSpace($steam)) { return @() }
    $steam = $steam -replace '/', '\'

    $roots = @($steam)

    # Extra library folders live in a VDF file. Rather than parse VDF properly,
    # pull out the quoted "path" values - the only ones that look like paths.
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"(.+)"') {
                $roots += ($matches[1] -replace '\\\\', '\')
            }
        }
    }

    return $roots
}

function Find-ArxFatalis {
    foreach ($root in Get-SteamLibraryRoots) {
        $candidate = Join-Path $root 'steamapps\common\Arx Fatalis'
        if (Test-ArxDataDir $candidate) { return $candidate }
    }

    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\*', 'HKLM:\SOFTWARE\GOG.com\Games\*') {
        foreach ($game in (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue)) {
            if ($game.gameName -like '*Arx Fatalis*' -and (Test-ArxDataDir $game.path)) {
                return $game.path
            }
        }
    }

    return $null
}

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\arx\$Config\arx.exe"
$dll = Join-Path $root 'runtime\remix-extract\.trex\d3d9.dll'
$userDir = Join-Path $root 'runtime\user'

if (-not (Test-Path $exe)) {
    throw "arx.exe not found at $exe. Configure and build first:`n" +
          "  cmake -S `"$root`" -B `"$root\build`" -A x64 -DWITH_RTX_REMIX=ON`n" +
          "  cmake --build `"$root\build`" --config $Config --target arx --parallel"
}

if (-not (Test-Path $dll)) {
    throw "Remix renderer not found at $dll.`n" +
          "Extract an RTX Remix release into $root\runtime\remix-extract\ and use .trex\d3d9.dll " +
          "(x64), not the d3d9.dll at the root of the zip (32-bit bridge)."
}

if (-not $DataDir) {
    $DataDir = Find-ArxFatalis
}

if (-not (Test-ArxDataDir $DataDir)) {
    throw "Arx Fatalis data not found (no data.pak). Checked the Steam and GOG registry entries.`n" +
          "Pass the install directory explicitly: -DataDir '<path to Arx Fatalis>'"
}

New-Item -ItemType Directory -Force -Path $userDir | Out-Null

$arguments = @('--user-dir', $userDir)
$arguments += @('--data-dir', $DataDir)
$arguments += @('--remix-dll', $dll)
if ($Probe) {
    $arguments += '--remix-probe'
}
if ($DebugMask -ne 0) {
    $arguments += @('--remix-debug', "$DebugMask")
}
if ($LoadLevel -ne 0) {
    $arguments += @('--loadlevel', "$LoadLevel")
}

Write-Host "exe    : $exe"
Write-Host "remix  : $dll"
Write-Host "data   : $DataDir"
Write-Host "userdir: $userDir"
Write-Host ''
Write-Host 'Alt+X opens the Remix developer menu. Log: runtime\user\arx.log'

Set-Location $root
& $exe @arguments
