<#
.SYNOPSIS
    Load each Arx Fatalis level for a few seconds and score the plausible review items from the log.

.DESCRIPTION
    This is a load sweep, not a playthrough. Each level is started with
    --loadlevel, --skipcinematic and --benchmark TIMELIMIT so the engine quits
    itself. The script does not send input.

    Isolated user dir: runtime/user-sweep (gitignored). Copies cfg.ini from
    runtime/user when present, then forces windowed + RT High + distance Ultra
    so DXR light lines appear.

    What the report can decide from existing logs:
      F4   - upload buffer full (near the silent wrap)
      Pdxr-5 - max n= on DXR lights set changed (shadow set is 16, not 256)
      F5   - DEVICE_REMOVED / timeout / non-zero exit
    F163 / torch-by-eye / PIX stay manual. F155 / F115 / F128 / F154 are CMake.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -DryRun
    powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1 -Levels 1
    powershell -ExecutionPolicy Bypass -File scripts/sweep-plausible-levels.ps1
#>
[CmdletBinding()]
param(
    [string]$Config = 'RelWithDebInfo',
    [string]$DataDir,
    [int]$Seconds = 15,
    [int[]]$Levels,
    [int]$TimeoutSec = 0,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Find-ArxFatalis.ps1')

if ($Seconds -lt 5) {
    throw '-Seconds must be at least 5 (load + a few rendered frames).'
}
if ($TimeoutSec -le 0) {
    $TimeoutSec = $Seconds + 90
}

# Official areas that map to a floor in Levels.cpp, minus unused slots 9/10/20.
if (-not $Levels -or $Levels.Count -eq 0) {
    $Levels = @(1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23)
}

$root = Get-ArxRepoRoot
$exe = Get-ArxExecutable -Config $Config
if (-not (Test-Path -LiteralPath $exe)) {
    throw "arx.exe not found at $exe. Build --target arx first."
}

if (-not $DataDir) {
    $DataDir = Find-ArxFatalis
}
if (-not (Test-ArxDataDir $DataDir)) {
    throw "Arx Fatalis data not found (no data.pak). Pass -DataDir with the install path."
}

$sweepDir = Join-Path $root 'runtime\user-sweep'
$logDir = Join-Path $sweepDir 'logs'
$reportPath = Join-Path $sweepDir 'sweep-report.txt'
$playCfg = Join-Path $root 'runtime\user\cfg.ini'
$sweepCfg = Join-Path $sweepDir 'cfg.ini'
$liveLog = Join-Path $sweepDir 'arx.log'

function Set-IniKey {
    param(
        [string]$Path,
        [string]$Section,
        [string]$Key,
        [string]$Value
    )
    $lines = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $Path) {
        Get-Content -LiteralPath $Path | ForEach-Object { [void]$lines.Add($_) }
    }

    $sectionHeader = '[' + $Section + ']'
    $sectionAt = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -eq $sectionHeader) {
            $sectionAt = $i
            break
        }
    }
    if ($sectionAt -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1].Trim() -ne '') {
            [void]$lines.Add('')
        }
        [void]$lines.Add($sectionHeader)
        [void]$lines.Add("$Key=$Value")
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllLines($Path, $lines, $utf8)
        return
    }

    $replaced = $false
    for ($i = $sectionAt + 1; $i -lt $lines.Count; $i++) {
        $trim = $lines[$i].Trim()
        if ($trim.StartsWith('[') -and $trim.EndsWith(']')) {
            break
        }
        if ($trim -match ('^' + [regex]::Escape($Key) + '\s*=')) {
            $lines[$i] = "$Key=$Value"
            $replaced = $true
            break
        }
    }
    if (-not $replaced) {
        $lines.Insert($sectionAt + 1, "$Key=$Value")
    }
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllLines($Path, $lines, $utf8)
}

function Initialize-SweepUserDir {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    if (Test-Path -LiteralPath $playCfg) {
        Copy-Item -LiteralPath $playCfg -Destination $sweepCfg -Force
    } elseif (-not (Test-Path -LiteralPath $sweepCfg)) {
        $utf8 = New-Object System.Text.UTF8Encoding $false
        [System.IO.File]::WriteAllLines($sweepCfg, @(
            '[video]',
            'renderer=Direct3D 12',
            'full_screen=0',
            'dxr_preset=3',
            'dxr_distance=3'
        ), $utf8)
    }
    Set-IniKey -Path $sweepCfg -Section 'video' -Key 'full_screen' -Value '0'
    Set-IniKey -Path $sweepCfg -Section 'video' -Key 'dxr_preset' -Value '3'
    Set-IniKey -Path $sweepCfg -Section 'video' -Key 'dxr_distance' -Value '3'
}

