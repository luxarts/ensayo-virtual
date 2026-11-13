# Integration scripts

End-to-end checks that drive the real `EnsayoServer.exe` and `Ensayo Virtual.exe --headless` binaries on one
machine (Windows PowerShell 5.1). Build the Release targets first:

```powershell
cmake --build build --config Release -- /m
powershell -NoProfile -ExecutionPolicy Bypass -File tests\integration\run-all.ps1
```

Every script takes `-BuildDir` (default `build`, relative to the repo root) and `-Config` (default `Release`),
prints `PASS <name>`, `FAIL <name>: <reason>` or `SKIP <name>: <reason>` as its last line and exits non-zero on
failure. A `  result: ...` line before it carries the measurements, which `run-all.ps1` shows in its summary.
Process output (`<process>.out.txt` / `.err.txt`), project directories, caches and exported WAVs land in
`tests/integration/out/<name>/` (ignored by git) so a failing run can be inspected. Every process a script starts is
stopped at the end; nothing else is touched.

| Script                 | Port  | What it checks |
|------------------------|-------|----------------|
| `screenshots.ps1`      | -     | `--screenshot`: `lobby.png`/`session.png` are 1100x760 and `controls.json` contains every name of `expected-controls.json` (lobby; DAW demo with 4 tracks, one recording live, one downloading), visible with `w,h > 0`. Then captures the real `--demo` window to `out/screenshots/shots/demo-window.png`; that part is skipped with the reason when an Ensayo Virtual is already running (`--demo` would only forward to it). |
| `record-sync.ps1`      | 22240 | A (host user) imports an 8 s click track; B records 6 s over it with `--fake-audio loopback --latency-in 200 --latency-out 264 --block 256`. A prints at least 3 distinct `cached <B's take> <k>` lines before B's `take_complete`; A and B end at the same version with the same `project_hash`; B's region starts at 48000 with offset 464; the mix exported with only B's track audible and with only the click track audible (mutes sent with `--ops`) have every click at the same sample (limit +-1). |
| `edit-sync.ps1`        | 22241 | A, B, C: B records, then sends add_track, add_region on its take, split, move, trim, set_mix and a set_mix on A's track. B prints exactly one `rejected`; A, B and C print the same `project_hash`; A's dump holds the edited regions and mix. |
| `reconnect.ps1`        | 22242 | B's process is killed about 3 s into a 6 s take, then restarted with the same `--user`/`--cache` and `--resume`. The take is incomplete until then; afterwards it is complete on the host with segment files exactly `0..n-1`, the same sample count on A and B, and one region covering it. |
| `reopen.ps1`           | 22243 | The server is stopped (stdin EOF) and started again on the same project dir: same session id and host user; B reconnects with the old link and gets the same `project_hash`; a new client with an empty cache downloads the take and gets the same tracks and takes. |
| `import-export.ps1`    | 22244 | Imports a 44.1 kHz stereo WAV (resampled to exactly 144000 samples, exported clicks at `k * 12000` +-1) and, when `yt-dlp.exe` and `deno.exe` are next to the built exe, a YouTube URL with `--import-url` (SKIP of that part only when the tools are missing or the download fails for a network reason; any other yt-dlp error FAILs). Each export is 24-bit PCM, 48 kHz, stereo, with length = the project's end. |
| `asio-calibration.ps1` | 22245 | Real hardware: `--asio "MiniFuse ASIO Driver" --out 1 --mic 2 --inst 3 --mute-monitor --calibrate`, then a recording of a click track through the MiniFuse LOOPBACK outputs; the recorded clicks align within +-48 samples (1 ms). SKIP only when the driver is not installed or step 1 cannot open it with one of JUCE's absent/busy-device errors (`No Driver`, `Driver failed to initialise`, `No such device`, `Can't start device`, `Device didn't start correctly`, or `ASIO device did not open`); any later failure FAILs. The busy path is untested here: the MiniFuse driver accepts several programs at once. |
| `run-all.ps1`          | -     | Runs all of the above in separate PowerShell processes and prints a summary table. |

Ports 22240-22245 are used only by these scripts (the unit tests use 22200-22260 outside that range), so the two can
run one after the other on the same machine; do not run two copies of a script at the same time.

`common.ps1` is dot-sourced by the others: binary resolution, `Start-Proc` / `Start-Headless` (stdout/stderr redirected
to files), `Start-ServerProc` (`EnsayoServer.exe --project <dir> --port N`; keeps the server's stdin open, since it
stops on stdin EOF, and returns the `session=` id and the `host_user` id), `Stop-ServerProc` (closes stdin and waits
for a clean exit, so the project is saved), `Wait-ForLine`, `Wait-Proc`, `Stop-Procs`, `Assert`, `Write-OpsFile`
(one JSON op per line for `--ops`), `New-ClickWav` (16-bit stereo WAV with a +0.9 click every N seconds) and the
`EnsayoTest.Wav` C# helper (reads PCM/float WAVs, finds click onsets: the first sample at or above half the file's
peak after 2000 quiet samples).
