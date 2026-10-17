<#
.SYNOPSIS
    Build ArxRaymix-<version>-setup.exe from the folder scripts/package.ps1 staged.

.DESCRIPTION
    Run scripts/package.ps1 -Version <tag> first. This script compiles
    scripts/installer/ArxRaymix.nsi with NSIS 3 (makensis on PATH or in its default install
    folder) into dist\ArxRaymix-<tag>-setup.exe and writes <setup>.sha256 next to it.

    The installer installs per user under %LOCALAPPDATA%\Programs\ArxRaymix, adds Start menu
    and optional desktop shortcuts and an entry in Settings -> Apps, and its uninstaller removes
    the files listed in package.txt.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Version v1.4
    powershell -ExecutionPolicy Bypass -File scripts/build-installer.ps1 -Version v1.4
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$OutDir,
    [switch]$NoLauncher
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) {
    $OutDir = Join-Path $root 'dist'
}
# Absolute: .NET calls below resolve relative paths against their own current folder.
$OutDir = (Resolve-Path -LiteralPath $OutDir).ProviderPath
$name = if ($NoLauncher) { "ArxRaymix-$Version-noupdate" } else { "ArxRaymix-$Version" }
$stage = Join-Path $OutDir $name
if (-not (Test-Path -LiteralPath (Join-Path $stage 'package.txt'))) {
    throw "$stage has no package.txt. Run scripts/package.ps1 -Version $Version first."
}
# Absolute and without a trailing separator: the folder list below cuts this prefix off FullName.
$stage = (Resolve-Path -LiteralPath $stage).ProviderPath.TrimEnd([char]92)

$makensis = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source
if (-not $makensis) {
    foreach ($dir in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'NSIS\makensis.exe'))) {
            $makensis = Join-Path $dir 'NSIS\makensis.exe'
            break
        }
    }
}
if (-not $makensis) {
    throw 'makensis.exe not found. Install NSIS 3 (winget install NSIS.NSIS, or https://nsis.sourceforge.io).'
}

# "v1.4" -> "1.4.0.0" for the setup's file version; CI builds ("ci-<sha>") get 0.0.0.0.
$numbers = @(0)
if ($Version -match '^[vV]?(\d+(\.\d+){0,3})$') {
    $numbers = @($Matches[1].Split('.') | ForEach-Object { [int]$_ })
}
while ($numbers.Count -lt 4) { $numbers += 0 }
$numeric = ($numbers[0..3] -join '.')

# The uninstaller removes the package's folders once they are empty, deepest first.
$dirsNsh = Join-Path $OutDir "$name-dirs.nsh"
$dirs = Get-ChildItem -LiteralPath $stage -Recurse -Directory |
    ForEach-Object { $_.FullName.Substring($stage.Length + 1) } |
    Sort-Object { $_.Length } -Descending
$lines = @($dirs | ForEach-Object { "RMDir `"`$INSTDIR\$_`"" })
[System.IO.File]::WriteAllText($dirsNsh, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding $true))

$setup = Join-Path $OutDir "$name-setup.exe"
$nsi = Join-Path $PSScriptRoot 'installer\ArxRaymix.nsi'
$icon = Join-Path $root 'data\icons\arx-libertatis.ico'
$defines = @("/DVERSION=$Version", "/DVERSION_NUMERIC=$numeric", "/DSTAGE=$stage",
             "/DDIRS_NSH=$dirsNsh", "/DICON=$icon", "/DOUTFILE=$setup")
if ($NoLauncher) {
    if (Test-Path -LiteralPath (Join-Path $stage 'ArxRaymix.exe')) {
        throw "$stage holds ArxRaymix.exe, so it was not staged with -NoLauncher. Run scripts/package.ps1 -NoLauncher first."
    }
    $defines += '/DNO_LAUNCHER'
}
& $makensis /V2 /INPUTCHARSET UTF8 @defines $nsi
if ($LASTEXITCODE -ne 0) {
    throw "makensis failed with exit code $LASTEXITCODE"
}
Remove-Item -LiteralPath $dirsNsh

$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$setup.sha256", "$hash  $(Split-Path -Leaf $setup)`n")

Write-Host "setup  : $setup"
Write-Host ("size   : {0:N1} MB" -f ((Get-Item -LiteralPath $setup).Length / 1MB))
Write-Host "sha256 : $hash"
