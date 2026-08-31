# Game Optimizer Release Staging Script
#
# Stages a release package from a manifest, verifying all dependencies are present
# and the staged folder matches the declared list exactly.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\stage-release.ps1 -Version 0.2.0
#
# The WebView2Loader.dll is loaded dynamically and does not appear in the exe's import table.
# This script's manifest is the only record that it must ship. The verification is stringent:
# every source path must exist before any copy begins, the staged folder must match the manifest
# exactly, and the DLL must sit in the same directory as the exe.

param(
    [Parameter(Mandatory=$true)]
    [string]$Version,
    [string]$OutDir = ""
)

$ErrorActionPreference = 'Stop'

# Resolve paths
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ManifestPath = Join-Path $PSScriptRoot "release-manifest.txt"
$ExePath = Join-Path $RepoRoot "build\GameOptimizer.exe"

# Determine output directory
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $RepoRoot "build\release"
}

# Create output directory if needed
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

$StagingDir = Join-Path $OutDir "GameOptimizer"
$ZipPath = Join-Path $OutDir "GameOptimizer-v$Version-x64.zip"

# a. Read and validate manifest
Write-Host "Reading manifest from $ManifestPath"
if (-not (Test-Path $ManifestPath)) {
    Write-Error "ABORT: manifest not found at $ManifestPath"
    exit 1
}

$ManifestLines = @()
foreach ($line in (Get-Content $ManifestPath -Raw).Split("`n")) {
    $trimmed = $line.Trim()
    # Skip empty lines and comments
    if ($trimmed -and -not $trimmed.StartsWith("#")) {
        $ManifestLines += $trimmed
    }
}

if ($ManifestLines.Count -eq 0) {
    Write-Error "ABORT: manifest is empty"
    exit 1
}

Write-Host "Manifest contains $($ManifestLines.Count) entries"

# Parse manifest into hashtable: source -> destination
$ManifestMap = @{}
foreach ($line in $ManifestLines) {
    $parts = $line -split '\s*->\s*', 2
    if ($parts.Count -ne 2) {
        Write-Error "ABORT: malformed manifest line: $line"
        exit 1
    }
    $ManifestMap[$parts[0].Trim()] = $parts[1].Trim()
}

# b. Verify ALL source paths exist before touching anything
Write-Host "Verifying all source paths exist..."
$MissingPaths = @()
foreach ($sourcePath in $ManifestMap.Keys) {
    $fullPath = Join-Path $RepoRoot $sourcePath
    if (-not (Test-Path $fullPath)) {
        $MissingPaths += $sourcePath
    }
}

if ($MissingPaths.Count -gt 0) {
    Write-Error "ABORT: missing source files:"
    foreach ($missing in $MissingPaths) {
        Write-Error "  $missing"
    }
    exit 1
}

Write-Host "All source paths verified"

# c. Verify exe version matches -Version parameter
Write-Host "Checking exe version..."
if (-not (Test-Path $ExePath)) {
    Write-Error "ABORT: built exe not found at $ExePath"
    exit 1
}

$ExeVersion = (Get-Item $ExePath).VersionInfo.FileVersion
Write-Host "Exe version: $ExeVersion, Parameter version: $Version"

# Normalize versions for comparison: compare the major.minor.patch components
$ExeParts = $ExeVersion -split '\.' | Select-Object -First 3
$ParamParts = $Version -split '\.' | Select-Object -First 3
$ExeNormalized = $ExeParts -join '.'
$ParamNormalized = $ParamParts -join '.'

if ($ExeNormalized -ne $ParamNormalized) {
    Write-Error "ABORT: version mismatch - exe has $ExeNormalized but -Version is $ParamNormalized"
    exit 1
}

# d. Create clean staging folder
Write-Host "Creating staging folder at $StagingDir"
if (Test-Path $StagingDir) {
    Remove-Item -Recurse -Force $StagingDir | Out-Null
}
New-Item -ItemType Directory -Path $StagingDir | Out-Null

Write-Host "Copying files..."
foreach ($sourcePath in $ManifestMap.Keys) {
    $fullSourcePath = Join-Path $RepoRoot $sourcePath
    $destPath = $ManifestMap[$sourcePath]
    $fullDestPath = Join-Path $StagingDir $destPath

    # Create destination directory if needed
    $destDir = Split-Path -Parent $fullDestPath
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir | Out-Null
    }

    Copy-Item -Path $fullSourcePath -Destination $fullDestPath -Force
    Write-Host "  $sourcePath -> $destPath"
}

