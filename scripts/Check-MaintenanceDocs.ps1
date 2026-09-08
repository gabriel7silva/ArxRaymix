<#
.SYNOPSIS
    Check that every grep printed in docs/maintenance/ still matches something.

.DESCRIPTION
    The maintenance guide anchors on symbol names and greps instead of line
    numbers and copied values, so that it survives the code being tuned. That
    only works while the greps still match. A grep that returns nothing means
    the guide is describing code that no longer exists, which is a bug report
    against the guide.

    This script extracts every fenced `grep -n` / `grep -rn` command from
    docs/maintenance/*.md, runs each one, and fails if any returns no matches.
    It also fails on absolute paths, which docs/CONTRIBUTING.md forbids
    committing, and on links to invariant anchors that do not exist.

    Run it after editing the guide, and after any change that renames a symbol
    the guide mentions.

.PARAMETER Quiet
    Only report failures.

.EXAMPLE
    pwsh -File scripts/Check-MaintenanceDocs.ps1
    pwsh -File scripts/Check-MaintenanceDocs.ps1 -Quiet
#>
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$docsDir = Join-Path $repoRoot 'docs/maintenance'

if (-not (Test-Path -LiteralPath $docsDir)) {
    Write-Error "No docs/maintenance directory at $docsDir"
}

Push-Location $repoRoot
try {
    $docs = Get-ChildItem -LiteralPath $docsDir -Filter '*.md' | Sort-Object Name
    if ($docs.Count -eq 0) { Write-Error 'No markdown files in docs/maintenance' }

    $failures = [System.Collections.Generic.List[string]]::new()
    $checked = 0

    # 1. Every fenced grep must match something.
    #
    # Only inside a fence, as the description says. A grep written to prove an ABSENCE — 'this
    # must return no hits' — matches nothing precisely when it is doing its job, and this check
    # would read that success as a failure. Keeping those out of fences is how a guide says
    # 'run this by hand', and the checker has to honour the distinction it already claims to.
    foreach ($doc in $docs) {
        $lineNo = 0
        $inFence = $false
        foreach ($line in (Get-Content -LiteralPath $doc.FullName)) {
            $lineNo++
            $line = $line.Trim()
            if ($line -match '^```') {
                $inFence = -not $inFence
                continue
            }
            if (-not $inFence) { continue }
            if ($line -notmatch '^grep\s') { continue }

            $checked++
            # Split "grep -n <pattern> <paths...>" without invoking a shell.
            $argv = [System.Management.Automation.PSParser]::Tokenize($line, [ref]$null) |
                Where-Object { $_.Type -in 'CommandArgument', 'CommandParameter', 'String', 'Command', 'Number' } |
                ForEach-Object { $_.Content }
            $argv = @($argv | Select-Object -Skip 1)   # drop "grep"

            $flags = @($argv | Where-Object { $_ -like '-*' })
            # Context flags such as -A take a count; it is not a path.
            $rest = @($argv | Where-Object { $_ -notlike '-*' -and $_ -notmatch '^\d+$' })
            if ($rest.Count -lt 2) {
                $failures.Add("$($doc.Name):${lineNo}  unparsable: $line")
                continue
            }

            $pattern = $rest[0]
            $paths = @($rest | Select-Object -Skip 1)
            $recurse = ($flags -join '') -match 'r'

            # Translate a basic grep pattern to .NET. In basic regular
            # expressions "\|" is alternation and ( ) { } + ? are literal, so
            # each alternative is escaped as a literal string.
            $regex = ($pattern -split '\\\|' | ForEach-Object { [regex]::Escape($_) }) -join '|'

            $files = foreach ($p in $paths) {
                if (Test-Path -LiteralPath $p -PathType Container) {
                    $inc = @()
                    foreach ($f in $flags) {
                        if ($f -match '^--include=\*(?<ext>\.\w+)$') { $inc += "*$($Matches.ext)" }
                    }
                    if ($inc.Count -eq 0) { $inc = @('*') }
                    foreach ($i in $inc) {
                        Get-ChildItem -LiteralPath $p -Filter $i -Recurse:$recurse -File -ErrorAction SilentlyContinue
                    }
                } elseif (Test-Path -LiteralPath $p -PathType Leaf) {
                    Get-Item -LiteralPath $p
                } else {
                    $failures.Add("$($doc.Name):${lineNo}  no such path: $p")
                }
            }

            $hits = 0
            foreach ($f in $files) {
                $hits += @(Select-String -LiteralPath $f.FullName -Pattern $regex -AllMatches -ErrorAction SilentlyContinue).Count
                if ($hits -gt 0) { break }
            }

            if ($hits -eq 0) {
                $failures.Add("$($doc.Name):${lineNo}  matched nothing: $line")
            } elseif (-not $Quiet) {
                Write-Host ("  ok   {0,-24} {1}" -f $doc.Name, $pattern)
            }
        }
    }

    # 2. No absolute paths. docs/CONTRIBUTING.md forbids committing them.
    foreach ($doc in $docs) {
        $bad = Select-String -LiteralPath $doc.FullName -Pattern '[A-Za-z]:\\|/Users/|/home/' -ErrorAction SilentlyContinue
        foreach ($b in $bad) {
            $failures.Add("$($doc.Name):$($b.LineNumber)  absolute path: $($b.Line.Trim())")
        }
    }

    # 3. Every INV-NN link must have a matching anchor.
    $invFile = Join-Path $docsDir 'INVARIANTS.md'
    if (Test-Path -LiteralPath $invFile) {
        $anchors = @(Select-String -LiteralPath $invFile -Pattern 'id="(inv-\d+)"' -AllMatches |
            ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value })
        foreach ($doc in $docs) {
            $links = Select-String -LiteralPath $doc.FullName -Pattern 'INVARIANTS\.md#(inv-\d+)|\]\(#(inv-\d+)\)' -AllMatches
            foreach ($l in $links) {
                foreach ($m in $l.Matches) {
                    $target = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
                    if ($anchors -notcontains $target) {
                        $failures.Add("$($doc.Name):$($l.LineNumber)  dangling anchor: $target")
                    }
                }
            }
        }
    }

    if (-not $Quiet) { Write-Host '' }

    if ($failures.Count -gt 0) {
        Write-Host "FAIL  $($failures.Count) problem(s) in docs/maintenance:" -ForegroundColor Red
        $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        Write-Host ''
        Write-Host 'A grep that matches nothing means the guide describes code that has'
        Write-Host 'moved or been renamed. Fix the guide, not the check.'
        exit 1
    }

    Write-Host "OK  $checked grep(s) across $($docs.Count) file(s); no absolute paths; no dangling anchors." -ForegroundColor Green
    exit 0
}
finally {
    Pop-Location
}
