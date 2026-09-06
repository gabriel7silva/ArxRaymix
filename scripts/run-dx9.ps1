<#
.SYNOPSIS
    Alias for scripts/run-d3d9.ps1 (DirectX 9).
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$LoadLevel = 0
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'run-d3d9.ps1') -Config $Config -DataDir $DataDir -LoadLevel $LoadLevel
