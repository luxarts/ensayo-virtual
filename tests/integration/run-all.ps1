# Runs every integration script in its own PowerShell process and prints a summary table.
# Exit code: 0 when nothing failed (SKIP counts as not failed), 1 otherwise.
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'

$scripts = @('two-instances', 'embedded-host', 'backing-track', 'packet-loss', 'screenshots')
$results = @()
$anyFailed = $false

foreach ($script in $scripts)
{
    $path = Join-Path $PSScriptRoot "$script.ps1"
    Write-Host ""
    Write-Host ("=== {0} ===" -f $script) -ForegroundColor Cyan

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $path -BuildDir $BuildDir -Config $Config 2>&1 | ForEach-Object { "$_" }
    $exit = $LASTEXITCODE
    $sw.Stop()

    $output | ForEach-Object { Write-Host $_ }

    $verdict = @($output | Where-Object { $_ -match '^(PASS|FAIL|SKIP) ' }) | Select-Object -Last 1
    $statusWord = 'FAIL'
    $detail = ''
    if ($null -ne $verdict)
    {
        $statusWord = ($verdict -split ' ')[0]
        if ($verdict -match '^\w+ \S+:\s*(.*)$') { $detail = $Matches[1] }
    }
    if ($exit -ne 0 -and $statusWord -ne 'FAIL') { $statusWord = 'FAIL'; $detail = "exit code $exit" }
    if ($statusWord -eq 'FAIL') { $anyFailed = $true }

    $latency = @($output | Where-Object { $_ -match '^\s*latency ' }) | Select-Object -Last 1
    if ($null -ne $latency -and $detail -eq '') { $detail = $latency.Trim() }

    $results += [PSCustomObject] @{ Script = $script; Status = $statusWord; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); Detail = $detail }
}

Write-Host ""
Write-Host "=== summary ===" -ForegroundColor Cyan
$results | Format-Table -AutoSize Script, Status, Seconds, Detail | Out-String -Width 200 | Write-Host

if ($anyFailed) { Write-Host 'Some integration scripts FAILED' -ForegroundColor Red; exit 1 }
Write-Host 'All integration scripts passed' -ForegroundColor Green
exit 0
