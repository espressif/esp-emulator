# esp-emu-relay installer / updater for Windows (GitHub Releases edition)
#
# Usage (one-liner, PowerShell):
#   irm https://raw.githubusercontent.com/espressif/esp-emulator/main/install-relay.ps1 | iex
#
# Usage (with options):
#   & ([scriptblock]::Create((irm https://.../install-relay.ps1))) -Version 1.0.0
#   & ([scriptblock]::Create((irm https://.../install-relay.ps1))) -BinDir C:\tools
#
# Drops esp-emu-relay.exe in $env:LOCALAPPDATA\Programs\esp-emu-relay by
# default and adds that folder to the user's PATH. install.sh --relay is the
# Linux and macOS counterpart. The relay has its own releases, relay-vX.Y.Z.

param(
    [string]$Version = $env:ESP_EMU_RELAY_VERSION,
    [string]$BinDir = $(if ($env:ESP_EMU_BIN_DIR) { $env:ESP_EMU_BIN_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\esp-emu-relay' }),
    [string]$Repo = $(if ($env:ESP_EMU_REPO) { $env:ESP_EMU_REPO } else { 'espressif/esp-emulator' }),
    [switch]$NoPath
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 defaults to TLS 1.0, which GitHub refuses
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# The progress bar slows Invoke-WebRequest down by an order of magnitude on 5.1
$ProgressPreference = 'SilentlyContinue'

$Base = "https://github.com/$Repo"
$Target = 'x86_64-pc-windows-gnu'

# The relay release this installs: scripts/relay-version.sh --check holds it to
# esp-emu-relay/Cargo.toml. /releases/latest is the emulator's, so no lookup.
$RelayVersion = '1.0.0'
if (-not $Version) { $Version = $RelayVersion }

$Name = "esp-emu-relay-$Version-$Target"
$Archive = "$Name.zip"
$Url = "$Base/releases/download/relay-v$Version/$Archive"
Write-Host "Target version: $Version"
Write-Host "Archive URL:    $Url"

$Tmp = Join-Path ([IO.Path]::GetTempPath()) ("esp-emu-relay-install-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
    $Zip = Join-Path $Tmp $Archive
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
    } catch {
        throw "download failed: $Url (the relay releases: $Base/releases?q=relay-v): $_"
    }

    # Older releases may have no SHA256SUMS entry: fall back to TLS alone
    $Expected = $null
    try {
        $Sums = (Invoke-WebRequest -UseBasicParsing -Uri "$Base/releases/download/relay-v$Version/SHA256SUMS").Content
        if ($Sums -is [byte[]]) { $Sums = [Text.Encoding]::UTF8.GetString($Sums) }
        foreach ($line in $Sums -split "`n") {
            $f = $line.Trim() -split '\s+', 2
            if ($f.Count -eq 2 -and $f[1].TrimStart('*') -eq $Archive) { $Expected = $f[0].ToLower() }
        }
    } catch { }
    if ($Expected) {
        $Got = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLower()
        if ($Got -ne $Expected) { throw "sha256 mismatch (expected $Expected, got $Got)" }
        Write-Host 'ok: sha256 verified'
    } else {
        Write-Warning "no published SHA256SUMS entry for $Archive - relying on TLS"
    }

    Expand-Archive -Path $Zip -DestinationPath $Tmp
    $Src = Join-Path $Tmp "$Name\esp-emu-relay.exe"
    if (-not (Test-Path $Src)) { throw 'esp-emu-relay.exe not found inside archive' }

    New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
    Copy-Item -Force $Src (Join-Path $BinDir 'esp-emu-relay.exe')
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}
Write-Host "ok: Installed esp-emu-relay $Version to $BinDir\esp-emu-relay.exe"

if (-not $NoPath) {
    $UserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (($UserPath -split ';') -notcontains $BinDir) {
        [Environment]::SetEnvironmentVariable('Path', ($(if ($UserPath) { "$UserPath;" } else { '' }) + $BinDir), 'User')
        $env:Path = "$env:Path;$BinDir"
        Write-Host "Added $BinDir to your user PATH; new terminals pick it up."
    }
}
Write-Host ''
Write-Host 'Run: esp-emu-relay, then connect from the Network panel of the browser emulator'
