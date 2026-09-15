param(
    [ValidateSet("build", "rebuild")]
    [string]$Mode = "rebuild"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$AppDir = Join-Path $Root "AppTest"
$Project = Join-Path $AppDir "AppTest.uvprojx"
$Objects = Join-Path $AppDir "Objects"
$Axf = Join-Path $Objects "AppTest.axf"
$Bin = Join-Path $Objects "AppTest.bin"
$Log = Join-Path $Objects "vscode_app_build.log"
$KeilRoot = $env:KEIL_ROOT
if ([string]::IsNullOrWhiteSpace($KeilRoot)) {
    throw "Set KEIL_ROOT to your Keil installation directory before building."
}
$Uv4 = Join-Path $KeilRoot "UV4\UV4.exe"
$FromElf = Join-Path $KeilRoot "ARM\ARMCC\Bin\fromelf.exe"
$ModeArg = if ($Mode -eq "rebuild") { "-r" } else { "-b" }

if (!(Test-Path -LiteralPath $Project)) {
    throw "Cannot find TestApp project: $Project"
}
if (!(Test-Path -LiteralPath $Uv4)) {
    throw "Cannot find Keil uVision: $Uv4"
}
if (!(Test-Path -LiteralPath $FromElf)) {
    throw "Cannot find fromelf: $FromElf"
}

New-Item -ItemType Directory -Force -Path $Objects | Out-Null
Remove-Item -LiteralPath $Log -Force -ErrorAction SilentlyContinue

Write-Host "Keil TestApp $Mode`: $Project"
& $Uv4 $ModeArg $Project -t "AppTest" -j0 -o $Log
$KeilExitCode = $LASTEXITCODE

# uVision can return while its background process still owns and writes the log.
# Poll until the file can be opened and contains Keil's final elapsed-time line.
$logText = $null
for ($wait = 0; $wait -lt 200; $wait++) {
    if (Test-Path -LiteralPath $Log) {
        try {
            $logBytes = [System.IO.File]::ReadAllBytes($Log)
            $candidate = [System.Text.Encoding]::Default.GetString($logBytes)
            if ($candidate -match "Build Time Elapsed") {
                $logText = $candidate
                break
            }
        }
        catch [System.IO.IOException] {
            # Keil is still writing the file; retry shortly.
        }
    }
    Start-Sleep -Milliseconds 100
}
if ($null -eq $logText) {
    throw "Keil did not finish writing build log: $Log"
}

$logText = [System.Net.WebUtility]::HtmlDecode($logText) -replace "<[^>]+>", ""
$logText | Select-String -Pattern "Program Size|Error\(s\)|Warning\(s\)|error:|warning:"

$errorMatch = [regex]::Match($logText, "(\d+)\s+Error\(s\)")
if (!$errorMatch.Success -or ([int]$errorMatch.Groups[1].Value -ne 0)) {
    throw "TestApp build failed. See $Log"
}
if (!(Test-Path -LiteralPath $Axf)) {
    throw "TestApp AXF was not generated: $Axf"
}

& $FromElf --bin --output $Bin $Axf
if ($LASTEXITCODE -ne 0) {
    throw "fromelf failed to generate TestApp.bin"
}

$image = [System.IO.File]::ReadAllBytes($Bin)
if ($image.Length -lt 8) {
    throw "TestApp.bin is too small to contain a vector table"
}

$stackTop = [System.BitConverter]::ToUInt32($image, 0)
$resetHandler = [System.BitConverter]::ToUInt32($image, 4)
$resetAddress = $resetHandler -band 0xFFFFFFFE

if (($stackTop -lt 0x20000000) -or ($stackTop -gt 0x20005000)) {
    throw ("Invalid APP stack top: 0x{0:X8}" -f $stackTop)
}
if (($resetHandler -band 1) -eq 0) {
    throw ("APP Reset_Handler is not a Thumb address: 0x{0:X8}" -f $resetHandler)
}
if (($resetAddress -lt 0x08005000) -or ($resetAddress -ge 0x08010000)) {
    throw ("APP Reset_Handler is outside APP partition: 0x{0:X8}" -f $resetHandler)
}
if ($image.Length -gt 0xB000) {
    throw "TestApp.bin exceeds the 44KB APP partition"
}

Write-Host ""
Write-Host "TestApp.bin ready: $Bin"
Write-Host ("Size: {0} bytes" -f $image.Length)
Write-Host ("Initial MSP: 0x{0:X8}" -f $stackTop)
Write-Host ("Reset_Handler: 0x{0:X8}" -f $resetHandler)
