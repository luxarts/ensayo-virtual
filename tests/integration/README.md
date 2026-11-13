# Integration scripts

End-to-end checks that drive the real `EnsayoServer.exe` and `Ensayo Virtual.exe --headless` binaries on one
machine (Windows PowerShell 5.1). Build the Release targets first:

```powershell
cmake --build build --config Release --target EnsayoVirtual EnsayoServer -- /m
powershell -NoProfile -ExecutionPolicy Bypass -File tests\integration\run-all.ps1
```

Every script takes `-BuildDir` (default `build`, relative to the repo root) and `-Config` (default `Release`),
prints `PASS <name>`, `FAIL <name>: <reason>` or `SKIP <name>: <reason>` as its last line and exits non-zero on
failure. Process output lands in `tests/integration/out/<name>/`
(ignored by git) so a failing run can be inspected.

| Script              | Port  | What it checks                                                                                   |
|---------------------|-------|--------------------------------------------------------------------------------------------------|
| `screenshots.ps1`   | -     | `--screenshot`: `lobby.png`/`session.png` are 1100x760 and `controls.json` contains every name of `expected-controls.json`, visible with `w,h > 0` (SKIP when the build has no screenshot mode). Then captures the real `--demo` window to `out/screenshots/shots/demo-window.png`. |
| `run-all.ps1`       | -     | Runs all of the above in separate PowerShell processes and prints a summary table.              |

`run-all.ps1` currently runs only `screenshots.ps1`.

`common.ps1` is dot-sourced by the others: binary resolution, `Start-Proc` (stdout/stderr redirected to files),
`Start-ServerProc` (`EnsayoServer.exe --project <dir> --port N`; keeps the server's stdin open, since it stops on
stdin EOF, and returns the `session=` id and the `host_user` id), `Wait-ForLine`, `Wait-Proc`, `Stop-Procs`, `Assert`
and the `click.wav` fixture generator (4 s, 44.1 kHz,
16-bit stereo, a +0.9 click every 250 ms; written on demand, not committed).
