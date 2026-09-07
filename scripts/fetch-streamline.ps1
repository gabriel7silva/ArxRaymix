<#
.SYNOPSIS
    Download NVIDIA Streamline SDK 2.12.0 (headers + signed production DLLs).

.DESCRIPTION
    Binaries are not in the GitHub source tree (SL 2.7.32+). This pulls the
    official release zip and lays it out as:

        arx/third_party/streamline/include/
        arx/third_party/streamline/lib/x64/
        arx/third_party/streamline/bin/x64/

    Re-run CMake after this so ARX_HAVE_STREAMLINE is set.

    https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.12.0
#>
[CmdletBinding()]
param(
    [string]$Version = '2.12.0'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'arx\third_party\streamline'
$zip = Join-Path $dest "streamline-sdk-v$Version.zip"
$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$Version/streamline-sdk-v$Version.zip"

New-Item -ItemType Directory -Force -Path $dest | Out-Null

if (-not (Test-Path $zip) -or (Get-Item $zip).Length -lt 1MB) {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
}

Write-Host "Extracting $(Get-Item $zip).Length bytes..."
$unpack = Join-Path $dest '_unpack'
if (Test-Path $unpack) {
    Remove-Item -Recurse -Force $unpack
}
Expand-Archive -Path $zip -DestinationPath $unpack -Force

function Find-Child([string]$name) {
    Get-ChildItem -Path $unpack -Recurse -Directory -Filter $name -ErrorAction SilentlyContinue |
        Select-Object -First 1
}

$includeSrc = Find-Child 'include'
if (-not $includeSrc) {
    throw 'Streamline zip has no include/ folder'
}
$libSrc = Get-ChildItem -Path $unpack -Recurse -Filter 'sl.interposer.lib' |
    Where-Object { $_.DirectoryName -match 'x64' } |
    Select-Object -First 1
$dllSrc = Get-ChildItem -Path $unpack -Recurse -Filter 'sl.interposer.dll' |
    Where-Object { $_.DirectoryName -match 'x64' -and $_.DirectoryName -notmatch 'development' } |
    Select-Object -First 1
if (-not $dllSrc) {
    $dllSrc = Get-ChildItem -Path $unpack -Recurse -Filter 'sl.interposer.dll' | Select-Object -First 1
}
if (-not $dllSrc) {
    throw 'Streamline zip has no sl.interposer.dll'
}

$includeDst = Join-Path $dest 'include'
$libDst = Join-Path $dest 'lib\x64'
$binDst = Join-Path $dest 'bin\x64'
foreach ($d in @($includeDst, $libDst, $binDst)) {
    if (Test-Path $d) {
        Remove-Item -Recurse -Force $d
    }
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

Copy-Item -Recurse -Force (Join-Path $includeSrc.FullName '*') $includeDst
if ($libSrc) {
    Copy-Item -Force $libSrc.FullName $libDst
}
Copy-Item -Force (Join-Path $dllSrc.DirectoryName '*.dll') $binDst

Remove-Item -Recurse -Force $unpack

Write-Host "Streamline $Version ready:"
Write-Host "  include : $includeDst"
Write-Host "  lib     : $libDst"
Write-Host "  bin     : $binDst"
Get-ChildItem $binDst -Filter '*.dll' | ForEach-Object { Write-Host "    $($_.Name)" }
Write-Host ''
Write-Host 'Reconfigure CMake so ARX_HAVE_STREAMLINE is set, then rebuild arx.'
