<#
.SYNOPSIS
    Alias for scripts/run-d3d12.ps1 (DirectX 12).
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$LoadLevel = 0
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'run-d3d12.ps1') -Config $Config -DataDir $DataDir -LoadLevel $LoadLevel
