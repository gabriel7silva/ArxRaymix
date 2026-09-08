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

function Start-Arx {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('d3d12', 'd3d9')]
        [string]$Renderer,
        [string]$Config = 'RelWithDebInfo',
        [string]$DataDir,
        [int]$LoadLevel = 0,
        # 0 off, 1 instance id, 2 hit normal, 3 hit distance, 4 sampled albedo.
        # Replaces the ray traced reflection with the raw value: the pass cannot print.
        [ValidateRange(0, 4)]
        [int]$DxrDebug = 0
    )

    $root = Get-ArxRepoRoot
    $exe = Get-ArxExecutable -Config $Config
    $userDir = Join-Path $root 'runtime\user'

    if (-not (Test-Path $exe)) {
        throw "arx.exe not found at $exe. Configure and build first:`n" +
              "  cmake -S <repo> -B <repo>\build -A x64`n" +
              "  cmake --build <repo>\build --config $Config --target arx --parallel"
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
    if ($Renderer -eq 'd3d12') {
        Write-Host 'DirectX 12 raster. RTAO: Options -> Ray tracing. Log: runtime\user\arx.log'
    } else {
        Write-Host 'DirectX 9 raster. Log: runtime\user\arx.log'
    }

    if ($DxrDebug -ne 0) {
        Write-Host "DXR debug view $DxrDebug - the reflection shows a raw value, not a reflection"
    }

    $previousRenderer = $env:ARX_RENDERER
    $previousDxrDebug = $env:ARX_DXR_DEBUG
    Push-Location $root
    try {
        $env:ARX_RENDERER = $Renderer
        if ($DxrDebug -ne 0) {
            $env:ARX_DXR_DEBUG = "$DxrDebug"
        }
        & $exe @arguments
    } finally {
        Pop-Location
        if ($null -eq $previousRenderer) {
            Remove-Item Env:\ARX_RENDERER -ErrorAction SilentlyContinue
        } else {
            $env:ARX_RENDERER = $previousRenderer
        }
        if ($null -eq $previousDxrDebug) {
            Remove-Item Env:\ARX_DXR_DEBUG -ErrorAction SilentlyContinue
        } else {
            $env:ARX_DXR_DEBUG = $previousDxrDebug
        }
    }
}
