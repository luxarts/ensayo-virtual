# A hosts the session in-process (--host); B joins the printed link, probes A's clicks and reports latency.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22151)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'embedded-host'
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $aLog = Join-Path $run 'A.csv'
    $bLog = Join-Path $run 'B.csv'
    $bStats = Join-Path $run 'B.stats.jsonl'

    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--host', '--port', "$Port", '--name', 'A', '--role', 'both',
        '--fake-audio', 'click', '--block', '128', '--emit-log', $aLog, '--duration', '12')
    $procs += $a
    $linkLine = Wait-ForLine $a '^link ensayo://' 15
    $link = ($linkLine -split '\s+')[1]
    Wait-ForLine $a '^joined ' 10 | Out-Null

    $b = Start-Proc -Name 'B' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'B', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--probe', $bLog, '--compare', $aLog,
        '--stats', $bStats, '--duration', '10')
    $procs += $b

    Wait-ForLine $b '^joined ' 10 | Out-Null
    Wait-ForLine $b '^participants 2$' 10 | Out-Null

    $bExit = Wait-Proc $b 25
    $aExit = Wait-Proc $a 25

    $bLines = Get-OutputLines $b
    $latencyLine = $bLines | Where-Object { $_ -match '^latency ' } | Select-Object -Last 1
    Write-Host "  B: $($bLines -join ' | ')"

    Assert ($bExit -eq 0) "B exited with code $bExit (stderr: $((Read-SharedFile $b.StdErr).Trim()))"
    Assert ($aExit -eq 0) "A exited with code $aExit (stderr: $((Read-SharedFile $a.StdErr).Trim()))"
    Assert (@($bLines | Where-Object { $_ -eq 'participants 2' }).Count -ge 1) 'B never printed "participants 2"'
    Assert ($null -ne $latencyLine) 'B printed no latency line'

    $lat = ConvertFrom-LatencyLine $latencyLine
    Write-Host "  $latencyLine"
    Assert ($lat.N -ge 10) "only $($lat.N) clicks paired (need >= 10)"
    Assert ($lat.Median -lt 40) "median latency $($lat.Median) ms (need < 40)"

    Write-Pass $name
    $status = 0
}
catch
{
    Write-Fail $name $_.Exception.Message
    $status = 1
}
finally
{
    Stop-Procs $procs
}

exit $status
