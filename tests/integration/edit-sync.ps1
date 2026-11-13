# edit-sync (port 22241): three clients on one EnsayoServer project. A (the host user) adds its track through --ops,
# C only watches. B records a 3 s take on its own track, then (second run, same user and cache) sends an ops file:
# add_track, add_region on its take, split_region, move_region, trim_region, set_mix, and finally a set_mix on A's
# track, which the host must reject (a guest edits only its own tracks).
# Asserts: B printed exactly one "rejected"; A, B and C print the same project_hash; A's dump holds the edits
# (regions {100000, 4000, 40000} and {200000, 48000, 48000}, gain -6 dB, pan -0.5) and A's track is not muted.
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'edit-sync'
$port = 22241
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name

    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project (Join-Path $run 'project')
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $userB = New-Id
    $userC = New-Id
    $trackA = New-Id
    $cacheB = Join-Path $run 'cache-b'

    $opsA = Join-Path $run 'ops-a.jsonl'
    Write-OpsFile $opsA @(@{ op = 'add_track'; track = $trackA; name = 'Voz A'; kind = 'voice' })

    $a = Start-Headless -Name 'a' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', (Join-Path $run 'cache-a'),
        '--ops', $opsA, '--duration', '30', '--dump-project', (Join-Path $run 'a-project.json'))
    $procs += $a
    $c = Start-Headless -Name 'c' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userC, '--name', 'Caro', '--cache', (Join-Path $run 'cache-c'), '--duration', '30')
    $procs += $c
    Wait-ForLine $a '^welcome ' 20 | Out-Null
    Wait-ForLine $c '^welcome ' 20 | Out-Null

    # B records a take.
    $b1 = Start-Headless -Name 'b1' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--fake-audio', 'sine',
        '--add-track', 'voice', 'Voz B', '--record', 'Voz B', '--at', '0.5', '--for', '3', '--duration', '0.5')
    $procs += $b1
    Assert ((Wait-Proc $b1 60) -eq 0) ("b1 failed: " + ((Get-OutputLines $b1) -join ' | '))
    $parts = (Wait-ForLine $b1 '^take_complete ' 1).Split(' ')
    $take = $parts[1]
    $takeSamples = [int64] $parts[2]
    Write-Host "  B's take $take, $takeSamples samples"
    Assert ($takeSamples -ge 144000) "B's take is only $takeSamples samples"

    # B edits a new track built on that take, then tries to mute A's track.
    $trackB = New-Id
    $r1 = New-Id
    $r2 = New-Id
    $opsB = Join-Path $run 'ops-b.jsonl'
    Write-OpsFile $opsB @(
        @{ op = 'add_track'; track = $trackB; name = 'Edicion B'; kind = 'voice' },
        @{ op = 'add_region'; track = $trackB; region = $r1; take = $take; start = 96000; offset = 0; length = 96000 },
        @{ op = 'split_region'; track = $trackB; region = $r1; at = 144000; newRegion = $r2 },
        @{ op = 'move_region'; track = $trackB; region = $r2; start = 200000 },
        @{ op = 'trim_region'; track = $trackB; region = $r1; start = 100000; offset = 4000; length = 40000 },
        @{ op = 'set_mix'; track = $trackB; gainDb = -6; pan = -0.5; muted = $false },
        @{ op = 'set_mix'; track = $trackA; muted = $true })

    $b2 = Start-Headless -Name 'b2' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--ops', $opsB, '--duration', '0.5')
    $procs += $b2
    Assert ((Wait-Proc $b2 60) -eq 0) ("b2 failed: " + ((Get-OutputLines $b2) -join ' | '))

    $rejected = @(Get-OutputLines $b2 | Where-Object { $_ -match '^rejected ' })
    Write-Host ("  B's rejections: {0}" -f ($rejected -join ' | '))
    Assert ($rejected.Count -eq 1) "B printed $($rejected.Count) rejected lines, expected 1"
    Assert ((@(Get-OutputLines $b2 | Where-Object { $_ -match '^applied ' })).Count -ge 6) 'B did not see its 6 edits applied'

    Assert ((Wait-Proc $a 60) -eq 0) ("a failed: " + ((Get-OutputLines $a) -join ' | '))
    Assert ((Wait-Proc $c 60) -eq 0) ("c failed: " + ((Get-OutputLines $c) -join ' | '))

    # A and C watch for a fixed time: both must have reached B's last version before their hashes are compared.
    $versions = @((Get-LastVersion $a), (Get-LastVersion $b2), (Get-LastVersion $c))
    Write-Host ("  last version A {0}, B {1}, C {2}" -f $versions[0], $versions[1], $versions[2])
    Assert ($versions[1] -gt 0 -and $versions[0] -eq $versions[1] -and $versions[2] -eq $versions[1]) "A or C left before B's last version (raise their --duration)"
    $hashA = Get-LastValue $a 'project_hash'
    $hashB = Get-LastValue $b2 'project_hash'
    $hashC = Get-LastValue $c 'project_hash'
    Write-Host "  project_hash A $hashA, B $hashB, C $hashC"
    Assert ($null -ne $hashA -and $hashA -eq $hashB -and $hashA -eq $hashC) 'project_hash differs between A, B and C'

    $p = Get-Content -Raw (Join-Path $run 'a-project.json') | ConvertFrom-Json
    $tb = @($p.tracks | Where-Object { $_.id -eq $trackB })[0]
    $ta = @($p.tracks | Where-Object { $_.id -eq $trackA })[0]
    Assert ($null -ne $tb -and $null -ne $ta) 'tracks missing from the dump'
    $reg1 = @($tb.regions | Where-Object { $_.id -eq $r1 })[0]
    $reg2 = @($tb.regions | Where-Object { $_.id -eq $r2 })[0]
    Write-Host ("  regions: r1 {{{0}, {1}, {2}}}, r2 {{{3}, {4}, {5}}}; mix gain {6} pan {7}; A's track muted {8}" -f `
        $reg1.start, $reg1.offset, $reg1.length, $reg2.start, $reg2.offset, $reg2.length, $tb.gainDb, $tb.pan, $ta.muted)
    Assert ([int64] $reg1.start -eq 100000 -and [int64] $reg1.offset -eq 4000 -and [int64] $reg1.length -eq 40000) 'r1 is not the trimmed region'
    Assert ([int64] $reg2.start -eq 200000 -and [int64] $reg2.offset -eq 48000 -and [int64] $reg2.length -eq 48000) 'r2 is not the moved right half'
    Assert ([double] $tb.gainDb -eq -6 -and [double] $tb.pan -eq -0.5) 'set_mix not applied'
    Assert (-not $ta.muted) "A's track was muted by a guest"
    Write-Host "  result: 6 edits applied, 1 rejected ($($rejected[0])); A, B and C hash $hashA"

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
