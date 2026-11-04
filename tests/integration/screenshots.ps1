# 1. `--screenshot <dir>`: renders lobby.png, session.png and controls.json; every control named in
#    expected-controls.json must be present, visible and have w/h > 0.
# 2. `--demo`: launches the real window, captures it with PrintWindow (CopyFromScreen as fallback) to
#    out\shots\demo-window.png for visual inspection, then kills it.
# Prints SKIP when the build has no screenshot mode (exit code 2).
param ([string] $BuildDir = 'build', [string] $Config = 'Release')

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$name = 'screenshots'
$procs = @()
$status = 1
$skipReason = $null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

if (-not ('EnsayoNative.Win32' -as [type]))
{
    Add-Type -Namespace EnsayoNative -Name Win32 -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
'@
}

function Test-ControlList
{
    param ([string] $ControlsJson, [string] $ExpectedJson)

    $controls = @((Get-Content -Raw $ControlsJson | ConvertFrom-Json))
    $expected = Get-Content -Raw $ExpectedJson | ConvertFrom-Json
    # controls.json holds one entry per control per rendered view, so look names up per view.
    $byViewAndName = @{}
    foreach ($c in $controls) { $byViewAndName["$($c.view)/$($c.name)"] = $c }

    $problems = @()
    foreach ($view in @('lobby', 'session'))
    {
        $names = @($expected.$view)
        foreach ($n in $names)
        {
            $key = "$view/$n"
            if (-not $byViewAndName.ContainsKey($key)) { $problems += "$n missing in view '$view'"; continue }
            $c = $byViewAndName[$key]
            if (-not $c.visible) { $problems += "$n not visible in view '$view'" }
            if ($c.w -le 0 -or $c.h -le 0) { $problems += "$n has empty bounds ($($c.w)x$($c.h)) in view '$view'" }
        }
        Write-Host ("  {0}: {1} expected controls checked" -f $view, $names.Count)
    }
    return $problems
}

function Test-PngSize
{
    param ([string] $Path, [int] $Width, [int] $Height)

    $img = [System.Drawing.Image]::FromFile($Path)
    try { return ($img.Width -eq $Width -and $img.Height -eq $Height) }
    finally { $img.Dispose() }
}

function Save-WindowCapture
{
    param ([IntPtr] $Handle, [string] $Path)

    $rect = New-Object EnsayoNative.Win32+RECT
    [EnsayoNative.Win32]::GetWindowRect($Handle, [ref] $rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    Assert ($w -gt 0 -and $h -gt 0) "window rect is empty ($w x $h)"

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try
    {
        $hdc = $g.GetHdc()
        $ok = [EnsayoNative.Win32]::PrintWindow($Handle, $hdc, 2)   # PW_RENDERFULLCONTENT
        $g.ReleaseHdc($hdc)

        if (-not $ok)
        {
            Write-Host '  PrintWindow failed, falling back to CopyFromScreen'
            [EnsayoNative.Win32]::SetForegroundWindow($Handle) | Out-Null
            Start-Sleep -Milliseconds 500
            $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
        }

        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally
    {
        $g.Dispose()
        $bmp.Dispose()
    }
    return @{ Width = $w; Height = $h }
}

try
{
    $bin = Resolve-Binaries -BuildDir $BuildDir -Config $Config
    $run = New-RunDir $name
    $shots = Join-Path $run 'shots'
    New-Item -ItemType Directory -Force $shots | Out-Null

    # ---- 1. --screenshot
    $shot = Start-Proc -Name 'screenshot' -FilePath $bin.Client -RunDir $run -Arguments @('--screenshot', $shots)
    $procs += $shot
    $shotExit = Wait-Proc $shot 60

    if ($shotExit -eq 2)
    {
        $skipReason = 'screenshot mode not built (--screenshot exited with code 2)'
        Write-Host "  $skipReason"
    }
    else
    {
        Assert ($shotExit -eq 0) "--screenshot exited with code $shotExit (stderr: $((Read-SharedFile $shot.StdErr).Trim()))"

        foreach ($png in @('lobby.png', 'session.png'))
        {
            $path = Join-Path $shots $png
            Assert (Test-Path $path) "$png was not written"
            Assert (Test-PngSize $path 1100 760) "$png is not 1100x760"
        }

        $controlsJson = Join-Path $shots 'controls.json'
        $expectedJson = Join-Path $script:IntegrationDir 'expected-controls.json'
        Assert (Test-Path $controlsJson) 'controls.json was not written'
        Assert (Test-Path $expectedJson) 'tests/integration/expected-controls.json is missing'

        $problems = @(Test-ControlList $controlsJson $expectedJson)
        Assert ($problems.Count -eq 0) ("controls.json problems: " + ($problems -join '; '))
        Write-Host "  lobby.png, session.png and controls.json OK in $shots"
    }

    # ---- 2. real window capture of --demo
    $demo = Start-Proc -Name 'demo' -FilePath $bin.Client -RunDir $run -Arguments @('--demo')
    $procs += $demo

    $handle = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(15)
    while ($handle -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
    {
        Start-Sleep -Milliseconds 250
        $demo.Process.Refresh()
        if ($demo.Process.HasExited) { throw "--demo exited early with code $($demo.Process.ExitCode) (another instance already running?)" }
        $handle = $demo.Process.MainWindowHandle
    }
    Assert ($handle -ne [IntPtr]::Zero) '--demo never showed a main window'

    Start-Sleep -Seconds 2   # let the demo session render
    $capture = Join-Path $shots 'demo-window.png'
    $size = Save-WindowCapture $handle $capture
    Assert ((Get-Item $capture).Length -gt 0) 'demo-window.png is empty'
    Write-Host ("  demo-window.png captured ({0}x{1}) at {2}" -f $size.Width, $size.Height, $capture)

    Stop-Procs @($demo)

    if ($null -ne $skipReason)
    {
        Write-Skip $name $skipReason
        $status = 0
    }
    else
    {
        Write-Pass $name
        $status = 0
    }
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
