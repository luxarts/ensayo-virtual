# reconnect (port 22242): B starts a 6 s take (fake sine) and its process is killed about 3 s into it (a crash: no
# finish, no rec_stop). B is started again with the same --user and --cache and --resume: it uploads what was still
# pending and finishes the unfinished take. A (the host user) watches the whole time with --wait-takes 1.
# Asserts: the take is not complete before the restart; A prints take_complete only after B's --resume started; B's
# --resume prints take_complete with the same sample count; on the host (project.json after a clean stop) the take is
# complete, its segments are exactly 0..n-1 on disk (no gaps, no duplicates, segments * 48000 >= samples), and B's
# track has one region {start 0, offset 0, length = samples}.
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'reconnect'
$port = 22242
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $project = Join-Path $run 'project'

    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project $project
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $userB = New-Id
    $cacheB = Join-Path $run 'cache-b'

    $a = Start-Headless -Name 'a' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', (Join-Path $run 'cache-a'),
        '--wait-takes', '1', '--duration', '90')
    $procs += $a
    Wait-ForLine $a '^welcome ' 20 | Out-Null

    $b1 = Start-Headless -Name 'b1' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--fake-audio', 'sine',
        '--add-track', 'voice', 'Voz', '--record', 'Voz', '--at', '0', '--for', '6')
    $procs += $b1

    # The first segment of B's own take lands in its cache about 1 s into the take; kill 2 s later.
    $take = (Wait-ForLine $b1 '^cached \S+ 1$' 20).Split(' ')[1]
    Start-Sleep -Seconds 2
    Assert (-not $b1.Process.HasExited) 'B exited before it was killed'
    $b1.Process.Kill()
    $b1.Process.WaitForExit()
    $segmentsBefore = @(Get-OutputLines $b1 | Where-Object { $_ -match "^cached $take (\d+)$" } | ForEach-Object { [int] $_.Split(' ')[2] }) | Measure-Object -Maximum
    Write-Host ("  killed B about 3 s into take {0} (its cache had {1} segments)" -f $take, $segmentsBefore.Maximum)
    Assert (@(Get-OutputLines $b1 | Where-Object { $_ -match '^take_complete ' }).Count -eq 0) 'B completed its take before the kill'

    Start-Sleep -Seconds 1
    Assert (@(Get-OutputLines $a | Where-Object { $_ -match "^take_complete $take" }).Count -eq 0) 'the take was complete on A before B came back'

    $b2 = Start-Headless -Name 'b2' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--resume')
    $procs += $b2
    Assert ((Wait-Proc $b2 60) -eq 0) ("b2 --resume failed: " + ((Get-OutputLines $b2) -join ' | '))
    $resumed = Wait-ForLine $b2 "^take_complete $take \d+$" 1
    $samples = [int64] $resumed.Split(' ')[2]
    Write-Host "  --resume: $resumed"

    Assert ((Wait-Proc $a 60) -eq 0) ("a failed: " + ((Get-OutputLines $a) -join ' | '))
    $aComplete = Wait-ForLine $a "^take_complete $take \d+$" 1
    Assert ([int64] $aComplete.Split(' ')[2] -eq $samples) "A saw $aComplete, B resumed $samples samples"
    Assert ($samples -ge 2 * 48000 -and $samples -le 4 * 48000) "the take has $samples samples; expected about 3 s"

    Assert ((Stop-ServerProc $server) -eq 0) 'the server did not exit cleanly'
    $p = Get-Content -Raw (Join-Path $project 'project.json') | ConvertFrom-Json
    $t = @($p.takes | Where-Object { $_.id -eq $take })[0]
    Assert ($null -ne $t) 'the take is not in the host project'
    $files = @(Get-ChildItem (Join-Path $project "takes\$take") -Filter '*.flac' | ForEach-Object { [int] $_.BaseName } | Sort-Object)
    $expectedIndices = @(0..([int] $t.segments - 1))
    Write-Host ("  host: take complete {0}, segments {1}, samples {2}, files {3}" -f $t.complete, $t.segments, $t.samples, ($files -join ','))
    Assert ($t.complete -eq $true) 'the take is not complete on the host'
    Assert ([int64] $t.samples -eq $samples) "host samples $($t.samples) != $samples"
    Assert (($files -join ',') -eq ($expectedIndices -join ',')) "segment files $($files -join ',') != 0..$([int] $t.segments - 1)"
    Assert ([int64] $t.segments * 48000 -ge $samples -and ([int64] $t.segments - 1) * 48000 -lt $samples) 'segment count does not match the samples'

    $track = @($p.tracks | Where-Object { $_.author -eq $userB })[0]
    $region = @($track.regions)[0]
    Write-Host ("  region: start {0} offset {1} length {2}" -f $region.start, $region.offset, $region.length)
    Assert (@($track.regions).Count -eq 1) "B's track has $(@($track.regions).Count) regions"
    Write-Host ("  result: killed after {0} segments; --resume completed {1} samples, host segments {2}, region length {3}" -f $segmentsBefore.Maximum, $samples, ($files -join ','), $region.length)
    Assert ($region.take -eq $take -and [int64] $region.start -eq 0 -and [int64] $region.offset -eq 0 -and [int64] $region.length -eq $samples) 'the recovered region is wrong'

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
