# End to end YouTube backing track with the tools the app ships: downloads a short public video's audio with
# tools\yt-dlp.exe + tools\deno.exe (no ffmpeg; m4a as YouTube serves it), then A hosts, B listens and C uploads
# the file and plays it; B's stats must show the transport playing and real audio arriving (peakReceived).
# SKIPs when the tools are missing (run tools\fetch-tools.ps1) or the download fails (no network, YouTube change).
param ([string] $BuildDir = 'build', [string] $Config = 'Release', [int] $Port = 22155,
       [string] $Url = 'https://www.youtube.com/watch?v=jNQXAC9IVRw')   # "Me at the zoo", 19 s, the first video on YouTube

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'youtube-track'
$procs = @()
$status = 1

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name

    # The app looks for the tools next to its exe; the build tree has them only when the release layout was staged,
    # so take them from the repo's tools\ folder and pass them explicitly.
    $tools = Join-Path $script:RepoRoot 'tools'
    $ytDlp = Join-Path $tools 'yt-dlp.exe'
    $deno = Join-Path $tools 'deno.exe'
    if (-not (Test-Path $ytDlp) -or -not (Test-Path $deno))
    {
        Write-Skip $name "tools\yt-dlp.exe / tools\deno.exe missing (run tools\fetch-tools.ps1)"
        exit 0
    }

    $dl = Start-Proc -Name 'yt-dlp' -FilePath $ytDlp -RunDir $run -Arguments @(
        '-f', 'bestaudio[ext=m4a]/bestaudio[ext=mp3]/18/bestaudio', '--no-playlist', '--no-update',
        '-o', (Join-Path $run 'track.%(ext)s'), '--js-runtimes', "deno:$deno", '--', $Url)
    $dlExit = Wait-Proc $dl 120
    $dlOut = Get-OutputLines $dl
    Write-Host "  yt-dlp: $(($dlOut | Where-Object { $_ -notmatch '^\[download\]\s+\d' }) -join ' | ')"
    $track = Get-ChildItem $run -Filter 'track.*' | Select-Object -First 1
    if ($dlExit -ne 0 -or $null -eq $track)
    {
        Write-Skip $name "download failed (exit $dlExit): $((Read-SharedFile $dl.StdErr).Trim())"
        exit 0
    }
    Assert ($dlOut -join "`n" -match 'Solving JS challenges using deno') 'yt-dlp did not use the bundled deno'
    Write-Host ("  downloaded {0} ({1:N0} KB)" -f $track.Name, ($track.Length / 1KB))
    # Keep a copy as the (git-ignored) fixture the MediaFoundationAudioFormat unit test decodes.
    if ($track.Extension -eq '.m4a') { Copy-Item $track.FullName (Join-Path $script:FixturesDir 'youtube-sample.m4a') -Force }

    $bStats = Join-Path $run 'B.stats.jsonl'
    $server = Start-ServerProc -FilePath $bin.Server -Port $Port -RunDir $run
    $procs += $server

    $a = Start-Proc -Name 'A' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', "ensayo://127.0.0.1:$Port/NEW", '--name', 'A', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--duration', '16')
    $procs += $a
    $linkLine = Wait-ForLine $a '^link ensayo://' 15
    $link = ($linkLine -split '\s+')[1]
    Wait-ForLine $a '^joined ' 10 | Out-Null

    $b = Start-Proc -Name 'B' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'B', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--stats', $bStats, '--duration', '14')
    $procs += $b
    Wait-ForLine $b '^joined ' 10 | Out-Null
    Wait-ForLine $b '^participants 2$' 10 | Out-Null

    $c = Start-Proc -Name 'C' -FilePath $bin.Client -RunDir $run -Arguments @(
        '--headless', '--join', $link, '--name', 'C', '--role', 'both',
        '--fake-audio', 'silence', '--block', '128', '--track', $track.FullName, '--play', '--duration', '10')
    $procs += $c
    Wait-ForLine $c '^joined ' 10 | Out-Null

    $cExit = Wait-Proc $c 30
    $bExit = Wait-Proc $b 30
    $aExit = Wait-Proc $a 30

    Write-Host "  C: $((Get-OutputLines $c) -join ' | ')"
    Assert ($cExit -eq 0) "C (uploader) exited with code $cExit (stderr: $((Read-SharedFile $c.StdErr).Trim()))"
    Assert ($bExit -eq 0) "B exited with code $bExit"
    Assert ($aExit -eq 0) "A exited with code $aExit"

    $statsLines = @((Read-SharedFile $bStats) -split "`r?`n" | Where-Object { $_ -ne '' })
    $playing = @($statsLines | Where-Object { $_ -match '"trackPlaying":true' })
    $peaks = @($statsLines | ForEach-Object { ($_ | ConvertFrom-Json).peakReceived })
    $maxPeak = ($peaks | Measure-Object -Maximum).Maximum
    Write-Host ("  B stats: {0} lines, {1} with trackPlaying:true, max peakReceived {2}" -f $statsLines.Count, $playing.Count, $maxPeak)
    Assert ($playing.Count -ge 1) 'B stats never showed "trackPlaying":true'
    Assert ($maxPeak -gt 0.02) "B never received audible audio from the track (max peak $maxPeak)"

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
