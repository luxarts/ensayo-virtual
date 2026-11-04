# Like two-instances, but B drops 10 % of the received blocks (--simulate-loss 10): it must survive (exit 0) and
# still pair clicks through packet-loss concealment.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22153, [int] $LossPercent = 10)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'packet-loss'
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $aLog = Join-Path $run 'A.csv'
    $bLog = Join-Path $run 'B.csv'
    $bStats = Join-Path $run 'B.stats.jsonl'

    $server = Start-ServerProc -FilePath $bin.Server -Port $Port -RunDir $run
    $procs += $server

    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', "ensayo://127.0.0.1:$Port/NEW", '--name', 'A', '--role', 'both',
        '--fake-audio', 'click', '--block', '128', '--emit-log', $aLog, '--duration', '12')
    $procs += $a
    $linkLine = Wait-ForLine $a '^link ensayo://' 15
    $link = ($linkLine -split '\s+')[1]
    Wait-ForLine $a '^joined ' 10 | Out-Null

    $b = Start-Proc -Name 'B' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'B', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--probe', $bLog, '--compare', $aLog,
        '--stats', $bStats, '--simulate-loss', "$LossPercent", '--duration', '10')
    $procs += $b

    Wait-ForLine $b '^joined ' 10 | Out-Null
    Wait-ForLine $b '^participants 2$' 10 | Out-Null

    $bExit = Wait-Proc $b 25
    $aExit = Wait-Proc $a 25

    $bLines = Get-OutputLines $b
    $latencyLine = $bLines | Where-Object { $_ -match '^latency ' } | Select-Object -Last 1
    Write-Host "  B: $($bLines -join ' | ')"

    Assert ($bExit -eq 0) "B exited with code $bExit (stderr: $((Read-SharedFile $b.StdErr).Trim()))"
    Assert ($aExit -eq 0) "A exited with code $aExit"
    Assert ($null -ne $latencyLine) 'B printed no latency line'

    $lat = ConvertFrom-LatencyLine $latencyLine
    Write-Host "  $latencyLine"
    Assert ($lat.N -ge 5) "only $($lat.N) clicks paired (need >= 5)"

    $statsLines = @((Read-SharedFile $bStats) -split "`r?`n" | Where-Object { $_ -ne '' })
    Write-Host "  B stats: $($statsLines.Count) lines; last: $($statsLines | Select-Object -Last 1)"
    Assert ($statsLines.Count -ge 1) 'B wrote no stats lines'

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
