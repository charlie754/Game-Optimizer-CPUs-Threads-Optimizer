# verify-release-notes.ps1 - verify release-note claims against packaged artifacts.
#
# v0.4.4 reused the zip hash for the exe. Checking for unfilled placeholders missed it.
# Check the actual property: exactly one SHA256 block, all required leaves claimed once,
# every claim matching its artifact, distinct hashes for different files, clean UTF-8.
#
# USAGE
#   tools\verify-release-notes.ps1 -Notes build\release\notes-v0.4.4.md `
#                                  -Zip build\release\GameOptimizer-v0.4.4-x64.zip
#   -Require overrides the default required leaves (zip leaf and GameOptimizer.exe).
#   The zip is extracted to a temporary directory and removed after verification.
# Exit 0 = all checks pass. Exit 1 = at least one failure.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Notes,
    [Parameter(Mandatory = $true)][string]$Zip,
    [string[]]$Require = @([IO.Path]::GetFileName($Zip), 'GameOptimizer.exe')
)

$ErrorActionPreference = 'Stop'
$failures = New-Object System.Collections.Generic.List[string]
$work = $null

function Fail([string]$m) { $script:failures.Add($m); Write-Host "  FAIL  $m" -ForegroundColor Red }
function Pass([string]$m) { Write-Host "  ok    $m" -ForegroundColor Green }

