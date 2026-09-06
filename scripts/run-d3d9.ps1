<#
.SYNOPSIS
    Launch Arx Raymix with the system Direct3D 9 renderer. No RTX Remix.

.DESCRIPTION
    Starts arx.exe through the stock Windows d3d9.dll:

        arx.exe --user-dir runtime\user --data-dir <Arx Fatalis> [--loadlevel N]

    Does not pass --remix-dll, --remix-probe or --remix-debug.

    The Arx Fatalis install is located from the Steam and GOG registry entries.
    Pass -DataDir to override. Logs go to runtime\user\arx.log.

.PARAMETER DataDir
    Arx Fatalis install directory (the one containing data.pak). Detected from
    Steam and GOG when omitted.

.EXAMPLE
    .\scripts\run-d3d9.ps1
    .\scripts\run-d3d9.ps1 -LoadLevel 1
    .\scripts\run-d3d9.ps1 -DataDir 'D:\Games\Arx Fatalis'
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$LoadLevel = 0
)

$ErrorActionPreference = 'Stop'

# [IO.Path]::Combine, not Join-Path: Join-Path resolves the drive, so a path on a
# drive that does not exist - a typo in -DataDir, or a Steam library on a disk
# that is not plugged in - throws, and $ErrorActionPreference = 'Stop' turns that
# into a stack trace instead of the message at the bottom of this script.
function Join-PathLiteral {
    param([string]$Base, [string]$Leaf)
    return [System.IO.Path]::Combine($Base, $Leaf)
}

function Test-ArxDataDir {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    return (Test-Path -LiteralPath (Join-PathLiteral $Path 'data.pak') -ErrorAction SilentlyContinue)
}

function Get-SteamLibraryRoots {
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
    } catch {
        return @()
    }
    if ([string]::IsNullOrWhiteSpace($steam)) { return @() }
    $steam = $steam -replace '/', '\'

    $roots = @($steam)

    $vdf = Join-PathLiteral $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $vdf -ErrorAction SilentlyContinue) {
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
        $candidate = Join-PathLiteral $root 'steamapps\common\Arx Fatalis'
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
$userDir = Join-Path $root 'runtime\user'

if (-not (Test-Path $exe)) {
    throw "arx.exe not found at $exe. Configure and build first:`n" +
          "  cmake -S `"$root`" -B `"$root\build`" -A x64`n" +
          "  cmake --build `"$root\build`" --config $Config --target arx --parallel"
}

if (-not $DataDir) {
    $DataDir = Find-ArxFatalis
}

if (-not (Test-ArxDataDir $DataDir)) {
    throw "Arx Fatalis data not found (no data.pak). Checked the Steam and GOG registry entries.`n" +
          "Pass the install directory explicitly: -DataDir '<path to Arx Fatalis>'"
}

New-Item -ItemType Directory -Force -Path $userDir | Out-Null

$arguments = @('--user-dir', $userDir, '--data-dir', $DataDir)
if ($LoadLevel -ne 0) {
    $arguments += @('--loadlevel', "$LoadLevel")
}

Write-Host "exe    : $exe"
Write-Host "data   : $DataDir"
Write-Host "userdir: $userDir"
Write-Host ''
Write-Host 'Direct3D 9 only (system d3d9.dll, no Remix / RT). Log: runtime\user\arx.log'

$env:ARX_RENDERER = 'd3d9'
Set-Location $root
& $exe @arguments
