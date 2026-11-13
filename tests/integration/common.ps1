# Shared helpers for the integration scripts. Dot-source it:  . "$PSScriptRoot\common.ps1"
# Windows PowerShell 5.1 compatible (no &&, no ternary).

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
# Numbers in messages and measurements print with a dot, whatever the Windows locale.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture

$script:IntegrationDir = $PSScriptRoot
$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:OutRoot = Join-Path $script:IntegrationDir 'out'
$script:FixturesDir = Join-Path $script:RepoRoot 'tests\fixtures'

# ---------------------------------------------------------------- binaries

function Resolve-Binaries
{
    param ([string] $BuildDir = 'build', [string] $Config = 'Release')

    $root = $BuildDir
    if (-not [System.IO.Path]::IsPathRooted($root)) { $root = Join-Path $script:RepoRoot $BuildDir }

    $client = Join-Path $root "EnsayoVirtual_artefacts\$Config\Ensayo Virtual.exe"
    if (-not (Test-Path $client)) { $client = Join-Path $root "EnsayoVirtual_artefacts\$Config\EnsayoVirtual.exe" }
    $server = Join-Path $root "EnsayoServer_artefacts\$Config\EnsayoServer.exe"

    if (-not (Test-Path $client)) { throw "client binary not found under $root (build the EnsayoVirtual target, config $Config)" }
    if (-not (Test-Path $server)) { throw "server binary not found: $server (build the EnsayoServer target, config $Config)" }

    return @{ Client = (Resolve-Path $client).Path; Server = (Resolve-Path $server).Path }
}

# ---------------------------------------------------------------- run directories

function New-RunDir
{
    param ([string] $Name)

    $dir = Join-Path $script:OutRoot $Name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force $dir | Out-Null
    return (Resolve-Path $dir).Path
}

# ---------------------------------------------------------------- processes

