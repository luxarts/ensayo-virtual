# voice-chat (port 22246): A hosts a fresh project in-process (--host) and B joins it; both use --fake-audio sine, so
# each one's mic is a 440 Hz tone that the voice chat sends all the time. Nobody records. A stays 12 s after its
# welcome, B 6 s after its own (B joins once A printed its link, so A is connected during B's whole window).
# Asserts, from the "chat_rx <clientId> voice|instrument <frames>" lines printed at exit (real frames decoded, not
# concealed ones; 375 a second):
#   - A received exactly one voice stream (B's) and B exactly one (A's); no instrument stream (nobody records);
#   - each one got at least 80 % of the frames of the overlap: B's 6 s connected window, 375 x 6 = 2250 frames, so at
#     least 1800. B's frames reach A from before B's welcome until B tears down, and A's reach B from B's first
#     keepalive (right after its welcome) to its exit, so both overlaps are about 6 s; the 20 % margin covers the first
#     keepalive and the device start. The script also prints the window it measures from seeing B's welcome to
#     B's exit, as a check (polling makes it a little shorter).
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'voice-chat'
$port = 22246
$procs = @()
$status = 1
$framesPerSecond = 375
$overlapSeconds = 6
$minRatio = 0.8

# The chat_rx lines of a finished headless client: @{ Client; Kind; Frames } each.
function Get-ChatRx
{
    param ($Proc)
    $result = @()
    foreach ($line in (Get-OutputLines $Proc))
    {
        if ($line -match '^chat_rx (\d+) (voice|instrument) (\d+)$')
        {
            $result += [PSCustomObject] @{ Client = [uint32] $Matches[1]; Kind = $Matches[2]; Frames = [int64] $Matches[3] }
        }
    }
    return ,$result
}

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name

    $a = Start-Headless -Name 'a' -Client $bin.Client -RunDir $run -Arguments @(
        '--host', (Join-Path $run 'project'), '--port', $port, '--name', 'Ana', '--cache', (Join-Path $run 'cache-a'),
        '--fake-audio', 'sine', '--duration', '12')
    $procs += $a
    $link = (Wait-ForLine $a '^link ' 20).Substring(5).Trim()
    Write-Host "  A hosts $link"

    $b = Start-Headless -Name 'b' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--name', 'Bruno', '--cache', (Join-Path $run 'cache-b'),
        '--fake-audio', 'sine', '--duration', "$overlapSeconds")
    $procs += $b

    $null = Wait-ForLine $b '^welcome ' 20
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Assert ((Wait-Proc $b 40) -eq 0) ("b failed: " + ((Get-OutputLines $b) -join ' | '))
    $measured = $sw.Elapsed.TotalSeconds
    Assert (-not $a.Process.HasExited) 'A left before B: the overlap is shorter than B''s window (raise A''s --duration)'
    Assert ((Wait-Proc $a 40) -eq 0) ("a failed: " + ((Get-OutputLines $a) -join ' | '))

    $expected = $framesPerSecond * $overlapSeconds
    $minimum = [math]::Floor($minRatio * $expected)
    Write-Host ("  overlap: B's window {0} s ({1:F1} s measured from its welcome to its exit); expected {2} frames, minimum {3}" -f $overlapSeconds, $measured, $expected, $minimum)

    $ratios = @()
    foreach ($pair in @(@{ Me = $a; Label = 'A from B' }, @{ Me = $b; Label = 'B from A' }))
    {
        $rx = Get-ChatRx $pair.Me
        Write-Host ("  {0}: {1}" -f $pair.Label, (($rx | ForEach-Object { "client $($_.Client) $($_.Kind) $($_.Frames)" }) -join '; '))
        $voice = @($rx | Where-Object { $_.Kind -eq 'voice' })
        $instrument = @($rx | Where-Object { $_.Kind -eq 'instrument' })
        Assert ($voice.Count -eq 1) ("{0}: {1} voice streams, expected 1" -f $pair.Label, $voice.Count)
        Assert ($instrument.Count -eq 0) ("{0}: an instrument stream without a recording" -f $pair.Label)
        $ratio = $voice[0].Frames / $expected
        Assert ($voice[0].Frames -ge $minimum) ("{0}: {1} voice frames, {2:P0} of {3} (minimum {4})" -f $pair.Label, $voice[0].Frames, $ratio, $expected, $minimum)
        $ratios += ("{0} {1} ({2:P0})" -f $pair.Label, $voice[0].Frames, $ratio)
    }

    Write-Host ("  result: voice frames over {0} s (expected {1}): {2}" -f $overlapSeconds, $expected, ($ratios -join ', '))
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