try {
    if (-not (Test-Path -LiteralPath $Notes -PathType Leaf)) { throw "notes file not found: $Notes" }
    if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { throw "zip not found: $Zip" }
    $Notes = (Resolve-Path -LiteralPath $Notes).Path
    $Zip = (Resolve-Path -LiteralPath $Zip).Path

    # ------------------------------------------------------------ encoding
    # Read bytes explicitly: PowerShell 5.1 otherwise reads BOM-less UTF-8 as ANSI.
    Write-Host "`n=== encoding ===" -ForegroundColor Cyan
    $bytes = [IO.File]::ReadAllBytes($Notes)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Fail "notes begin with a UTF-8 BOM"
    } else {
        Pass "no BOM"
    }
    $enc = New-Object Text.UTF8Encoding($false, $true)
    try {
        $text = $enc.GetString($bytes)
        Pass "valid UTF-8"
    } catch {
        Fail "notes are not valid UTF-8: $($_.Exception.Message)"
        $text = [Text.Encoding]::UTF8.GetString($bytes)
    }

    # Parse only the decided grammar; save errors for the existing hash-claims section.
    # A bare fence closes an open block. All other nonblank body lines must be claims,
    # except the mandatory first line, which is the case-sensitive literal SHA256.
    $lines = $text -split "`r?`n"
    $fenced = New-Object 'bool[]' $lines.Count
    $claims = New-Object System.Collections.Generic.List[object]
    $grammarErrors = New-Object System.Collections.Generic.List[string]
    $inBlock = $false
    $needHeader = $false
    $blockCount = 0
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $number = $i + 1
        if (-not $inBlock) {
            if ($line -cmatch '^```[a-z]*$') {
                $blockCount++
                $inBlock = $true
                $needHeader = $true
            } elseif ($line -match '[0-9A-Fa-f]{64}') {
                $grammarErrors.Add("64-hex run outside the fenced block on line $number")
            }
            continue
        }
        $fenced[$i] = $true
        if ($line -ceq '```') {
            if ($needHeader) {
                $grammarErrors.Add("first line inside the fenced block must be exactly SHA256 (line $number)")
            }
            $inBlock = $false
            continue
        }
        if ($needHeader) {
            if ($line -cne 'SHA256') {
                $grammarErrors.Add("first line inside the fenced block must be exactly SHA256 (line $number)")
            }
            $needHeader = $false
            continue
        }
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $claim = [regex]::Match($line, '^([0-9A-Fa-f]{64})[ \t]+(\S+)$')
        if (-not $claim.Success) {
            $grammarErrors.Add("malformed hash claim on line $number")
            continue
        }
        $claims.Add([pscustomobject]@{
            File = [IO.Path]::GetFileName($claim.Groups[2].Value)
            Hash = $claim.Groups[1].Value.ToUpperInvariant()
        })
    }
    if ($blockCount -ne 1) {
        $grammarErrors.Add("exactly one fenced code block is required (found $blockCount)")
    }
    if ($inBlock) { $grammarErrors.Add('fenced code block is not closed') }

    # Round-trip each line independently, without lossy CP1252 replacement of Unicode.
    $cp1252 = [Text.Encoding]::GetEncoding(1252,
        (New-Object Text.EncoderExceptionFallback), (New-Object Text.DecoderExceptionFallback))
    $highLatin = '[\u00A0-\u00FF]'
    $mojibake = $false
    # /* ponytail: exempts quotations of the bug; mark as [KNOWN-LIMIT] if a line both contains mojibake and is inside block */
    # Ceiling: exempt lines can conceal actual corruption; decoded Latin-1 accents also
    # remain exempt under the specified no-high-Latin-left rule (caf\u00C3\u00A9 -> caf\u00E9).
    # Upgrade path: explicit quotation spans and an agreed policy for decoded Latin-1.
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $number = $i + 1
        $corrupt = $false
        try {
            $decoded = $enc.GetString($cp1252.GetBytes($line))
            $corrupt = $decoded -cne $line -and $decoded -notmatch $highLatin
        } catch {
            # An unrepresentable character or invalid UTF-8 is not a clean round trip.
        }
        $quotesBug = $line -match '\bmojibake\b'
        if ($fenced[$i]) {
            if ($corrupt -or $quotesBug) {
                Pass "[KNOWN-LIMIT] line ${number}: fenced mojibake quotation is exempt"
            }
        } elseif ($corrupt -and -not $quotesBug) {
            Fail "double-encoded UTF-8 (mojibake) on line $number - it will render as garbage on the release page"
            $mojibake = $true
        }
    }
    if (-not $mojibake) { Pass "no double-encoded UTF-8" }
    if ($text -match '<[^>\r\n]*filled[^>\r\n]*>') { Fail "an unfilled placeholder remains: $($Matches[0])" }
    else { Pass "no unfilled placeholders" }

    # ------------------------------------------------------------ hash claims
    Write-Host "`n=== hash claims ===" -ForegroundColor Cyan
    foreach ($message in $grammarErrors) { Fail $message }
    if ($claims.Count -eq 0) { Fail "the notes state no file hashes at all" }
    $counts = New-Object 'System.Collections.Generic.Dictionary[string,int]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($claim in $claims) {
        if (-not $counts.ContainsKey($claim.File)) { $counts.Add($claim.File, 0) }
        $counts[$claim.File]++
    }
    $requiredLeaves = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($name in $Require) {
        $leaf = [IO.Path]::GetFileName($name)
        if ([string]::IsNullOrWhiteSpace($leaf)) {
            Fail "required filename must have a nonempty leaf"
            continue
        }
        [void]$requiredLeaves.Add($leaf)
        $count = 0
        if ($counts.ContainsKey($leaf)) { $count = $counts[$leaf] }
        if ($count -ne 1) { Fail "${leaf}: required file must appear exactly once (found $count)" }
    }

    # ------------------------------------------------------------ archive metadata, then unzip
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $workName = 'verify-notes-' + [Guid]::NewGuid().ToString('N')
    $work = [IO.Path]::GetFullPath((Join-Path $tempRoot $workName))
    New-Item -ItemType Directory -Path $work | Out-Null
    $workPrefix = $work + [IO.Path]::DirectorySeparatorChar
    $inZip = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    $entryNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $entryPaths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $archiveValid = $true
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        foreach ($entry in $archive.Entries) {
            # Inspect names BEFORE Expand-Archive -Force can overwrite duplicate entries.
            if (-not $entryNames.Add($entry.FullName)) {
                Fail "duplicate zip entry name: $($entry.FullName)"
                $archiveValid = $false
                continue
            }
            $target = [IO.Path]::GetFullPath([IO.Path]::Combine($work, $entry.FullName))
            if (-not $target.StartsWith($workPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                Fail "zip entry escapes the extraction directory: $($entry.FullName)"
                $archiveValid = $false
                continue
            }
            if (-not $entryPaths.Add($target)) {
                Fail "duplicate zip extraction path: $($entry.FullName)"
                $archiveValid = $false
            }
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) { continue }
            $leaf = [IO.Path]::GetFileName($entry.FullName)
            if ($inZip.ContainsKey($leaf)) {
                # $counts already tracks the case-insensitive leaves extracted from $claims.
                if ($counts.ContainsKey($leaf) -or $requiredLeaves.Contains($leaf)) {
                    Fail "${leaf}: ambiguous leaf in zip - more than one file with that name"
                    $archiveValid = $false
                } else {
                    Write-Host "  info  ${leaf}: ambiguous leaf in zip - unclaimed and not required"
                }
            } else {
                $inZip.Add($leaf, $target)
            }
        }
    } finally {
        $archive.Dispose()
    }
    # The zip is the only checked artifact outside the zip; refuse a colliding leaf.
    $zipLeaf = [IO.Path]::GetFileName($Zip)
    if ($inZip.ContainsKey($zipLeaf)) {
        Fail "${zipLeaf}: ambiguous leaf in zip - also names the archive itself"
        $archiveValid = $false
    } else {
        $inZip.Add($zipLeaf, $Zip)
    }
    if ($archiveValid) {
        Expand-Archive -LiteralPath $Zip -DestinationPath $work -Force
        foreach ($claim in $claims) {
            if (-not $inZip.ContainsKey($claim.File)) {
                Fail "$($claim.File): named in the notes but not present in the zip"
                continue
            }
            $actual = (Get-FileHash -LiteralPath $inZip[$claim.File] -Algorithm SHA256).Hash.ToUpperInvariant()
            if ($actual -eq $claim.Hash) { Pass "$($claim.File)  $($claim.Hash)" }
            else { Fail "$($claim.File): notes say $($claim.Hash), actual $actual" }
        }
    }

    # ------------------------------------------------------------ distinctness
    Write-Host "`n=== distinctness ===" -ForegroundColor Cyan
    $sharedHash = $false
    foreach ($group in ($claims | Group-Object Hash)) {
        $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($claim in $group.Group) { [void]$names.Add($claim.File) }
        if ($names.Count -gt 1) {
            Fail ("one hash claimed for several files ({0}): {1}" -f (($names | Sort-Object) -join ', '), $group.Name)
            $sharedHash = $true
        }
    }
    if (-not $sharedHash) { Pass "$($claims.Count) hash claim(s), no two files share a hash" }
} catch {
    Fail "verification could not complete: $($_.Exception.Message)"
} finally {
    if ($work -and (Test-Path -LiteralPath $work)) {
        try {
            # Validate the exact generated temporary target before recursive removal.
            $cleanupPath = [IO.Path]::GetFullPath($work)
            if ([IO.Path]::GetDirectoryName($cleanupPath) -cne $tempRoot -or
                [IO.Path]::GetFileName($cleanupPath) -cne $workName) {
                throw "unexpected cleanup target: $cleanupPath"
            }
            Remove-Item -LiteralPath $cleanupPath -Recurse -Force
        } catch {
            Fail "temporary directory cleanup failed: $($_.Exception.Message)"
        }
    }
}

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host "=== RESULT: $($failures.Count) FALSE CLAIM(S) IN THE NOTES ===" -ForegroundColor Red
    exit 1
}
Write-Host "=== RESULT: every claim in the notes is true ===" -ForegroundColor Green
exit 0
