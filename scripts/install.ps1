#Requires -Version 5.1
# astral-cli installer for Windows x64 (PowerShell 5.1+).
#
# Usage:
#   powershell -c "iwr -useb https://raw.githubusercontent.com/The-Astral-Express-Family/astral-cli/main/scripts/install.ps1 | iex"
#   or download and run: .\install.ps1
#
# Environment overrides:
#   ASTRAL_VERSION      tag to install (e.g. v0.2.1); default: latest GitHub release
#   ASTRAL_API_BASE     GitHub API base;      default: https://api.github.com
#   ASTRAL_DOWNLOAD_BASE  download base;      default: https://github.com
#
# Proxy: Invoke-WebRequest / Invoke-RestMethod use the system (WinINET) proxy
# configuration by default, so no extra configuration is needed in most setups.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is very slow in 5.1 with the progress bar

# GitHub requires TLS 1.2; older .NET defaults may not offer it.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$RepoOwner = 'The-Astral-Express-Family'
$RepoName = 'astral-cli'
$ApiBase = if ($env:ASTRAL_API_BASE) { $env:ASTRAL_API_BASE.TrimEnd('/') } else { 'https://api.github.com' }
$DownloadBase = if ($env:ASTRAL_DOWNLOAD_BASE) { $env:ASTRAL_DOWNLOAD_BASE.TrimEnd('/') } else { 'https://github.com' }

# --- 1. Architecture guard ---------------------------------------------------

if (-not [Environment]::Is64BitProcess) {
    throw 'astral requires 64-bit Windows. Please use a 64-bit PowerShell.'
}
if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
    throw 'ARM64 Windows is not supported yet'
}

# --- 2. Resolve version ------------------------------------------------------

$tag = $env:ASTRAL_VERSION
if ([string]::IsNullOrWhiteSpace($tag)) {
    $release = Invoke-RestMethod -Uri "$ApiBase/repos/$RepoOwner/$RepoName/releases/latest" `
        -Headers @{ Accept = 'application/vnd.github+json' }
    $tag = $release.tag_name
}
if ([string]::IsNullOrWhiteSpace($tag)) {
    throw 'Could not determine the version to install (set ASTRAL_VERSION to override).'
}
$version = $tag.TrimStart('v')
$zipName = "astral-$version-windows-x64.zip"

# --- 3. Download archive + checksums to a temp dir ---------------------------

$tempDir = Join-Path ([IO.Path]::GetTempPath()) ("astral-install-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
$zipPath = Join-Path $tempDir $zipName
$sumsPath = Join-Path $tempDir 'SHA256SUMS.txt'

try {
    Write-Host "Downloading astral $tag for Windows x64..."
    Invoke-WebRequest -Uri "$DownloadBase/$RepoOwner/$RepoName/releases/download/$tag/$zipName" -OutFile $zipPath -UseBasicParsing
    Invoke-WebRequest -Uri "$DownloadBase/$RepoOwner/$RepoName/releases/download/$tag/SHA256SUMS.txt" -OutFile $sumsPath -UseBasicParsing

    # --- 4. Verify SHA256 (case-insensitive) ---------------------------------

    $expected = $null
    foreach ($line in (Get-Content -Path $sumsPath)) {
        $parts = $line -split '\s+', 2
        if ($parts.Count -eq 2) {
            # sha256sum format: "<hash>  <name>" ("*" prefix = binary mode)
            $name = $parts[1].Trim().TrimStart('*')
            if ($name -ieq $zipName) { $expected = $parts[0]; break }
        }
    }
    if (-not $expected) {
        throw "SHA256SUMS.txt does not contain an entry for $zipName"
    }

    $actual = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash
    if ($actual -ine $expected) {
        throw "Checksum mismatch for $zipName`nExpected: $expected`nActual:   $actual"
    }
    Write-Host 'Checksum OK.'

    # --- 5. Extract to install dir -------------------------------------------

    $installDir = Join-Path $env:LOCALAPPDATA 'Programs\astral'
    $target = Join-Path $installDir 'astral.exe'
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null

    $extractDir = Join-Path $tempDir 'extract'
    Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force
    $newExe = Get-ChildItem -Path $extractDir -Recurse -Filter 'astral.exe' | Select-Object -First 1
    if (-not $newExe) {
        throw "Archive $zipName does not contain astral.exe"
    }

    # --- 6. Overwrite, tolerating a running astral.exe ------------------------

    if (Test-Path -LiteralPath $target) {
        # Renaming a running executable is allowed on Windows; move the old
        # binary aside first, then move the new one into place.
        Move-Item -LiteralPath $target -Destination "$target.old" -Force
        try {
            Move-Item -LiteralPath $newExe.FullName -Destination $target -Force
        } catch {
            # Put the old binary back rather than leaving no astral.exe at all.
            Move-Item -LiteralPath "$target.old" -Destination $target -Force
            throw
        }
        Remove-Item -LiteralPath "$target.old" -Force -ErrorAction SilentlyContinue
    } else {
        Move-Item -LiteralPath $newExe.FullName -Destination $target -Force
    }

    # --- 7. Append install dir to the user PATH (never reorder) ---------------

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $entries = @()
    if ($userPath) { $entries = $userPath -split ';' | Where-Object { $_ -ne '' } }
    $present = $false
    foreach ($entry in $entries) {
        if ($entry.TrimEnd('\') -ieq $installDir.TrimEnd('\')) { $present = $true; break }
    }
    if (-not $present) {
        $newPath = if ($entries.Count -gt 0) { ($entries -join ';') + ';' + $installDir } else { $installDir }
        [Environment]::SetEnvironmentVariable('Path', $newPath, 'User')
        Write-Host "Added $installDir to your user PATH. Restart your terminal for it to take effect."
    }
} finally {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}

# --- 8. Success --------------------------------------------------------------

Write-Host ''
Write-Host "astral $tag installed successfully: $target"
