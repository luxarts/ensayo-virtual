# A uploads tests\fixtures\click.wav and plays it; B probes the clicks coming from the server's backing track
# and its stats must report the transport as playing.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22152)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'backing-track'
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $wav = Get-ClickWav
    $bLog = Join-Path $run 'B.csv'
    $bStats = Join-Path $run 'B.stats.jsonl'

    $server = Start-ServerProc -FilePath $bin.Server -Port $Port -RunDir $run
    $procs += $server

    # B joins first so it is listening when the track starts.
    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', "ensayo://127.0.0.1:$Port/NEW", '--name', 'A', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--duration', '14')
    $procs += $a
    $linkLine = Wait-ForLine $a '^link ensayo://' 15
    $link = ($linkLine -split '\s+')[1]
    Wait-ForLine $a '^joined ' 10 | Out-Null

    $b = Start-Proc -Name 'B' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'B', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--probe', $bLog, '--stats', $bStats, '--duration', '12')
    $procs += $b
    Wait-ForLine $b '^joined ' 10 | Out-Null
    Wait-ForLine $b '^participants 2$' 10 | Out-Null

    # C (the uploader) joins with the track and sends play as soon as it is loaded.
    $c = Start-Proc -Name 'C' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'C', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--track', $wav, '--play', '--duration', '8')
    $procs += $c
    Wait-ForLine $c '^joined ' 10 | Out-Null

    $cExit = Wait-Proc $c 25
    $bExit = Wait-Proc $b 25
    $aExit = Wait-Proc $a 25

    Write-Host "  B: $((Get-OutputLines $b) -join ' | ')"
    Write-Host "  C: $((Get-OutputLines $c) -join ' | ')"

    Assert ($cExit -eq 0) "C (uploader) exited with code $cExit (stderr: $((Read-SharedFile $c.StdErr).Trim()))"
    Assert ($bExit -eq 0) "B exited with code $bExit (stderr: $((Read-SharedFile $b.StdErr).Trim()))"
    Assert ($aExit -eq 0) "A exited with code $aExit"

    $clicks = Get-ClickCount $bLog
    Write-Host "  B detected $clicks clicks from the backing track"
    Assert ($clicks -ge 8) "B detected only $clicks clicks (need >= 8)"

    $statsLines = @((Read-SharedFile $bStats) -split "`r?`n" | Where-Object { $_ -ne '' })
    $playing = @($statsLines | Where-Object { $_ -match '"trackPlaying":true' })
    Write-Host "  B stats: $($statsLines.Count) lines, $($playing.Count) with trackPlaying:true"
    Assert ($playing.Count -ge 1) 'B stats never showed "trackPlaying":true'

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
