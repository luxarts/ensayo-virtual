# record-sync (port 22240): EnsayoServer hosts a fresh project. A (the host user) imports an 8 s click track
# (44.1 kHz, a click every 250 ms). B downloads it, then records 6 s on its own voice track at 1.0 s with the fake
# device in loopback mode (latency in 200, out 264, block 256): what B records is its own playback of the click
# track, delayed by the hidden latencies. A stays connected for 40 s (not --wait-takes 2: that returns as soon as both
# takes are complete and cached here, and B's add_region lands one version later, so A's hash could miss it).
# Asserts:
#   - A printed at least 3 distinct "cached <B's take> <k>" lines before B printed take_complete (segments arrive
#     while B is still recording);
#   - A's and B's project_hash are equal, B's region starts at 48000 with offset 464 (= 200 + 264);
#   - alignment: the mix exported with only the click track audible and with only B's track audible (set_mix muted
#     through --ops) have their clicks at the same samples: every click onset in B's region coincides with a
#     reference onset within +-1 sample (onset = first sample at or above half the file's peak after 2000 quiet
#     samples, left channel). Onsets within the region's 144-sample edge fades are not compared.
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'record-sync'
$port = 22240
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $project = Join-Path $run 'project'
    $click = New-ClickWav -Path (Join-Path $run 'click.wav') -Seconds 8 -Rate 44100 -Interval 0.25

    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project $project
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $userB = New-Id
    $cacheA = Join-Path $run 'cache-a'
    $cacheB = Join-Path $run 'cache-b'

    # A: host user, imports the click track and waits for B's take.
    $a = Start-Headless -Name 'a' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', $cacheA,
        '--import', $click, '--duration', '40', '--dump-project', (Join-Path $run 'a-project.json'))
    $procs += $a
    $clickTake = (Wait-ForLine $a '^take_complete ' 60).Split(' ')[1]
    Write-Host "  click take $clickTake imported"

    # B, first run: only downloads the click track into its cache, so its playback has it.
    $b1 = Start-Headless -Name 'b1' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--wait-takes', '1')
    $procs += $b1
    Assert ((Wait-Proc $b1 60) -eq 0) ("b1 failed: " + ((Get-OutputLines $b1) -join ' | '))

    # B, second run: records through the loopback.
    $b2 = Start-Headless -Name 'b2' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB,
        '--fake-audio', 'loopback', '--latency-in', '200', '--latency-out', '264', '--block', '256',
        '--add-track', 'voice', 'Voz', '--record', 'Voz', '--at', '1.0', '--for', '6', '--duration', '1')
    $procs += $b2

    # Poll both outputs: when did each "cached <take> <k>" line of A (for takes other than the click) appear, and when
    # did B print take_complete?
    $firstSeen = @{}
    $completeAt = $null
    $voiceTake = $null
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 90)
    {
        foreach ($line in (Get-OutputLines $a))
        {
            if ($line -match '^cached (\S+) (\d+)$' -and $Matches[1] -ne $clickTake -and -not $firstSeen.ContainsKey($line))
            {
                $firstSeen[$line] = $sw.Elapsed.TotalMilliseconds
            }
        }
        if ($null -eq $completeAt)
        {
            $hit = @(Get-OutputLines $b2 | Where-Object { $_ -match '^take_complete ' }) | Select-Object -First 1
            if ($null -ne $hit) { $completeAt = $sw.Elapsed.TotalMilliseconds; $voiceTake = $hit.Split(' ')[1] }
        }
        if ($b2.Process.HasExited -and $a.Process.HasExited) { break }
        Start-Sleep -Milliseconds 50
    }

    Assert ((Wait-Proc $b2 30) -eq 0) ("b2 failed: " + ((Get-OutputLines $b2) -join ' | '))
    Assert ((Wait-Proc $a 60) -eq 0) ("a failed: " + ((Get-OutputLines $a) -join ' | '))
    Assert ($null -ne $voiceTake) 'B never printed take_complete'

    $before = @()
    foreach ($key in $firstSeen.Keys)
    {
        $parts = $key.Split(' ')
        if ($parts[1] -eq $voiceTake -and $firstSeen[$key] -lt $completeAt) { $before += [int] $parts[2] }
    }
    $before = @($before | Sort-Object -Unique)
    Write-Host ("  A cached segments of B's take before B's take_complete: k = {0}" -f ($before -join ', '))
    Assert ($before.Count -ge 3) "A saw only $($before.Count) distinct segment counts of B's take before B's take_complete"

    $aLines = Get-OutputLines $a
    Assert (@($aLines | Where-Object { $_ -match "^take_complete $voiceTake " }).Count -eq 1) "A never saw B's take complete"
    Assert (@($aLines | Where-Object { $_ -match "^cached $clickTake " }).Count -ge 1) "A has no cached line for the click take"

    # Same project everywhere.
    $hashA = Get-LastValue $a 'project_hash'
    $hashB = Get-LastValue $b2 'project_hash'
    Write-Host "  project_hash A $hashA, B $hashB"
    Assert ($null -ne $hashA -and $hashA -eq $hashB) "project_hash differs: A $hashA, B $hashB"

    $p = Get-Content -Raw (Join-Path $run 'a-project.json') | ConvertFrom-Json
    $voiceTrack = @($p.tracks | Where-Object { $_.name -eq 'Voz' })[0]
    $clickTrack = @($p.tracks | Where-Object { $_.kind -eq 'imported' })[0]
    $region = @($voiceTrack.regions)[0]
    Write-Host ("  B's region: start {0} offset {1} length {2}" -f $region.start, $region.offset, $region.length)
    Assert ([int64] $region.start -eq 48000) "region start $($region.start), expected 48000"
    Assert ([int64] $region.offset -eq 464) "region offset $($region.offset), expected 464 (200 + 264)"

    # Two exports of the same project: only the click track, then only B's track.
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
        Assert (@(Get-OutputLines $e | Where-Object { $_ -match '^rejected ' }).Count -eq 0) "$($pass.N): an op was rejected"
    }

    $ref = [EnsayoTest.Wav]::Read($wavClick)
    $voc = [EnsayoTest.Wav]::Read($wavVoice)
    $refOnsets = [EnsayoTest.Wav]::Onsets($ref.Data[0], 0.5 * [EnsayoTest.Wav]::Peak($ref.Data[0]), 2000)
    $vocOnsets = [EnsayoTest.Wav]::Onsets($voc.Data[0], 0.5 * [EnsayoTest.Wav]::Peak($voc.Data[0]), 2000)

    $regionStart = [int64] $region.start
    $regionEnd = $regionStart + [int64] $region.length
    $compared = @($vocOnsets | Where-Object { $_ -ge ($regionStart + 144) -and $_ -lt ($regionEnd - 144) })
    $outside = @($vocOnsets | Where-Object { $_ -lt $regionStart -or $_ -ge $regionEnd })
    Assert ($outside.Count -eq 0) "B's track has onsets outside its region: $($outside -join ', ')"

    # Reference clicks that fall inside B's region (away from the edge fades) must all be in B's track too.
    $expected = @($refOnsets | Where-Object { $_ -ge ($regionStart + 144) -and $_ -lt ($regionEnd - 144) })
    Write-Host ("  reference onsets: {0} (every {1}); in B's region: {2}; B's onsets compared: {3}" -f $refOnsets.Count, ($refOnsets[1] - $refOnsets[0]), $expected.Count, $compared.Count)
    Assert ($expected.Count -ge 20) "only $($expected.Count) reference clicks inside B's region"
    Assert ($compared.Count -eq $expected.Count) "B's track has $($compared.Count) clicks in its region, the reference $($expected.Count)"

    $errors = [EnsayoTest.Wav]::NearestErrors([long[]] $compared, $refOnsets)
    $maxError = 0
    foreach ($err in $errors) { $maxError = [math]::Max($maxError, [math]::Abs($err)) }
    Write-Host ("  alignment errors (samples): {0}" -f (($errors | Sort-Object -Unique) -join ', '))
    Write-Host ("  alignment: {0} clicks, max |error| {1} samples" -f $errors.Count, $maxError)
    Assert ($maxError -le 1) "clicks of B's take are off by up to $maxError samples"
    Write-Host ("  result: A cached k={0} of B's take before its take_complete; hashes equal; offset {1}; {2} clicks, max |error| {3} samples" -f ($before -join ','), $region.offset, $errors.Count, $maxError)

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
