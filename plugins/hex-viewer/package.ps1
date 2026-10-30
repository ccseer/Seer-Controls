# Packaging script for the Seer Hex View Control Plugin.
# Produces hex-viewer-1.0.0.zip with a flat package root, mirroring the OCR
# package's layout: dist\package-stage holds the staged tree, dist\ holds the
# archive.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File package.ps1
#   powershell -ExecutionPolicy Bypass -File package.ps1 -AppMinVersion 4.8.0
#
# -AppMinVersion is available for a release package against a development
# build: development builds report version 0.0.1, so a pinned manifest would
# be rejected by the installer.

param(
    [string]$AppMinVersion = "",
    # Optional: use an existing binary (e.g. from the out-of-repo CMake preset)
    # instead of building into .\build.
    [string]$ExePath = "",
    # Strip appMinVersion and emit a -dev zip: a development build of Seer
    # reports 0.0.1, and the installer hard-rejects any pinned minimum above it.
    [switch]$Dev
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot is reliable under -File, dot-sourcing and `& .\package.ps1`;
# $MyInvocation.MyCommand.Path is not always populated by host wrappers, and a
# wrong $scriptDir silently stages the payload from the caller's directory.
$scriptDir = $PSScriptRoot
if (-not $scriptDir -or -not (Test-Path $scriptDir)) {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if (-not $scriptDir -or -not (Test-Path $scriptDir)) {
    throw "Cannot locate the plugin directory; run package.ps1 from its own folder."
}
Set-Location $scriptDir
Write-Host "Plugin directory: $scriptDir" -ForegroundColor DarkGray
if (!(Test-Path (Join-Path $scriptDir "plugin.json"))) {
    throw "plugin.json not found under $scriptDir - refusing to stage from the wrong directory."
}

Write-Host "=== Step 0: Checking payload ===" -ForegroundColor Cyan
if (!(Test-Path (Join-Path $scriptDir "PACKAGE_README.md"))) {
    throw "Missing payload file: PACKAGE_README.md"
}
if (!(Test-Path (Join-Path $scriptDir "syntax\hextxt.xml"))) {
    throw "Missing payload file: syntax\hextxt.xml"
}

Write-Host "=== Step 1: Locating seer_hex.exe ===" -ForegroundColor Cyan
if ($ExePath -eq "") {
    $outOfRepoDir = Join-Path $env:USERPROFILE "Dev\build_output\Seer-Controls\hex-viewer"
    $outOfRepoCandidates = @(
        (Join-Path $outOfRepoDir "seer_hex.exe"),
        (Join-Path $outOfRepoDir "Release\seer_hex.exe")
    )
    foreach ($cand in $outOfRepoCandidates) {
        if (Test-Path $cand) {
            $ExePath = $cand
            Write-Host "Using pre-built binary from: $ExePath" -ForegroundColor Green
            break
        }
    }
}

if ($ExePath -eq "") {
    try {
        Write-Host "Building via cmake --preset default..."
        cmake --preset default
        cmake --build --preset default
        $cand = Join-Path $env:USERPROFILE "Dev\build_output\Seer-Controls\hex-viewer\seer_hex.exe"
        if (Test-Path $cand) {
            $ExePath = $cand
        }
    }
    catch {
        Write-Host "Preset build failed or compiler not on PATH." -ForegroundColor Yellow
    }
}

if ($ExePath -eq "" -or !(Test-Path $ExePath)) {
    throw "Build failed: seer_hex.exe not found. Build it via 'cmake --preset default && cmake --build --preset default' or pass -ExePath <path>."
}

Write-Host "=== Step 2: Staging package layout ===" -ForegroundColor Cyan
$stageDir = Join-Path $scriptDir "dist\package-stage"
if (Test-Path $stageDir) {
    # Plain .NET delete: the staged tree is a build artifact we just produced,
    # and Remove-Item can be routed through a trash helper that fails here.
    [System.IO.Directory]::Delete($stageDir, $true)
}
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

Copy-Item "plugin.json" -Destination $stageDir -Force
Copy-Item "PACKAGE_README.md" -Destination (Join-Path $stageDir "README.md") -Force
Copy-Item $ExePath -Destination $stageDir -Force
# The HexDump definition travels with the package: Seer's text viewer loads
# <package-root>\syntax\*.xml through the syntax contribution mechanism.
New-Item -ItemType Directory -Path (Join-Path $stageDir "syntax") -Force | Out-Null
Copy-Item "syntax\hextxt.xml" -Destination (Join-Path $stageDir "syntax") -Force
# No res\ payload: the control bar takes the button icon from the helper
# executable's embedded icon resource, exactly like the OCR package.

$manifestPath = Join-Path $stageDir "plugin.json"
# Windows PowerShell's -Encoding UTF8 writes a BOM, which Qt's JSON reader
# rejects; always emit BOM-less UTF-8.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-Manifest($obj) {
    [System.IO.File]::WriteAllText($manifestPath, ($obj | ConvertTo-Json -Depth 10), $utf8NoBom)
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
if ($Dev) {
    $manifest.PSObject.Properties.Remove("appMinVersion")
    Write-Manifest $manifest
    Write-Host "Stripped appMinVersion (dev package for a 0.0.1 Seer build)" -ForegroundColor Yellow
}
elseif ($AppMinVersion -ne "") {
    $manifest | Add-Member -NotePropertyName "appMinVersion" -NotePropertyValue $AppMinVersion -Force
    Write-Manifest $manifest
    Write-Host "Injected appMinVersion=$AppMinVersion into the staged manifest" -ForegroundColor Green
}
else {
    Write-Host "Staged manifest with appMinVersion=$($manifest.appMinVersion)" -ForegroundColor Green
}

Write-Host "=== Step 3: Compressing to hex-viewer-1.0.0.zip ===" -ForegroundColor Cyan
$suffix = if ($Dev) { "-dev" } else { "" }
$zipFile = Join-Path $scriptDir "dist\hex-viewer-1.0.0$suffix.zip"
# ZipFile::CreateFromDirectory refuses to overwrite; build under a scratch
# name and swap it in.
$tmpZip = Join-Path $scriptDir ("dist\hex-viewer-1.0.0$suffix.zip.tmp")
if (Test-Path $tmpZip) {
    [System.IO.File]::Delete($tmpZip)
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
# Filesystem-backed trees can hand back a transient FileNotFound while a file
# is hydrating (OneDrive); retry before giving up on an otherwise valid stage.
$zipDone = $false
for ($attempt = 1; $attempt -le 3 -and -not $zipDone; $attempt++) {
    try {
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $stageDir,
            $tmpZip,
            [System.IO.Compression.CompressionLevel]::Optimal,
            $false
        )
        $zipDone = $true
    }
    catch {
        if (Test-Path $tmpZip) { [System.IO.File]::Delete($tmpZip) }
        if ($attempt -eq 3) { throw }
        Write-Host "Zip attempt $attempt failed ($($_.Exception.Message)); retrying..." -ForegroundColor Yellow
        Start-Sleep -Seconds 2
    }
}

if (Test-Path $zipFile) {
    [System.IO.File]::Delete($zipFile)
}
[System.IO.File]::Move($tmpZip, $zipFile)

$zipInfo = Get-Item $zipFile
Write-Host "Package successfully created: $($zipInfo.FullName) ($([Math]::Round($zipInfo.Length / 1KB, 2)) KB)" -ForegroundColor Green

Write-Host "=== Step 4: Verifying archive root entries ===" -ForegroundColor Cyan
$archive = [System.IO.Compression.ZipFile]::OpenRead($zipFile)
$archive.Entries | Select-Object -Property FullName, CompressedLength
$archive.Dispose()

Write-Host "=== Done! The zip is ready for installation in Seer ===" -ForegroundColor Green
