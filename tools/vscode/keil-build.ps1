param(
    [ValidateSet("build", "rebuild", "clean")]
    [string]$Mode = "build",

    [string]$Target = "Target 1"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Project = Join-Path $Root "Project.uvprojx"
$KeilRoot = $env:KEIL_ROOT
if ([string]::IsNullOrWhiteSpace($KeilRoot)) {
    throw "Set KEIL_ROOT to your Keil installation directory before building."
}
$Uv4 = Join-Path $KeilRoot "UV4\UV4.exe"
$LogDir = Join-Path $Root "Objects"
$LogFile = Join-Path $LogDir "vscode_$Mode.log"

if (!(Test-Path -LiteralPath $Uv4)) {
    throw "Cannot find Keil uVision: $Uv4"
}

if (!(Test-Path -LiteralPath $Project)) {
    throw "Cannot find project file: $Project"
}

if (!(Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
}
Remove-Item -LiteralPath $LogFile -Force -ErrorAction SilentlyContinue

$ModeArg = @{
    build = "-b"
    rebuild = "-r"
    clean = "-c"
}[$Mode]

Write-Host "Keil $Mode`: $Project"
Write-Host "Target: $Target"
Write-Host ""

& $Uv4 $ModeArg $Project -t $Target -j0 -o $LogFile
$KeilExitCode = $LASTEXITCODE

$content = $null
# uVision can return before its build worker has finished writing the log.
for ($wait = 0; $wait -lt 600; $wait++) {
    if (Test-Path -LiteralPath $LogFile) {
        try {
            $bytes = [System.IO.File]::ReadAllBytes($LogFile)
            $candidate = [System.Text.Encoding]::Default.GetString($bytes)
            if ($candidate -match "Build Time Elapsed") {
                $content = $candidate
                break
            }
        }
        catch [System.IO.IOException] { }
    }
    Start-Sleep -Milliseconds 100
}
if ($null -eq $content) {
    throw "Keil did not finish writing the build log: $LogFile"
}

if ($null -ne $content) {
    $content = [System.Net.WebUtility]::HtmlDecode($content)
    $content = $content -replace "<[^>]+>", ""
    $content = $content -replace "`r", ""

    $interesting = $content -split "`n" | Where-Object {
        $_ -match "compiling|assembling|linking|Program Size|Error\(s\)|Warning\(s\)|error:|warning:|\.\\Objects\\Project\.axf"
    }

    if ($interesting) {
        $interesting | ForEach-Object { Write-Host $_ }
    }

    Write-Host ""
    Write-Host "Full log: $LogFile"
}

if ($KeilExitCode -ne 0) {
    exit $KeilExitCode
}

if ($Mode -ne "clean") {
    $errorMatch = [regex]::Match($content, "(\d+)\s+Error\(s\)")
    if (!$errorMatch.Success -or [int]$errorMatch.Groups[1].Value -gt 0) {
        throw "Build failed or missing result summary; inspect the local log."
    }
}
