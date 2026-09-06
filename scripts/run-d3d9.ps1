<#
.SYNOPSIS
    Launch Arx Raymix with the Direct3D 9 / DirectX 9 raster renderer.

.DESCRIPTION
    Starts arx.exe with ARX_RENDERER=d3d9:

        arx.exe --user-dir runtime\user --data-dir <Arx Fatalis> [--loadlevel N]

    The Arx Fatalis install is located from the Steam and GOG registry entries.
    Pass -DataDir to override. Logs go to runtime\user\arx.log.

    This environment variable overrides cfg.ini for this process only. The
    in-game Video Options slider still writes the saved choice for later launches.

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

. (Join-Path $PSScriptRoot 'Find-ArxFatalis.ps1')

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
Write-Host 'DirectX 9 raster. Log: runtime\user\arx.log'

$env:ARX_RENDERER = 'd3d9'
Set-Location $root
& $exe @arguments
