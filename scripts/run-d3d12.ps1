<#
.SYNOPSIS
    Launch Arx Raymix with the Direct3D 12 / DirectX 12 raster renderer.

.DESCRIPTION
    Starts arx.exe with ARX_RENDERER=d3d12 (raster; RTAO is an in-game toggle).

.EXAMPLE
    .\scripts\run-d3d12.ps1
    .\scripts\run-d3d12.ps1 -LoadLevel 1
    .\scripts\run-d3d12.ps1 -DataDir 'D:\Games\Arx Fatalis'
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$LoadLevel = 0,
    [ValidateRange(0, 4)]
    [int]$DxrDebug = 0,
    [ValidateRange(0, 2)]
    [int]$D3d12Debug = 0
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-ArxFatalis.ps1')
Start-Arx -Renderer d3d12 -Config $Config -DataDir $DataDir -LoadLevel $LoadLevel -DxrDebug $DxrDebug -D3d12Debug $D3d12Debug
