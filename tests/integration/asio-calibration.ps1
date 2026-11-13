# asio-calibration (port 22245): real hardware. Needs the Arturia MiniFuse ("MiniFuse ASIO Driver"), whose LOOPBACK
# outputs 2/3 come back as inputs 2/3. SKIPs when the driver is not installed, and also when it is installed but cannot
# be opened (the interface is unplugged or another program, such as a running Ensayo Virtual, holds it).
# 1. --calibrate alone (--out 1 --mic 2 --inst 3 --mute-monitor): prints the measured calibration.
# 2. A (the host user of an EnsayoServer project) opens the same device, calibrates again (the value is used for that
#    run), imports a 4 s click track (44.1 kHz, a click every 250 ms) and records 4 s on its voice track at 1.0 s:
#    the playback of the click track goes out of LOOPBACK and back into input 2.
# 3. Two exports with set_mix mutes (only the click track, only the voice track) on the fake device: every click onset
#    in the recorded region must coincide with a reference onset within +-48 samples (1 ms). The script also prints
#    what the error would have been without the calibration (the region offset minus the calibration).
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [string] $Driver = 'MiniFuse ASIO Driver')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'asio-calibration'
$port = 22245
$procs = @()
$status = 1
$asioArgs = @('--asio', $Driver, '--out', '1', '--mic', '2', '--inst', '3', '--mute-monitor')

