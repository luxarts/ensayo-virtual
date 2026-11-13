# Shared helpers for the integration scripts. Dot-source it:  . "$PSScriptRoot\common.ps1"
# Windows PowerShell 5.1 compatible (no &&, no ternary).

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

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

# ---------------------------------------------------------------- fixtures

# tests\fixtures\click.wav: 44.1 kHz, 16-bit stereo, +0.9 click (3 samples) every 250 ms.
function Get-ClickWav
{
    param ([double] $Seconds = 4.0)

    $path = Join-Path $script:FixturesDir 'click.wav'
    if (Test-Path $path) { return $path }

    New-Item -ItemType Directory -Force $script:FixturesDir | Out-Null

    $sampleRate = 44100
    $channels = 2
    $bits = 16
    $frames = [int]($sampleRate * $Seconds)
    $clickEvery = [int]($sampleRate / 4)
    $clickWidth = 3
    $dataBytes = $frames * $channels * ($bits / 8)
    $amplitude = [int16] [math]::Round(0.9 * 32767)

    $fs = [System.IO.File]::Create($path)
    try
    {
        $w = New-Object System.IO.BinaryWriter($fs)
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('RIFF'))
        $w.Write([int32] (36 + $dataBytes))
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('WAVE'))
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('fmt '))
        $w.Write([int32] 16)
        $w.Write([int16] 1)                                        # PCM
        $w.Write([int16] $channels)
        $w.Write([int32] $sampleRate)
        $w.Write([int32] ($sampleRate * $channels * ($bits / 8)))  # byte rate
        $w.Write([int16] ($channels * ($bits / 8)))                # block align
        $w.Write([int16] $bits)
        $w.Write([System.Text.Encoding]::ASCII.GetBytes('data'))
        $w.Write([int32] $dataBytes)

        $buffer = New-Object byte[] $dataBytes
        for ($i = 0; $i -lt $frames; $i++)
        {
            if (($i % $clickEvery) -lt $clickWidth)
            {
                $bytes = [System.BitConverter]::GetBytes($amplitude)
                $offset = $i * $channels * 2
                [Array]::Copy($bytes, 0, $buffer, $offset, 2)
                [Array]::Copy($bytes, 0, $buffer, $offset + 2, 2)
            }
        }
        $w.Write($buffer)
        $w.Flush()
    }
    finally { $fs.Dispose() }

    Write-Host "  generated fixture $path"
    return $path
}
