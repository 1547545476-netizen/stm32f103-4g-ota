param(
    [string]$Port = "auto",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$MainFile = Join-Path $Root "AppTest\main.c"
$BinFile = Join-Path $Root "AppTest\Objects\AppTest.bin"
$Sender = Join-Path $Root "tools\ota\serial_ota_sender.py"

# Windows may assign a new COM number after the USB adapter is moved.
# In auto mode, use the only port; require an explicit choice when several exist.
if ($Port -eq "auto") {
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($ports.Count -eq 0) {
        throw "No serial port found. Reconnect the USB-to-TTL adapter."
    }
    if ($ports.Count -gt 1) {
        throw ("Multiple serial ports found: {0}. Enter the required COM port in the task prompt." -f ($ports -join ", "))
    }
    $Port = $ports[0]
    Write-Host "Auto-detected serial port: $Port"
}

if (!(Test-Path -LiteralPath $BinFile)) {
    throw "APP bin does not exist. Run APP: Build BIN first."
}

# APP_VERSION controls both the APP log and the version stored in AT24C02.
# Read it from source so the sender metadata cannot silently disagree.
$mainText = Get-Content -LiteralPath $MainFile -Raw -Encoding UTF8
$versionMatch = [regex]::Match($mainText, '#define\s+APP_VERSION\s+(\d+)(?:U?L?)')
if (!$versionMatch.Success) {
    throw "Cannot read APP_VERSION from $MainFile"
}
$version = [int]$versionMatch.Groups[1].Value

Write-Host "Serial install APP v$version through $Port from $BinFile"
if ($DryRun) {
    Write-Host "Dry run complete; serial sender was not started."
    return
}

& python $Sender --port $Port --bin $BinFile --version $version
if ($LASTEXITCODE -ne 0) {
    throw "Serial OTA sender failed with exit code $LASTEXITCODE"
}
