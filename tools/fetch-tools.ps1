# Downloads the helper binaries the app ships in <exe dir>\tools for YouTube backing tracks:
#   yt-dlp.exe  (Unlicense)  https://github.com/yt-dlp/yt-dlp
#   deno.exe    (MIT)        https://github.com/denoland/deno  - the JavaScript runtime yt-dlp needs for YouTube
# No ffmpeg: the audio is downloaded as m4a/AAC and decoded by Windows Media Foundation in the server.
# Usage:  .\tools\fetch-tools.ps1 [-Destination <dir>]   (default: this folder). Needs the GitHub CLI (gh) or curl.
param ([string] $Destination = $PSScriptRoot, [string] $DenoVersion = 'v2.9.8')

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Destination | Out-Null

$ytDlpUrl = 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe'
$denoUrl = "https://github.com/denoland/deno/releases/download/$DenoVersion/deno-x86_64-pc-windows-msvc.zip"

Write-Host "Downloading yt-dlp.exe (latest) ..."
Invoke-WebRequest -UseBasicParsing -Uri $ytDlpUrl -OutFile (Join-Path $Destination 'yt-dlp.exe')

Write-Host "Downloading deno $DenoVersion ..."
$zip = Join-Path $env:TEMP 'deno-x86_64-pc-windows-msvc.zip'
Invoke-WebRequest -UseBasicParsing -Uri $denoUrl -OutFile $zip
Expand-Archive -Path $zip -DestinationPath $Destination -Force
Remove-Item $zip

Get-ChildItem $Destination -Filter '*.exe' | ForEach-Object { "{0,-12} {1,10:N0} KB  {2}" -f $_.Name, ($_.Length / 1KB), (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
