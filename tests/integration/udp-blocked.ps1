# Field report (2026-11-08): a friend joined over Tailscale, TCP worked but his UDP never reached the host; the host
# heard constant hiss for him and he got "Sin audio del servidor". This reproduces it with a participant that joins
# over TCP and never sends UDP:
#   - A (headless host, embedded server) must hear SILENCE from him, not PLC noise (peakReceived ~ 0);
#   - the server's "stats" must report packets 0 for him (what lets the UI say whose UDP is missing) and > 0 for A.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22156)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'udp-blocked'
$procs = @()
$status = 1
$tcp = $null

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $aStats = Join-Path $run 'A.stats.jsonl'

    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--host', '--port', "$Port", '--name', 'Host', '--role', 'both',
        '--fake-audio', 'silence', '--block', '64', '--stats', $aStats, '--duration', '10')
    $procs += $a
    $joined = Wait-ForLine $a '^joined ' 15
    $session = ($joined -split '\s+')[1]

    # The "friend": joins over TCP only and never sends a datagram.
    $tcp = New-Object System.Net.Sockets.TcpClient
    $tcp.Connect('127.0.0.1', $Port)
    $stream = $tcp.GetStream()
    $stream.ReadTimeout = 8000
    $writer = New-Object System.IO.StreamWriter ($stream, (New-Object System.Text.UTF8Encoding $false))
    $writer.NewLine = "`n"; $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader ($stream)
    $writer.WriteLine('{"t":"join","session":"' + $session + '","name":"Amigo","role":"both"}')

    $joinReply = $reader.ReadLine() | ConvertFrom-Json
    Assert ($joinReply.t -eq 'joined') "friend could not join: $($joinReply | ConvertTo-Json -Compress)"
    $friendId = [int64] $joinReply.clientId
    Write-Host "  friend joined $session as $friendId (TCP only, no UDP)"

    # Read control lines for ~6 s and keep the last stats message.
    $lastStats = $null
    $deadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $deadline)
    {
        $line = $reader.ReadLine()
        if ($null -eq $line) { break }
        if ($line -match '"t":\s*"stats"') { $lastStats = $line | ConvertFrom-Json }
    }
    Assert ($null -ne $lastStats) 'the server sent no stats message'

    $friendStats = @($lastStats.participants | Where-Object { [int64] $_.id -eq $friendId })
    $hostStats = @($lastStats.participants | Where-Object { [int64] $_.id -ne $friendId })
    Write-Host ("  server stats: friend packets {0}, host packets {1}" -f $friendStats[0].packets, $hostStats[0].packets)
    Assert ($friendStats.Count -eq 1 -and $null -ne $friendStats[0].packets) 'stats carry no packet counter for the friend'
    Assert ([int64] $friendStats[0].packets -eq 0) "friend packets should be 0, got $($friendStats[0].packets)"
    Assert ([int64] $hostStats[0].packets -gt 100) "host packets should be > 100, got $($hostStats[0].packets)"

    $aExit = Wait-Proc $a 25
    Assert ($aExit -eq 0) "A exited with code $aExit"

    $lines = @((Read-SharedFile $aStats) -split "`r?`n" | Where-Object { $_ -ne '' } | ForEach-Object { $_ | ConvertFrom-Json })
    $withFriend = @($lines | Where-Object { $_.participants -eq 2 })
    $maxPeak = ($withFriend | ForEach-Object { [double] $_.peakReceived } | Measure-Object -Maximum).Maximum
    Write-Host ("  host stats: {0} seconds with the friend in the session, max peakReceived {1}" -f $withFriend.Count, $maxPeak)
    Assert ($withFriend.Count -ge 3) 'the friend was not in the session long enough'
    Assert ($maxPeak -lt 0.001) "the host hears noise from a participant who sends nothing (peak $maxPeak)"

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
    if ($null -ne $tcp) { $tcp.Close() }
    Stop-Procs $procs
}

exit $status