function Wait-LogUnlocked {
    param(
        [string]$Path,
        [int]$TimeoutMs = 15000
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return $true
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            $fs = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
            $fs.Dispose()
            return $true
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    return $false
}

function Save-LiveLog {
    param(
        [string]$Source,
        [string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source)) {
        return
    }
    if (-not (Wait-LogUnlocked -Path $Source)) {
        Write-Host "  warn: $Source still locked after exit; copying anyway"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Clear-LiveLog {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    if (-not (Wait-LogUnlocked -Path $Path)) {
        Write-Host "  warn: could not unlock $Path before next level"
        return
    }
    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
}

function Get-LogHits {
    param([string]$Path)
    $empty = [pscustomobject]@{
        D3D12          = $false
        DxrYes         = $false
        DxrNo          = $false
        UploadFull     = 0
        LightsMaxN     = 0
        LightsChanges  = 0
        DeviceRemoved  = $false
        BenchmarkOk    = $false
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        return $empty
    }
    $text = Get-Content -LiteralPath $Path -Raw -ErrorAction SilentlyContinue
    if ([string]::IsNullOrEmpty($text)) {
        return $empty
    }
    $empty.D3D12 = $text -match 'Using D3D12 renderer'
    $empty.DxrYes = ($text -match 'RaytracingTier=') -and ($text -notmatch 'RaytracingTier=NOT_SUPPORTED')
    $empty.DxrNo = $text -match 'Ray tracing unavailable|RTAO disabled|RaytracingTier=NOT_SUPPORTED'
    $empty.UploadFull = ([regex]::Matches($text, 'upload buffer full')).Count
    $empty.DeviceRemoved = $text -match 'DEVICE_REMOVED|Device removed|GetDeviceRemovedReason|DXGI_ERROR_DEVICE'
    $empty.BenchmarkOk = $text -match 'Benchmark summary'
    $nMatches = [regex]::Matches($text, 'DXR lights set changed.* n=(\d+)')
    $empty.LightsChanges = $nMatches.Count
    foreach ($m in $nMatches) {
        $n = [int]$m.Groups[1].Value
        if ($n -gt $empty.LightsMaxN) {
            $empty.LightsMaxN = $n
        }
    }
    return $empty
}

Initialize-SweepUserDir

Write-Host "exe      : $exe"
Write-Host "data     : $DataDir"
Write-Host "userdir  : $sweepDir"
Write-Host "levels   : $($Levels -join ', ')"
Write-Host "hold     : ${Seconds}s  timeout ${TimeoutSec}s"
Write-Host ''

if ($DryRun) {
    foreach ($level in $Levels) {
        $cmd = 'arx.exe --user-dir {0} --data-dir {1} --loadlevel {2} --skipcinematic --benchmark {3}s'
        Write-Host ($cmd -f $sweepDir, $DataDir, $level, $Seconds)
    }
    Write-Host ''
    Write-Host 'Dry run only. Re-run without -DryRun to launch.'
    return
}

$previousRenderer = $env:ARX_RENDERER
$rows = New-Object System.Collections.Generic.List[object]

try {
    $env:ARX_RENDERER = 'd3d12'
    Push-Location $root

    foreach ($level in $Levels) {
        $tag = 'level-{0:D2}' -f $level
        Write-Host "=== $tag ==="
        Clear-LiveLog -Path $liveLog

        # One argument string so paths with spaces (Steam "Program Files") stay intact.
        # UseShellExecute=false so ARX_RENDERER reaches the child.
        $argString = '--user-dir "{0}" --data-dir "{1}" --loadlevel {2} --skipcinematic --benchmark {3}s' -f $sweepDir, $DataDir, $level, $Seconds
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.WorkingDirectory = $root
        $psi.Arguments = $argString
        $psi.UseShellExecute = $false
        if ($psi.EnvironmentVariables.ContainsKey('ARX_RENDERER')) {
            $psi.EnvironmentVariables['ARX_RENDERER'] = 'd3d12'
        } else {
            $psi.EnvironmentVariables.Add('ARX_RENDERER', 'd3d12')
        }
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        [void]$proc.Start()
        $finished = $proc.WaitForExit($TimeoutSec * 1000)
        $timedOut = -not $finished
        if ($timedOut) {
            Write-Host "timeout after ${TimeoutSec}s - killing $tag"
            try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
        }
        if (-not $timedOut) {
            $proc.WaitForExit()
        }
        if ($timedOut) {
            $exit = -1
        } else {
            $exit = $proc.ExitCode
        }
        $proc.Dispose()
        Start-Sleep -Milliseconds 400

        $savedLog = Join-Path $logDir ($tag + '.log')
        Save-LiveLog -Source $liveLog -Destination $savedLog

        $hits = Get-LogHits -Path $savedLog
        $status = 'ok'
        if ($timedOut) { $status = 'timeout' }
        elseif ($exit -ne 0) { $status = "exit $exit" }
        elseif (-not $hits.D3D12) { $status = 'no-d3d12' }
        elseif (-not $hits.BenchmarkOk) { $status = 'no-quit' }

        $dxrLabel = '?'
        if ($hits.DxrYes) { $dxrLabel = 'yes' }
        elseif ($hits.DxrNo) { $dxrLabel = 'no' }

        $row = [pscustomobject]@{
            Level         = $level
            Status        = $status
            Exit          = $exit
            D3D12         = $hits.D3D12
            Dxr           = $dxrLabel
            UploadFull    = $hits.UploadFull
            LightsMaxN    = $hits.LightsMaxN
            LightsChanges = $hits.LightsChanges
            DeviceRemoved = $hits.DeviceRemoved
            Log           = $savedLog
        }
        [void]$rows.Add($row)
        $line = '  {0}  dxr={1}  uploadFull={2}  lights n_max={3} changes={4}  removed={5}'
        Write-Host ($line -f $status, $row.Dxr, $row.UploadFull, $row.LightsMaxN, $row.LightsChanges, $row.DeviceRemoved)
    }
} finally {
    Pop-Location
    if ($null -eq $previousRenderer) {
        Remove-Item Env:\ARX_RENDERER -ErrorAction SilentlyContinue
    } else {
        $env:ARX_RENDERER = $previousRenderer
    }
}

$f4Hits = ($rows | Where-Object { $_.UploadFull -gt 0 }).Count
$f5Hits = ($rows | Where-Object { $_.DeviceRemoved -or $_.Status -eq 'timeout' -or $_.Exit -notin @(0, $null) }).Count
$maxN = 0
foreach ($r in $rows) {
    if ($r.LightsMaxN -gt $maxN) { $maxN = $r.LightsMaxN }
}

$report = New-Object System.Collections.Generic.List[string]
[void]$report.Add('Arx Raymix - plausible load sweep')
[void]$report.Add(('When: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm')))
[void]$report.Add(('Hold: {0}s   Levels: {1}' -f $Seconds, ($Levels -join ',')))
[void]$report.Add('')
[void]$report.Add('level  status      dxr  upload  n_max  changes  removed')
foreach ($r in $rows) {
    $rowLine = '{0,5}  {1,-11} {2,-4} {3,6}  {4,5}  {5,7}  {6}'
    [void]$report.Add(($rowLine -f $r.Level, $r.Status, $r.Dxr, $r.UploadFull, $r.LightsMaxN, $r.LightsChanges, $r.DeviceRemoved))
}
[void]$report.Add('')
[void]$report.Add('Verdicts (observed / not observed - not a code fix):')
if ($f4Hits -gt 0) {
    [void]$report.Add(('F4     OBSERVED   upload buffer full on {0} level(s). Add a wrap counter before changing the ring.' -f $f4Hits))
} else {
    [void]$report.Add('F4     not seen   no upload-buffer-full line. Do not implement the wrap flush.')
}
if ($maxN -ge 16) {
    [void]$report.Add(('Pdxr-5 NOTE      shadow set hit n={0} (cap is 16 lights, not 256 cands). Check those levels by eye.' -f $maxN))
} else {
    [void]$report.Add(('Pdxr-5 not seen  max n={0}. The 256-candidate starve did not happen in this sweep.' -f $maxN))
}
if ($f5Hits -gt 0) {
    [void]$report.Add(('F5     OBSERVED   timeout / bad exit / DEVICE_REMOVED on {0} level(s). Re-run those with D3D12_DEBUG=1.' -f $f5Hits))
} else {
    [void]$report.Add('F5     not seen   clean exits, no device-removed line.')
}
[void]$report.Add('F163   manual    DLSS On, still water, look for 1px reflection shimmer (F26 already landed).')
[void]$report.Add('F155   cmake     cmake -DBUILD_TESTS=ON must fail at CppUnit, not reject the flag.')
[void]$report.Add('F115   cmake     summary may say OpenGL; the process is still D3D12.')
[void]$report.Add('F128   docs      /GR- lives in arx/cmake/BuildType.cmake.')
[void]$report.Add('F154   files     Get-AuthenticodeSignature sl.interposer.dll')
[void]$report.Add('F83    skip      Remix path deleted.')
[void]$report.Add('F159   skip      static_asserts landed with F48.')
[void]$report.Add('')
[void]$report.Add(('Per-level logs: {0}' -f $logDir))

$report | Set-Content -LiteralPath $reportPath -Encoding UTF8
Write-Host ''
Write-Host ($report -join [Environment]::NewLine)
Write-Host ''
Write-Host "Report: $reportPath"
