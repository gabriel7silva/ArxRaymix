<#
.SYNOPSIS
    Launch Arx Raymix with the Direct3D 9 / DirectX 9 raster renderer.

.DESCRIPTION
    Starts arx.exe with ARX_RENDERER=d3d9.

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
Start-Arx -Renderer d3d9 -Config $Config -DataDir $DataDir -LoadLevel $LoadLevel