# The device could not be opened (not plugged in, or held by another program): SKIP, not FAIL.
function Get-OpenFailure
{
    param ($Proc)
    $line = @(Get-OutputLines $Proc | Where-Object { $_ -match '^error (ASIO|audio device|fake audio)' }) | Select-Object -First 1
    return $line
}

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

    # ---- 1. calibration alone
    $cal = Start-Headless -Name 'calibrate' -Client $bin.Client -RunDir $run -Arguments ($asioArgs + @('--calibrate'))
    $procs += $cal
    $calExit = Wait-Proc $cal 60
    $openFailure = Get-OpenFailure $cal
    if ($null -ne $openFailure)
    {
        Write-Skip $name "the '$Driver' device could not be opened (unplugged, or another program holds it): $openFailure"
        exit 0
    }
    Assert ($calExit -eq 0) ("--calibrate failed: " + ((Get-OutputLines $cal) -join ' | '))
    $deviceLine = Wait-ForLine $cal '^device ' 1
    $calibration = [int] (Get-LastValue $cal 'calibration')
    Write-Host "  $deviceLine"
    Write-Host "  calibration $calibration samples"

    # ---- 2. record the click track through the loopback
    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project (Join-Path $run 'project')
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $cacheA = Join-Path $run 'cache-a'
    $click = New-ClickWav -Path (Join-Path $run 'click.wav') -Seconds 6 -Rate 44100 -Interval 0.25
    $dump = Join-Path $run 'project.json'

    $a = Start-Headless -Name 'a' -Client $bin.Client -RunDir $run -Arguments (@(
        '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', $cacheA) + $asioArgs + @(
        '--calibrate', '--add-track', 'voice', 'Voz', '--import', $click, '--record', 'Voz', '--at', '1.0', '--for', '4',
        '--duration', '0.5', '--dump-project', $dump))
    $procs += $a
    $aExit = Wait-Proc $a 120
    $openFailure = Get-OpenFailure $a
    if ($null -ne $openFailure)
    {
        Write-Skip $name "the '$Driver' device could not be opened for the recording: $openFailure"
        exit 0
    }
    Assert ($aExit -eq 0) ("recording through the loopback failed: " + ((Get-OutputLines $a) -join ' | '))
    $runCalibration = [int] (Get-LastValue $a 'calibration')
    $devA = Wait-ForLine $a '^device ' 1
    Write-Host "  recording run: calibration $runCalibration; callbacks $(Get-LastValue $a 'callbacks')"
    $latIn = [int] ([regex]::Match($devA, 'latencyIn (\d+)').Groups[1].Value)
    $latOut = [int] ([regex]::Match($devA, 'latencyOut (\d+)').Groups[1].Value)

    $p = Get-Content -Raw $dump | ConvertFrom-Json
    $voiceTrack = @($p.tracks | Where-Object { $_.name -eq 'Voz' })[0]
    $clickTrack = @($p.tracks | Where-Object { $_.kind -eq 'imported' })[0]
    $region = @($voiceTrack.regions)[0]
    Write-Host ("  region: start {0} offset {1} length {2} (latency in {3} + out {4} + calibration {5} = {6})" -f $region.start, $region.offset, $region.length, $latIn, $latOut, $runCalibration, ($latIn + $latOut + $runCalibration))
    Assert ([int64] $region.offset -eq ($latIn + $latOut + $runCalibration)) 'the region offset is not latency in + out + calibration'

    # ---- 3. exports: only the click track, only the recorded track
    $opsClick = Join-Path $run 'ops-click-only.jsonl'
    $opsVoice = Join-Path $run 'ops-voice-only.jsonl'
    Write-OpsFile $opsClick @(@{ op = 'set_mix'; track = $voiceTrack.id; muted = $true })
    Write-OpsFile $opsVoice @(@{ op = 'set_mix'; track = $voiceTrack.id; muted = $false }, @{ op = 'set_mix'; track = $clickTrack.id; muted = $true })
    $wavClick = Join-Path $run 'click-only.wav'
    $wavVoice = Join-Path $run 'voice-only.wav'
    foreach ($pass in @(@{ N = 'export-click'; Ops = $opsClick; Wav = $wavClick }, @{ N = 'export-voice'; Ops = $opsVoice; Wav = $wavVoice }))
    {
        $e = Start-Headless -Name $pass.N -Client $bin.Client -RunDir $run -Arguments @(
            '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', $cacheA,
            '--ops', $pass.Ops, '--duration', '0.5', '--export', $pass.Wav)
        $procs += $e
        Assert ((Wait-Proc $e 60) -eq 0) ("$($pass.N) failed: " + ((Get-OutputLines $e) -join ' | '))
    }

    $ref = [EnsayoTest.Wav]::Read($wavClick)
    $voc = [EnsayoTest.Wav]::Read($wavVoice)
    $vocPeak = [EnsayoTest.Wav]::Peak($voc.Data[0])
    Write-Host ("  peaks: click track {0:F3}, recorded track {1:F3}" -f [EnsayoTest.Wav]::Peak($ref.Data[0]), $vocPeak)
    Assert ($vocPeak -gt 0.01) 'the recorded track is silent: nothing came back through the loopback'
    $refOnsets = [EnsayoTest.Wav]::Onsets($ref.Data[0], 0.5 * [EnsayoTest.Wav]::Peak($ref.Data[0]), 2000)
    $vocOnsets = [EnsayoTest.Wav]::Onsets($voc.Data[0], 0.5 * $vocPeak, 2000)

    $regionStart = [int64] $region.start
    $regionEnd = $regionStart + [int64] $region.length
    $compared = @($vocOnsets | Where-Object { $_ -ge ($regionStart + 144) -and $_ -lt ($regionEnd - 144) })
    $expected = @($refOnsets | Where-Object { $_ -ge ($regionStart + 144) -and $_ -lt ($regionEnd - 144) })
    Write-Host ("  clicks in the region: reference {0}, recorded {1}" -f $expected.Count, $compared.Count)
    Assert ($expected.Count -ge 12) "only $($expected.Count) reference clicks inside the region"
    Assert ($compared.Count -eq $expected.Count) "the recorded track has $($compared.Count) clicks in its region, the reference $($expected.Count)"

    $errors = [EnsayoTest.Wav]::NearestErrors([long[]] $compared, $refOnsets)
    $maxError = 0
    foreach ($err in $errors) { $maxError = [math]::Max($maxError, [math]::Abs($err)) }
    $first = $errors[0]
    Write-Host ("  alignment errors (samples): {0}" -f (($errors | Sort-Object -Unique) -join ', '))
    Write-Host ("  without the calibration the clicks would be off by {0} samples" -f ($first + $runCalibration))
    Write-Host ("  result: calibration {0} (run {1}), {2} clicks, max |error| {3} samples (limit 48)" -f $calibration, $runCalibration, $errors.Count, $maxError)
    Assert ($maxError -le 48) "the recorded clicks are off by up to $maxError samples (limit 48)"

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
