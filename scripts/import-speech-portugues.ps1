<#
.SYNOPSIS
    Copy Brazilian Portuguese speech WAVs into the data/core overlay.

.DESCRIPTION
    Places files at arx/data/core/speech/portugues/ so the game lists
    Português (Brasil) on the audio language slider. Does not hard-code a
    user profile or download path — pass -SourceDir.

.PARAMETER SourceDir
    Folder that contains the 1999 .wav files (flat, official English names).

.EXAMPLE
    .\scripts\import-speech-portugues.ps1 -SourceDir 'D:\speech\Portuguese'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'Find-ArxFatalis.ps1')

if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) {
    throw "Source directory not found: $SourceDir"
}

$wavs = @(Get-ChildItem -LiteralPath $SourceDir -File -Filter '*.wav')
if ($wavs.Count -eq 0) {
    throw "No .wav files in $SourceDir"
}

$dst = Join-PathLiteral (Get-ArxRepoRoot) 'arx\data\core\speech\portugues'
New-Item -ItemType Directory -Force -Path $dst | Out-Null

$copied = 0
foreach ($file in $wavs) {
    $name = $file.Name.ToLowerInvariant()
    Copy-Item -LiteralPath $file.FullName -Destination (Join-PathLiteral $dst $name) -Force
    $copied++
}

Write-Host "Copied $copied WAV file(s) to $dst"
Write-Host 'In-game: Options → Language → audio slider → Português (Brasil)'
