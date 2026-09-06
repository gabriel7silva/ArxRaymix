<#
.SYNOPSIS
    Shared helpers to locate an Arx Fatalis install and the built arx.exe.

.DESCRIPTION
    Used by the D3D9 and D3D12 launch scripts. Detects Steam and GOG registry
    entries. Never hard-codes a user profile, drive letter, or install path.
#>

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

function Get-ArxRepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Get-ArxExecutable {
    param(
        [string]$Config = 'RelWithDebInfo'
    )
    $root = Get-ArxRepoRoot
    return Join-Path $root "build\arx\$Config\arx.exe"
}
