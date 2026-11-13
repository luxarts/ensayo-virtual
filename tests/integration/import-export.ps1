# import-export (port 22244): A (the host user) imports a generated 44.1 kHz stereo WAV (3 s, a click every 250 ms)
# and exports the mix. Then, only when yt-dlp.exe and deno.exe are in tools\ next to the built exe (or in the repo's
# tools\), a second run imports a YouTube URL with --import-url and exports again; that part prints SKIP (and the
# script still passes) when the tools are missing or the download fails.
# Asserts for each export: PCM 24-bit, 48000 Hz, 2 channels, length = the project's end (max region end, from
# --dump-project). The WAV import is resampled exactly: 144000 samples, and the exported clicks sit at k * 12000 (+-1).
# The YouTube take is an imported stereo track whose region covers the whole take.
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [string] $YouTubeUrl = 'https://www.youtube.com/watch?v=jNQXAC9IVRw')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'import-export'
$port = 22244
$procs = @()
$status = 1

function Get-ProjectEnd
{
    param ($Project)
    $end = [int64] 0
    foreach ($t in @($Project.tracks)) { foreach ($r in @($t.regions)) { $end = [math]::Max($end, [int64] $r.start + [int64] $r.length) } }
    return $end
}

function Test-Export
{
    param ([string] $Wav, [int64] $End)
    $info = [EnsayoTest.Wav]::Read($Wav)
    Write-Host ("  {0}: format {1}, {2} bit, {3} Hz, {4} ch, {5} frames (project end {6})" -f [System.IO.Path]::GetFileName($Wav), $info.Format, $info.Bits, $info.SampleRate, $info.Channels, $info.Frames, $End)
    Assert ($info.Format -eq 1 -and $info.Bits -eq 24) "export is not 24-bit PCM ($($info.Format)/$($info.Bits))"
    Assert ($info.SampleRate -eq 48000) "export rate $($info.SampleRate)"
    Assert ($info.Channels -eq 2) "export has $($info.Channels) channels"
    Assert ($info.Frames -eq $End) "export has $($info.Frames) frames, project end is $End"
    return $info
}

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name

    $server = Start-ServerProc -FilePath $bin.Server -Port $port -RunDir $run -Project (Join-Path $run 'project')
    $procs += $server
    $link = "ensayo://127.0.0.1:$port/$($server.Session)"
    $cacheA = Join-Path $run 'cache-a'
    $wav = New-ClickWav -Path (Join-Path $run 'Clicks 44k.wav') -Seconds 3 -Rate 44100 -Interval 0.25

    # ---- 1. 44.1 kHz WAV
    $mix1 = Join-Path $run 'mix-wav.wav'
    $dump1 = Join-Path $run 'project-wav.json'
    $a1 = Start-Headless -Name 'a-wav' -Client $bin.Client -RunDir $run -Arguments @(
        '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', $cacheA,
        '--import', $wav, '--duration', '0.5', '--export', $mix1, '--dump-project', $dump1)
    $procs += $a1
    Assert ((Wait-Proc $a1 120) -eq 0) ("import of the WAV failed: " + ((Get-OutputLines $a1) -join ' | '))

    $p1 = Get-Content -Raw $dump1 | ConvertFrom-Json
    $track = @($p1.tracks)[0]
    $take = @($p1.takes | Where-Object { $_.id -eq @($track.regions)[0].take })[0]
    Write-Host ("  imported track '{0}' ({1}), take {2} ch {3} samples, region {{{4}, {5}, {6}}}" -f $track.name, $track.kind, $take.channels, $take.samples, @($track.regions)[0].start, @($track.regions)[0].offset, @($track.regions)[0].length)
    Assert ($track.kind -eq 'imported' -and $track.name -eq 'Clicks 44k') "unexpected track $($track.name)/$($track.kind)"
    Assert ([int64] $take.samples -eq 144000 -and [int] $take.channels -eq 2 -and $take.complete) 'the imported take is not 144000 stereo samples, complete'

    $info = Test-Export $mix1 (Get-ProjectEnd $p1)
    $onsets = [EnsayoTest.Wav]::Onsets($info.Data[0], 0.5 * [EnsayoTest.Wav]::Peak($info.Data[0]), 2000)
    $expected = @(1..11 | ForEach-Object { [long] ($_ * 12000) })
    $errors = [EnsayoTest.Wav]::NearestErrors([long[]] $expected, $onsets)
    $maxError = 0
    foreach ($e in $errors) { $maxError = [math]::Max($maxError, [math]::Abs($e)) }
    Write-Host ("  exported clicks: {0} onsets, max |error| vs k*12000 = {1} samples" -f $onsets.Count, $maxError)
    Assert ($maxError -le 1) "exported clicks are off by up to $maxError samples"
    $result = "wav 144000 samples, export 24/48/2 $($info.Frames) frames, clicks +-$maxError"

    # ---- 2. YouTube URL (optional)
    $exeTools = Join-Path (Split-Path $bin.Client) 'tools'
    $repoTools = Join-Path $script:RepoRoot 'tools'
    $hasTools = ((Test-Path (Join-Path $exeTools 'yt-dlp.exe')) -and (Test-Path (Join-Path $exeTools 'deno.exe'))) -or
                ((Test-Path (Join-Path $repoTools 'yt-dlp.exe')) -and (Test-Path (Join-Path $repoTools 'deno.exe')))

    if (-not $hasTools)
    {
        Write-Host '  youtube: SKIP (tools\yt-dlp.exe and tools\deno.exe not found)'
        $result += '; youtube SKIP (no tools)'
    }
    elseif (-not (Test-Path (Join-Path $exeTools 'yt-dlp.exe')))
    {
        Write-Host "  youtube: SKIP (the tools are in the repo but not next to the built exe in $exeTools; rebuild copies them)"
        $result += '; youtube SKIP (tools not next to the exe)'
    }
    else
    {
        $mix2 = Join-Path $run 'mix-youtube.wav'
        $dump2 = Join-Path $run 'project-youtube.json'
        $a2 = Start-Headless -Name 'a-youtube' -Client $bin.Client -RunDir $run -Arguments @(
            '--join', $link, '--user', $server.HostUser, '--name', 'Ana', '--cache', $cacheA,
            '--import-url', $YouTubeUrl, '--duration', '0.5', '--export', $mix2, '--dump-project', $dump2)
        $procs += $a2
        $code = Wait-Proc $a2 600
        $ytError = @(Get-OutputLines $a2 | Where-Object { $_ -match '^error youtube: ' }) | Select-Object -First 1

        if ($code -ne 0 -and $null -ne $ytError)
        {
            Write-Host "  youtube: SKIP (download failed: $ytError)"
            $result += '; youtube SKIP (download failed)'
        }
        else
        {
            Assert ($code -eq 0) ("YouTube import failed: " + ((Get-OutputLines $a2) -join ' | '))
            Write-Host ("  youtube: {0}" -f (Wait-ForLine $a2 '^downloaded ' 1))
            $p2 = Get-Content -Raw $dump2 | ConvertFrom-Json
            $yt = @($p2.tracks | Where-Object { $_.id -ne $track.id })[0]
            Assert ($null -ne $yt -and $yt.kind -eq 'imported') 'no imported YouTube track'
            $ytRegion = @($yt.regions)[0]
            $ytTake = @($p2.takes | Where-Object { $_.id -eq $ytRegion.take })[0]
            Write-Host ("  youtube track '{0}', take {1} ch {2} samples ({3:N1} s), region {{{4}, {5}, {6}}}" -f $yt.name, $ytTake.channels, $ytTake.samples, ($ytTake.samples / 48000.0), $ytRegion.start, $ytRegion.offset, $ytRegion.length)
            Assert ($ytTake.complete -and [int64] $ytRegion.start -eq 0 -and [int64] $ytRegion.offset -eq 0 -and [int64] $ytRegion.length -eq [int64] $ytTake.samples) 'the YouTube region does not cover its complete take'
            Assert ([int64] $ytTake.samples -gt 48000) 'the YouTube take is shorter than 1 s'
            $info2 = Test-Export $mix2 (Get-ProjectEnd $p2)
            $result += ("; youtube {0:N1} s, export {1} frames" -f ($ytTake.samples / 48000.0), $info2.Frames)
        }
    }

    Write-Host "  result: $result"
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
