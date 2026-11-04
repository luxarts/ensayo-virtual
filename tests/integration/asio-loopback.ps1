# Real ASIO hardware: A runs on a real driver whose output pair is wired back to two of its inputs (the MiniFuse 2
# exposes "LOOPBACK" outputs 2/3 that reappear as inputs 2/3). B (fake device) emits clicks and probes its own output:
# B click -> server -> A (ASIO out) -> loopback -> A (ASIO in) -> server -> B output, i.e. two full hops plus the
# driver's input + output latency. SKIPs when the driver is not installed.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22154,
       [string] $Driver = 'MiniFuse ASIO Driver', [int] $OutPair = 1, [int] $Mic = 2, [int] $Inst = 3, [int] $Block = 128)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'asio-loopback'
$procs = @()
$status = 1

try
{
    $installed = @(Get-ChildItem 'HKLM:\SOFTWARE\ASIO' -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName })
    if ($installed -notcontains $Driver)
    {
        Write-Skip $name "ASIO driver '$Driver' not installed (found: $($installed -join ', '))"
        exit 0
    }

    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $bLog = Join-Path $run 'B.emit.csv'
    $bProbe = Join-Path $run 'B.probe.csv'
    $aStats = Join-Path $run 'A.stats.jsonl'
    $bStats = Join-Path $run 'B.stats.jsonl'

    $server = Start-ServerProc -FilePath $bin.Server -Port $Port -RunDir $run
    $procs += $server

    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', "ensayo://127.0.0.1:$Port/NEW", '--name', 'A', '--role', 'both',
        '--asio', $Driver, '--block', "$Block", '--out', "$OutPair", '--mic', "$Mic", '--inst', "$Inst",
        '--mute-monitor', '--stats', $aStats, '--duration', '14')
    $procs += $a
    $deviceLine = Wait-ForLine $a '^(device|error) ' 20
    Write-Host "  $deviceLine"
    Assert ($deviceLine -match '^device ') "A could not open the driver: $deviceLine"
    Assert ($deviceLine -match " rate 48000 block $Block ") "driver did not open at 48000 Hz / $Block samples"
    $linkLine = Wait-ForLine $a '^link ensayo://' 15
    $link = ($linkLine -split '\s+')[1]
    Wait-ForLine $a '^joined ' 10 | Out-Null

    $b = Start-Proc -Name 'B' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'B', '--role', 'both',
        '--fake-audio', 'click', '--block', "$Block", '--emit-log', $bLog, '--probe', $bProbe, '--compare', $bLog,
        '--stats', $bStats, '--duration', '10')
    $procs += $b

    Wait-ForLine $b '^joined ' 10 | Out-Null
    Wait-ForLine $b '^participants 2$' 10 | Out-Null

    $bExit = Wait-Proc $b 25
    $aExit = Wait-Proc $a 25

    $aLines = Get-OutputLines $a
    $bLines = Get-OutputLines $b
    $latencyLine = $bLines | Where-Object { $_ -match '^latency ' } | Select-Object -Last 1
    $callbacksLine = $aLines | Where-Object { $_ -match '^callbacks ' } | Select-Object -Last 1
    Write-Host "  A: $(($aLines | Where-Object { $_ -notmatch '^(input|output|blocks|rates) ' }) -join ' | ')"
    Write-Host "  B: $($bLines -join ' | ')"

    Assert ($bExit -eq 0) "B exited with code $bExit (stderr: $((Read-SharedFile $b.StdErr).Trim()))"
    Assert ($aExit -eq 0) "A exited with code $aExit (stderr: $((Read-SharedFile $a.StdErr).Trim()))"
    Assert ($null -ne $callbacksLine) 'A printed no callbacks line'
    $callbacks = [int] (($callbacksLine -split '\s+')[1])
    # 14 s at 48000 / Block callbacks; allow the join/leave slack.
    $expected = 14 * 48000 / $Block
    Assert ($callbacks -ge $expected * 0.8) "A ran only $callbacks audio callbacks (expected about $expected)"

    $aLast = Get-Content $aStats | Select-Object -Last 1 | ConvertFrom-Json
    Assert ($aLast.underruns -eq 0) "A reported $($aLast.underruns) underruns on the real driver"
    Assert ($null -ne $latencyLine) 'B printed no latency line'

    $lat = ConvertFrom-LatencyLine $latencyLine
    Write-Host "  $latencyLine"
    Assert ($lat.N -ge 10) "only $($lat.N) clicks came back through the hardware loopback (need >= 10)"
    Assert ($lat.Median -lt 80) "round-trip median $($lat.Median) ms (need < 80)"

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
