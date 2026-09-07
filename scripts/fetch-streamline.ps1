<#
.SYNOPSIS
    Download NVIDIA Streamline SDK 2.12.0 (headers + signed production DLLs).

.DESCRIPTION
    Binaries are not in the GitHub source tree (SL 2.7.32+). This pulls the
    official release zip, verifies SHA-256 and Authenticode, and lays it out as:

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

# GitHub release asset digest for streamline-sdk-v2.12.0.zip (api.github.com).
$PinnedSha256 = @{
    '2.12.0' = 'F5C0A3D870707DDDC3570FB4BCD3655CF48A8A68C3A9D342910CFA21B77DCF48'
}

if (-not $PinnedSha256.ContainsKey($Version)) {
    throw "No pinned SHA-256 for Streamline $Version. Add it to `$PinnedSha256 before fetching."
}
$expected = $PinnedSha256[$Version]

$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'arx\third_party\streamline'
$zip = Join-Path $dest "streamline-sdk-v$Version.zip"
$part = "$zip" + '.part'
$url = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$Version/streamline-sdk-v$Version.zip"

function Get-Sha256Upper([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Test-NvidiaSignedDll([string]$Path) {
    $sig = Get-AuthenticodeSignature -LiteralPath $Path
    if ($sig.Status -ne 'Valid') {
        throw "Authenticode failed for $(Split-Path $Path -Leaf): Status=$($sig.Status)"
    }
    $subject = [string]$sig.SignerCertificate.Subject
    if ($subject -notmatch 'NVIDIA') {
        throw "Authenticode subject is not NVIDIA for $(Split-Path $Path -Leaf): $subject"
    }
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null

$needDownload = $true
if (Test-Path -LiteralPath $zip) {
    $have = Get-Sha256Upper $zip
    if ($have -eq $expected) {
        $needDownload = $false
    } else {
        Write-Host "Cached zip SHA-256 $have does not match $expected -- re-downloading"
        Remove-Item -LiteralPath $zip -Force
    }
}

if ($needDownload) {
    if (Test-Path -LiteralPath $part) {
        Remove-Item -LiteralPath $part -Force
    }
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
    $got = Get-Sha256Upper $part
    if ($got -ne $expected) {
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        throw "Downloaded zip SHA-256 $got does not match pinned $expected"
    }
    Move-Item -LiteralPath $part -Destination $zip -Force
}

Write-Host "Extracting $((Get-Item -LiteralPath $zip).Length) bytes..."
$unpack = Join-Path $dest '_unpack'
if (Test-Path $unpack) {
    Remove-Item -Recurse -Force $unpack
}
try {
    Expand-Archive -Path $zip -DestinationPath $unpack -Force
} catch {
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $unpack -ErrorAction SilentlyContinue
    throw "Expand-Archive failed. Deleted the zip -- re-run this script. $_"
}

function Find-Child([string]$name) {
    Get-ChildItem -Path $unpack -Recurse -Directory -Filter $name -ErrorAction SilentlyContinue |
        Select-Object -First 1
}

$includeSrc = Find-Child 'include'
if (-not $includeSrc) {
    throw 'Streamline zip has no include/ folder'
}
$slh = Join-Path $includeSrc.FullName 'sl.h'
if (-not (Test-Path -LiteralPath $slh)) {
    throw 'Streamline zip include/ has no sl.h'
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

$dlls = Get-ChildItem -LiteralPath $dllSrc.DirectoryName -Filter '*.dll'
if ($dlls.Count -eq 0) {
    throw 'Streamline zip has no DLLs next to sl.interposer.dll'
}
foreach ($dll in $dlls) {
    Test-NvidiaSignedDll $dll.FullName
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
foreach ($dll in $dlls) {
    Copy-Item -Force $dll.FullName $binDst
}

Remove-Item -Recurse -Force $unpack

Write-Host "Streamline $Version ready:"
Write-Host "  include : $includeDst"
Write-Host "  lib     : $libDst"
Write-Host "  bin     : $binDst"
Get-ChildItem $binDst -Filter '*.dll' | ForEach-Object { Write-Host "    $($_.Name)" }
Write-Host ''
Write-Host 'Reconfigure CMake so ARX_HAVE_STREAMLINE is set, then rebuild arx.'