# Starts a process with stdout/stderr redirected to <RunDir>\<Name>.out.txt / .err.txt.
# Returns a PSCustomObject { Name, Process, StdOut, StdErr }.
function Start-Proc
{
    param (
        [string] $Name,
        [string] $FilePath,
        [string[]] $Arguments,
        [string] $RunDir,
        [string] $WorkingDirectory = $null
    )

    $stdout = Join-Path $RunDir "$Name.out.txt"
    $stderr = Join-Path $RunDir "$Name.err.txt"
    if ($null -eq $WorkingDirectory -or $WorkingDirectory -eq '') { $WorkingDirectory = $RunDir }

    $quoted = @()
    foreach ($a in $Arguments)
    {
        if ($a -match '[\s"]') { $quoted += ('"' + ($a -replace '"', '\"') + '"') } else { $quoted += $a }
    }

    Write-Host ("  [{0}] {1} {2}" -f $Name, [System.IO.Path]::GetFileName($FilePath), ($quoted -join ' '))

    $p = Start-Process -FilePath $FilePath -ArgumentList ($quoted -join ' ') -WorkingDirectory $WorkingDirectory `
                       -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru -NoNewWindow
    # Touch the handle now so ExitCode is available after the process exits.
    $null = $p.Handle

    return [PSCustomObject] @{ Name = $Name; Process = $p; StdOut = $stdout; StdErr = $stderr }
}

# EnsayoServer.exe stops on stdin EOF, so it gets a stdin pipe that stays open until Stop-Procs kills it
# (Start-Process would hand it the parent's stdin, which is already at EOF when the script runs unattended).
# Hosts -Project (a project directory) and blocks until the server prints "listening port=N session=ID".
# Returns .ListenLine, .Session and .HostUser (from the "host_user <id>" line printed before it).
function Start-ServerProc
{
    param ([string] $FilePath, [int] $Port, [string] $RunDir, [string] $Project, [string[]] $ExtraArgs = @(), [double] $TimeoutSec = 10)

    if ([string]::IsNullOrEmpty($Project)) { $Project = Join-Path $RunDir 'server-project' }
    $arguments = @('--project', ('"{0}"' -f $Project), '--port', $Port) + $ExtraArgs
    Write-Host ("  [server] {0} {1}" -f [System.IO.Path]::GetFileName($FilePath), ($arguments -join ' '))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $arguments -join ' '
    $psi.WorkingDirectory = $RunDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $p = [System.Diagnostics.Process]::Start($psi)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $hostUser = $null
    $line = $null

    while ($true)
    {
        $remaining = [int][math]::Max(1, ($deadline - (Get-Date)).TotalMilliseconds)
        $read = $p.StandardOutput.ReadLineAsync()
        if (-not $read.Wait($remaining))
        {
            try { $p.Kill() } catch {}
            throw "server did not print its listening line within $TimeoutSec s"
        }

        $line = $read.Result
        if ($null -ne $line -and $line -match '^host_user (\S+)') { $hostUser = $Matches[1]; continue }
        break
    }

    if ($null -eq $line -or $line -notmatch '^listening port=\d+ session=(\S+)')
    {
        $err = ''
        try { $err = $p.StandardError.ReadToEnd().Trim() } catch {}
        try { $p.Kill() } catch {}
        throw "server failed to start on port ${Port}: '$line' $err"
    }

    $session = $Matches[1]
    Write-Host "  [server] host_user $hostUser; $line"
    return [PSCustomObject] @{ Name = 'server'; Process = $p; StdOut = $null; StdErr = $null; ListenLine = $line; Session = $session; HostUser = $hostUser }
}

# Stops a server started by Start-ServerProc the way a user does: closes its stdin (EOF) and waits for it to print
# "stopped" and exit, so the project is saved. Kills it only when it does not exit in $TimeoutSec. Returns the exit code.
function Stop-ServerProc
{
    param ($Server, [double] $TimeoutSec = 10)

    try { $Server.Process.StandardInput.Close() } catch {}
    if (-not $Server.Process.WaitForExit([int]($TimeoutSec * 1000)))
    {
        try { $Server.Process.Kill() } catch {}
        throw "the server did not stop within $TimeoutSec s of stdin EOF"
    }
    $Server.Process.WaitForExit()
    return $Server.Process.ExitCode
}

# Reads a file that another process is still writing to.
function Read-SharedFile
{
    param ([string] $Path)

    if (-not (Test-Path $Path)) { return '' }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try
    {
        $sr = New-Object System.IO.StreamReader($fs)
        return $sr.ReadToEnd()
    }
    finally { $fs.Dispose() }
}

function Get-OutputLines
{
    param ($Proc)
    $text = Read-SharedFile $Proc.StdOut
    return @($text -split "`r?`n" | Where-Object { $_ -ne '' })
}

# Polls the process's stdout file until a line matches $Pattern. Returns the matching line.
# Throws on timeout or when the process exits without printing it.
function Wait-ForLine
{
    param ($Proc, [string] $Pattern, [double] $TimeoutSec = 15)

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ($true)
    {
        foreach ($line in (Get-OutputLines $Proc))
        {
            if ($line -match $Pattern) { return $line }
        }

        if ($Proc.Process.HasExited)
        {
            # One last read: the process may have flushed on exit.
            foreach ($line in (Get-OutputLines $Proc)) { if ($line -match $Pattern) { return $line } }
            throw ("{0} exited (code {1}) before printing /{2}/. stderr: {3}" -f $Proc.Name, $Proc.Process.ExitCode, $Pattern, (Read-SharedFile $Proc.StdErr).Trim())
        }

        if ((Get-Date) -gt $deadline)
        {
            throw ("timeout ({0} s) waiting for /{1}/ from {2}. stdout so far: {3}" -f $TimeoutSec, $Pattern, $Proc.Name, ((Get-OutputLines $Proc) -join ' | '))
        }

        Start-Sleep -Milliseconds 200
    }
}

# Waits for the process to exit by itself; kills it and throws on timeout. Returns the exit code.
function Wait-Proc
{
    param ($Proc, [double] $TimeoutSec = 30)

    if (-not $Proc.Process.WaitForExit([int]($TimeoutSec * 1000)))
    {
        try { $Proc.Process.Kill() } catch {}
        throw ("{0} did not exit within {1} s" -f $Proc.Name, $TimeoutSec)
    }

    $Proc.Process.WaitForExit()
    return $Proc.Process.ExitCode
}

function Stop-Procs
{
    param ($Procs)

    foreach ($proc in @($Procs))
    {
        if ($null -eq $proc) { continue }
        try
        {
            if (-not $proc.Process.HasExited)
            {
                $proc.Process.Kill()
                $proc.Process.WaitForExit(5000) | Out-Null
            }
        }
        catch {}
    }
}

# ---------------------------------------------------------------- assertions and reporting

function Assert
{
    param ([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

function Write-Pass { param ([string] $Name) Write-Host ("PASS {0}" -f $Name) -ForegroundColor Green }
function Write-Fail { param ([string] $Name, [string] $Reason) Write-Host ("FAIL {0}: {1}" -f $Name, $Reason) -ForegroundColor Red }
function Write-Skip { param ([string] $Name, [string] $Reason) Write-Host ("SKIP {0}: {1}" -f $Name, $Reason) -ForegroundColor Yellow }

# ---------------------------------------------------------------- headless clients

# A new 32-hex id (user, track, region ids for --user and ops files).
function New-Id { return [guid]::NewGuid().ToString('N') }

# Starts `Ensayo Virtual.exe --headless <args>` with Start-Proc.
function Start-Headless
{
    param ([string] $Name, [string] $Client, [string[]] $Arguments, [string] $RunDir)
    return Start-Proc -Name $Name -FilePath $Client -RunDir $RunDir -Arguments (@('--headless') + $Arguments)
}

# The value after "<Prefix> " in the last stdout line that starts with it (or $null).
function Get-LastValue
{
    param ($Proc, [string] $Prefix)
    $hit = @(Get-OutputLines $Proc | Where-Object { $_.StartsWith("$Prefix ") }) | Select-Object -Last 1
    if ($null -eq $hit) { return $null }
    return $hit.Substring($Prefix.Length + 1).Trim()
}

# The highest "applied <version>" a headless client printed (-1 when it printed none after its welcome).
function Get-LastVersion
{
    param ($Proc)
    $max = -1
    foreach ($line in (Get-OutputLines $Proc)) { if ($line -match '^applied (\d+)$') { $max = [math]::Max($max, [int] $Matches[1]) } }
    return $max
}

# Writes one op object per line (ConvertTo-Json -Compress) to $Path.
function Write-OpsFile
{
    param ([string] $Path, [object[]] $Ops)
    $lines = @()
    foreach ($op in $Ops) { $lines += ($op | ConvertTo-Json -Compress) }
    [System.IO.File]::WriteAllLines($Path, [string[]] $lines)
}

# ---------------------------------------------------------------- WAV reading and click detection

if (-not ('EnsayoTest.Wav' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
namespace EnsayoTest
{
    public class WavInfo
    {
        public int Format; public int Channels; public int SampleRate; public int Bits; public long Frames;
        public double[][] Data;
    }

    public static class Wav
    {
        // PCM 16/24/32 and IEEE float 32 (WAVE_FORMAT_EXTENSIBLE included). Data[channel][frame].
        public static WavInfo Read (string path)
        {
            byte[] b = File.ReadAllBytes (path);
            if (b.Length < 12 || b[0] != 'R' || b[1] != 'I' || b[2] != 'F' || b[3] != 'F' || b[8] != 'W' || b[9] != 'A' || b[10] != 'V' || b[11] != 'E')
                throw new Exception ("not a RIFF/WAVE file: " + path);
            var info = new WavInfo();
            int pos = 12, dataPos = -1, dataLen = 0;
            while (pos + 8 <= b.Length)
            {
                string id = System.Text.Encoding.ASCII.GetString (b, pos, 4);
                int len = BitConverter.ToInt32 (b, pos + 4);
                if (id == "fmt ")
                {
                    info.Format = BitConverter.ToUInt16 (b, pos + 8);
                    info.Channels = BitConverter.ToUInt16 (b, pos + 10);
                    info.SampleRate = BitConverter.ToInt32 (b, pos + 12);
                    info.Bits = BitConverter.ToUInt16 (b, pos + 22);
                    if (info.Format == 0xFFFE && len >= 40) info.Format = BitConverter.ToUInt16 (b, pos + 32);
                }
                else if (id == "data") { dataPos = pos + 8; dataLen = Math.Min (len, b.Length - dataPos); break; }
                pos += 8 + len + (len & 1);
            }
            if (dataPos < 0 || info.Channels <= 0) throw new Exception ("no fmt/data chunk in " + path);
            int bytes = info.Bits / 8, frameBytes = bytes * info.Channels;
            info.Frames = dataLen / frameBytes;
            info.Data = new double[info.Channels][];
            for (int c = 0; c < info.Channels; c++) info.Data[c] = new double[info.Frames];
            for (long f = 0; f < info.Frames; f++)
                for (int c = 0; c < info.Channels; c++)
                {
                    int o = dataPos + (int) (f * frameBytes) + c * bytes;
                    double v;
                    if (info.Format == 3 && bytes == 4) v = BitConverter.ToSingle (b, o);
                    else if (bytes == 2) v = BitConverter.ToInt16 (b, o) / 32768.0;
                    else if (bytes == 3) v = ((b[o] << 8 | b[o + 1] << 16 | b[o + 2] << 24) >> 8) / 8388608.0;
                    else if (bytes == 4) v = BitConverter.ToInt32 (b, o) / 2147483648.0;
                    else throw new Exception ("unsupported sample size " + info.Bits);
                    info.Data[c][f] = v;
                }
            return info;
        }

        public static double Peak (double[] x)
        {
            double m = 0; foreach (double v in x) m = Math.Max (m, Math.Abs (v)); return m;
        }

        // Onsets: the first sample at or above `threshold` (absolute value) after at least `quiet` samples below it.
        public static long[] Onsets (double[] x, double threshold, int quiet)
        {
            var result = new List<long>();
            long lastAbove = -1L - quiet;
            for (long i = 0; i < x.Length; i++)
            {
                if (Math.Abs (x[i]) >= threshold)
                {
                    if (i - lastAbove > quiet) result.Add (i);
                    lastAbove = i;
                }
            }
            return result.ToArray();
        }

        // For each onset of `a`, the signed distance to the nearest onset of `b` (long.MaxValue when b is empty).
        public static long[] NearestErrors (long[] a, long[] b)
        {
            var errors = new long[a.Length];
            for (int i = 0; i < a.Length; i++)
            {
                long best = long.MaxValue;
                foreach (long r in b) if (Math.Abs (a[i] - r) < Math.Abs (best)) best = a[i] - r;
                errors[i] = best;
            }
            return errors;
        }
    }
}
'@
}

# ---------------------------------------------------------------- fixtures

# Writes a 16-bit stereo WAV at $Rate with a +0.9 click (3 samples) every $Interval seconds, starting at 0.
function New-ClickWav
{
    param ([string] $Path, [double] $Seconds = 4.0, [int] $Rate = 44100, [double] $Interval = 0.25)

    $channels = 2
    $frames = [int]($Rate * $Seconds)
    $clickEvery = [int][math]::Round($Rate * $Interval)
    $clickWidth = 3
    $dataBytes = $frames * $channels * 2
    $amplitude = [int16] [math]::Round(0.9 * 32767)

    $fs = [System.IO.File]::Create($Path)
    try
    {
        $w = New-Object System.IO.BinaryWriter($fs)
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
        $w.Write([int32] (36 + $dataBytes))
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
        $w.Write([int32] 16)
        $w.Write([int16] 1)                                  # PCM
        $w.Write([int16] $channels)
        $w.Write([int32] $Rate)
        $w.Write([int32] ($Rate * $channels * 2))            # byte rate
        $w.Write([int16] ($channels * 2))                    # block align
        $w.Write([int16] 16)
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
        $w.Write([int32] $dataBytes)

        $buffer = New-Object byte[] $dataBytes
        $bytes = [System.BitConverter]::GetBytes($amplitude)
        for ($start = 0; $start -lt $frames; $start += $clickEvery)
        {
            for ($k = 0; $k -lt $clickWidth -and ($start + $k) -lt $frames; $k++)
            {
                $offset = ($start + $k) * $channels * 2
                [Array]::Copy($bytes, 0, $buffer, $offset, 2)
                [Array]::Copy($bytes, 0, $buffer, $offset + 2, 2)
            }
        }
        $w.Write($buffer)
        $w.Flush()
    }
    finally { $fs.Dispose() }

    return $Path
}
