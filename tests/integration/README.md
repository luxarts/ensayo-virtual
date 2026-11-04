# Integration scripts

End-to-end checks that drive the real `EnsayoServer.exe` and `Ensayo Virtual.exe --headless` binaries on one
machine (Windows PowerShell 5.1). Build the Release targets first:

```powershell
cmake --build build --config Release --target EnsayoVirtual EnsayoServer -- /m
powershell -NoProfile -ExecutionPolicy Bypass -File tests\integration\run-all.ps1
```

Every script takes `-BuildDir` (default `build`, relative to the repo root) and `-Config` (default `Release`),
prints `PASS <name>`, `FAIL <name>: <reason>` or `SKIP <name>: <reason>` as its last line and exits non-zero on
failure. Process output, click logs (`*.csv`) and stats (`*.jsonl`) land in `tests/integration/out/<name>/`
(ignored by git) so a failing run can be inspected.

| Script              | Port  | What it checks                                                                                   |
|---------------------|-------|--------------------------------------------------------------------------------------------------|
| `two-instances.ps1` | 22150 | Standalone server; A emits clicks, B probes them: `participants 2`, `n >= 10`, median `< 40 ms`. |
| `embedded-host.ps1` | 22151 | Same, but A hosts the session in-process (`--host`) and B joins the printed link.               |
| `backing-track.ps1` | 22152 | A creates, B probes, C uploads `tests/fixtures/click.wav` and plays it: B detects `>= 8` clicks and its stats show `"trackPlaying":true`. |
| `packet-loss.ps1`   | 22153 | As two-instances with B `--simulate-loss 10`: B exits 0 and still pairs `n >= 5` clicks.        |
| `asio-loopback.ps1` | 22154 | Real hardware: A opens `-Driver` (default `MiniFuse ASIO Driver`) with `--out 1 --mic 2 --inst 3` (its LOOPBACK outputs feed inputs 2/3); B (fake) emits clicks and probes them after the full round trip through the driver: driver opens at 48000/`-Block`, A runs the expected callback count with 0 underruns, `n >= 10`, median `< 80 ms`. SKIP when the driver is not in `HKLM\SOFTWARE\ASIO`. |
| `youtube-track.ps1` | 22155 | Downloads a 19 s public video's audio with `tools\yt-dlp.exe` + `tools\deno.exe` (m4a, no ffmpeg; asserts deno solved the JS challenge), then C uploads it and plays it: B's stats show `trackPlaying:true` and `peakReceived > 0.02`. SKIP when the tools are missing or the download fails. |
| `screenshots.ps1`   | -     | `--screenshot`: `lobby.png`/`session.png` are 1100x760 and `controls.json` contains every name of `expected-controls.json`, visible with `w,h > 0` (SKIP when the build has no screenshot mode). Then captures the real `--demo` window to `out/screenshots/shots/demo-window.png`. |
| `run-all.ps1`       | -     | Runs all of the above in separate PowerShell processes and prints a summary table.              |

`common.ps1` is dot-sourced by the others: binary resolution, `Start-Proc` (stdout/stderr redirected to files),
`Start-ServerProc` (keeps the server's stdin open, since `EnsayoServer.exe` stops on stdin EOF), `Wait-ForLine`,
`Wait-Proc`, `Stop-Procs`, `Assert`, latency-line parsing and the `click.wav` fixture generator (4 s, 44.1 kHz,
16-bit stereo, a +0.9 click every 250 ms; written on demand, not committed).

Latency is one-way: the QPC timestamp of each click emitted by A's fake input, paired by B with the detection
of that click in its device output. Both processes run on the same machine, so they share the clock.