# e. Verify staged folder matches manifest exactly (no missing, no extras)
Write-Host "Verifying staged folder..."
$StagedFiles = @{}
Get-ChildItem -Recurse -File $StagingDir | ForEach-Object {
    $relPath = $_.FullName.Substring($StagingDir.Length + 1)
    $StagedFiles[$relPath] = $_.FullName
}

$ManifestDestinations = @($ManifestMap.Values)

# Check for missing files (in manifest but not staged)
$Missing = @()
foreach ($destPath in $ManifestDestinations) {
    # Normalize path for comparison (convert to backslashes)
    $normalizedDest = $destPath.Replace("/", "\")
    $found = $false
    foreach ($stagedPath in $StagedFiles.Keys) {
        if ($stagedPath.Replace("/", "\") -eq $normalizedDest) {
            $found = $true
            break
        }
    }
    if (-not $found) {
        $Missing += $destPath
    }
}

if ($Missing.Count -gt 0) {
    Write-Error "ABORT: files in manifest but not in staged folder:"
    foreach ($m in $Missing) {
        Write-Error "  $m"
    }
    exit 1
}

# Check for extra files (staged but not in manifest)
$Extras = @()
foreach ($stagedPath in $StagedFiles.Keys) {
    $normalizedStaged = $stagedPath.Replace("/", "\")
    $found = $false
    foreach ($destPath in $ManifestDestinations) {
        if ($destPath.Replace("/", "\") -eq $normalizedStaged) {
            $found = $true
            break
        }
    }
    if (-not $found) {
        $Extras += $stagedPath
    }
}

if ($Extras.Count -gt 0) {
    Write-Error "ABORT: extra files in staged folder (not in manifest):"
    foreach ($e in $Extras) {
        Write-Error "  $e"
    }
    exit 1
}

Write-Host "Staged folder verified: exactly matches manifest"

# f. Assert WebView2Loader.dll is in same directory as GameOptimizer.exe
Write-Host "Verifying WebView2Loader.dll placement..."
$StagedExePath = Join-Path $StagingDir "GameOptimizer.exe"
$StagedLoaderPath = Join-Path $StagingDir "WebView2Loader.dll"

if (-not (Test-Path $StagedExePath)) {
    Write-Error "ABORT: staged exe not found at $StagedExePath"
    exit 1
}

if (-not (Test-Path $StagedLoaderPath)) {
    Write-Error "ABORT: WebView2Loader.dll not in root directory beside exe"
    exit 1
}

Write-Host "WebView2Loader.dll confirmed in same directory as GameOptimizer.exe"

# g. Compress to zip
Write-Host "Creating zip: $ZipPath"
if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($StagingDir, $ZipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)

# h. Print summary
Write-Host ""
Write-Host "=== STAGING SUMMARY ==="
Write-Host ""

$TotalSize = 0
foreach ($sourcePath in $ManifestMap.Keys) {
    $destPath = $ManifestMap[$sourcePath]
    $fullSourcePath = Join-Path $RepoRoot $sourcePath
    $fileSize = (Get-Item $fullSourcePath).Length
    $TotalSize += $fileSize
    $sizeKB = [math]::Round($fileSize / 1KB, 2)
    Write-Host "  $destPath : $sizeKB KB"
}

$ZipSize = (Get-Item $ZipPath).Length
$ZipSizeKB = [math]::Round($ZipSize / 1KB, 2)
$ZipSizeMB = [math]::Round($ZipSize / 1MB, 2)
Write-Host ""
Write-Host "Package contents total: $([math]::Round($TotalSize / 1KB, 2)) KB"
Write-Host "Zip file: $ZipPath"
Write-Host "Zip size: $ZipSizeMB MB ($ZipSizeKB KB)"

# Calculate SHA256
$SHA256 = (Get-FileHash $ZipPath -Algorithm SHA256).Hash
Write-Host "SHA256:   $SHA256"
Write-Host ""
Write-Host "Staging complete: GameOptimizer-v$Version-x64.zip"
