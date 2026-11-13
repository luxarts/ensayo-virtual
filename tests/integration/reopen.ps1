# reopen (port 22243): EnsayoServer hosts a project; B adds a track and records 2 s. The server is stopped cleanly
# (stdin EOF) and started again on the same project directory, without --user.
# Asserts: the restarted server has the same host user and session id (so the old link still works); B reconnects
# with the old link, its cache and user and prints the same project_hash as before the restart; a new client C with an
# empty cache downloads the take from the restarted host (--wait-takes 1) and its tracks and takes equal the dump from
# before the restart (its own join adds a user, so its hash differs by design).
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'reopen'
$port = 22243
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $project = Join-Path $run 'project'

    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project $project -ExtraArgs @('--name', 'Reabrir')
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $userB = New-Id
    $cacheB = Join-Path $run 'cache-b'

    $b1 = Start-Headless -Name 'b1' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--fake-audio', 'sine',
        '--add-track', 'voice', 'Voz', '--record', 'Voz', '--at', '0.25', '--for', '2', '--duration', '0.5',
        '--dump-project', (Join-Path $run 'before.json'))
    $procs += $b1
    Assert ((Wait-Proc $b1 60) -eq 0) ("b1 failed: " + ((Get-OutputLines $b1) -join ' | '))
    # B1 is the recorder: it exits only once its region is in its replica, so this hash already includes it.
    $hashBefore = Get-LastValue $b1 'project_hash'
    $beforeDump = Get-Content -Raw (Join-Path $run 'before.json') | ConvertFrom-Json
    Assert (@(@($beforeDump.tracks)[0].regions).Count -eq 1) "B's region is not in the project B printed the hash of"
    Write-Host "  before the restart: project_hash $hashBefore"

    Assert ((Stop-ServerProc $server) -eq 0) 'the server did not exit cleanly'
    Assert (Test-Path (Join-Path $project 'project.json')) 'project.json was not saved'

    $server2 = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project $project
    $procs += $server2
    Write-Host ("  restarted: session {0} -> {1}, host user {2} -> {3}" -f $server.Session, $server2.Session, $server.HostUser, $server2.HostUser)
    Assert ($server2.Session -eq $server.Session) 'the session id changed after the restart'
    Assert ($server2.HostUser -eq $server.HostUser) 'the host user changed after the restart'

    $b2 = Start-Headless -Name 'b2' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $userB, '--name', 'Bruno', '--cache', $cacheB, '--wait-takes', '1')
    $procs += $b2
    Assert ((Wait-Proc $b2 60) -eq 0) ("b2 failed: " + ((Get-OutputLines $b2) -join ' | '))
    $hashB = Get-LastValue $b2 'project_hash'
    Write-Host "  after the restart: B $hashB"
    Assert ($null -ne $hashBefore -and $hashB -eq $hashBefore) 'project_hash changed across the restart'

    # A new user changes the project (add_user), so C is compared on what it downloaded: tracks and takes.
    $c = Start-Headless -Name 'c' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', (New-Id), '--name', 'Caro', '--cache', (Join-Path $run 'cache-c'), '--wait-takes', '1',
        '--dump-project', (Join-Path $run 'c.json'))
    $procs += $c
    Assert ((Wait-Proc $c 60) -eq 0) ("c failed: " + ((Get-OutputLines $c) -join ' | '))
    $before = Get-Content -Raw (Join-Path $run 'before.json') | ConvertFrom-Json
    $after = Get-Content -Raw (Join-Path $run 'c.json') | ConvertFrom-Json
    $same = (($before.tracks | ConvertTo-Json -Depth 8 -Compress) -eq ($after.tracks | ConvertTo-Json -Depth 8 -Compress)) -and
            (($before.takes | ConvertTo-Json -Depth 8 -Compress) -eq ($after.takes | ConvertTo-Json -Depth 8 -Compress))
    Write-Host ("  C (new user, empty cache): {0}; tracks and takes equal to before the restart: {1}" -f ((@(Get-OutputLines $c | Where-Object { $_ -match '^cached ' }) -join ', ')), $same)
    Assert $same "C's tracks/takes differ from the project before the restart"
    Assert (@(Get-OutputLines $c | Where-Object { $_ -match '^cached \S+ \d+$' }).Count -ge 1) 'C did not download the take'
    Write-Host "  result: same session $($server2.Session) and host user after the restart; hash $hashB unchanged; C downloaded the take"

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
