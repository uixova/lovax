# Lume installer for Windows (PowerShell).
#
#   irm https://raw.githubusercontent.com/uixova/lume/main/install.ps1 | iex
#
# Env: $env:LUME_CHANNEL = "stable" | "latest"
$ErrorActionPreference = "Stop"

$Repo    = "uixova/lume"
$Channel = if ($env:LUME_CHANNEL) { $env:LUME_CHANNEL } else { "stable" }
$Dir     = Join-Path $env:LOCALAPPDATA "Lume\bin"

$arch = if ([Environment]::Is64BitOperatingSystem) { "x64" } else { "x86" }
$asset = "lume-windows-$arch.exe"

$api = "https://api.github.com/repos/$Repo/releases"
if ($Channel -eq "latest") {
    $tag = (Invoke-RestMethod "$api")[0].tag_name
} else {
    $tag = (Invoke-RestMethod "$api/latest").tag_name
}
if (-not $tag) { throw "No release found (channel: $Channel)." }

$url = "https://github.com/$Repo/releases/download/$tag/$asset"
Write-Host "Lume $tag - downloading $asset..."

New-Item -ItemType Directory -Force -Path $Dir | Out-Null
$out = Join-Path $Dir "lume.exe"
Invoke-WebRequest -Uri $url -OutFile $out

# Verify checksum if published
try {
    $sum = (Invoke-WebRequest -Uri "$url.sha256" -UseBasicParsing).Content.Split(" ")[0]
    $actual = (Get-FileHash $out -Algorithm SHA256).Hash.ToLower()
    if ($sum -ne $actual) { throw "checksum mismatch" }
    Write-Host "checksum verified."
} catch { }

# Add to user PATH if missing
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$Dir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$Dir", "User")
    Write-Host "Added $Dir to your PATH (restart the terminal to use 'lume')."
}
Write-Host "Installed lume $tag to $out"
