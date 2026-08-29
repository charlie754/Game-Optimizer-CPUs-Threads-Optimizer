# Game Optimizer

**CPU threads optimizer.**

Per-game CPU Set isolation for split-topology CPUs — the isolation of BIOS "Turbo Game Mode",
applied only to the game you chose, and only while it is running.

On a dual-CCD AMD part (7950X3D, 7900X3D, 9950X3D) one CCD carries 3D V-Cache and the other
clocks higher. Some games run better pinned to the cache CCD with nothing else on it. The
existing ways to arrange that — a BIOS game mode, or letting Windows and AMD's driver decide —
are session-wide: they cost you half the machine for Discord, OBS, browsers and compilers too.

Game Optimizer applies the isolation to one process tree, keeps the other CCD free for everything
else, and clears every mask the moment the game exits.

- **CPU Sets only.** `SetProcessDefaultCpuSets`, never `SetProcessAffinityMask`.
- **Child processes included.** A worker spawned twenty minutes into a session is picked up
  within one poll period. This is not optional — CPU sets are *not* inherited by children.
- **Nothing global.** No CCD parking, no system policy, no driver, no reboot.
- **No elevation, no injection, no overlay.** One native exe, no .NET runtime.
- **Local config only.** No account, no telemetry, and the app itself makes no network
  requests. See the WebView2 note below for the one component that is a browser engine.
- **One optional runtime dependency: WebView2.** It is used for the sponsor strip in the
  Settings window and for nothing else. See below — the app runs, and looks right, without it.

## WebView2 — the one optional runtime dependency

It was true until 2026-08-29 that this app had **no** runtime dependency at all. That sentence
is corrected here rather than left standing.

The three sponsor buttons at the bottom of the Settings window are the author's own browser
extension elements — the Ko-fi button, a GitHub star button, and the animated GoatProject
lockup with its meteors, spark and glyph cross-fade. They were first hand-ported into GDI
drawing code, which cannot reproduce CSS transitions, blur filters and a radial mask exactly
and never looked right. They are now **copied verbatim** and rendered by an embedded
**Microsoft Edge WebView2**, so the pixels are the original's pixels.

What that does and does not mean:

- **It is optional.** If the WebView2 runtime is not installed, or `WebView2Loader.dll` is
  missing, or creation fails for any other reason, the Settings window shows the original GDI
  sponsor strip in exactly the same place. **The app always starts and never loses the strip.**
  The *loader-missing* path was exercised deliberately rather than reasoned about: the loader
  filename was pointed at a file that does not exist, the app was rebuilt and run, and the GDI
  strip appeared at its own size with the window healthy. The later failure points — runtime
  absent, environment or controller creation refused — run the same fallback but have not each
  been forced individually on this machine.
- **It is lazy.** Nothing web-related is created until the Settings window opens, and it is
  all destroyed when that window closes. While the app sits in the tray — which is nearly all
  of its life — there is no browser and no extra process. Nothing on the startup path touches
  it.
- **The page is inside the binary.** It is generated into `src\sponsor_html.h` by
  `python tools\gen-sponsor-html.py`, which is run by hand and whose output is committed with
  the source. The app never reads the extension's files at run time and does not depend on any
  path outside its own tree.
- **Links leave.** Every navigation the page attempts is cancelled and handed to
  `ShellExecuteW`, so a click opens your normal browser. Only the three URLs in
  `src\sponsor.h` are ever opened; anything else is refused and logged.
- **On network access:** the app makes no requests, and the page is loaded from a string in
  the binary with every navigation cancelled — so nothing in this repository fetches anything.
  WebView2 is nevertheless a full browser engine and this project has not audited what the
  Microsoft runtime may do on its own. If that matters to you, do not install WebView2: the
  app then uses the GDI strip and this component never loads.

`WebView2Loader.dll` is Microsoft's redistributable loader and is vendored under
`third_party\webview2\x64\`; see `NOTICE.md`. It is loaded with `LoadLibraryW` at run time and
is deliberately **not** in the exe's import table, so its absence can never stop the app
starting.

## Status

**Pre-release.** Read [what has actually been tested](#what-has-actually-been-tested) before
using this on anything you care about.

## Build

Requires MSVC (Build Tools 2019 or newer) and a Windows 10 SDK. No other dependencies.

```bat
tools\build.bat
```

Produces `build\GameOptimizer.exe`. Run `tools\gate-a.bat` for the build + unit-test gate.

Two diagnostic probes report what this machine actually looks like and how it actually
behaves; their output is the evidence behind `docs\spec\04-measurements.md`:

```bat
tools\build-probe.bat      && build\topology_probe.exe
tools\build-behaviour.bat  && build\behaviour_probe.exe
```

## Use

Run it. It lives in the tray.

On first launch it detects your topology, shows you a core map, and asks you to confirm it
before anything is applied. Then pick a game — from the running-process list or by browsing to
an .exe — and choose which named mask it should get.

On a 9950X3D the derived masks are `Cache`, `Cache no SMT`, `Freq`, `Freq no SMT`, `All` and
`All no SMT`. On Intel hybrid they are `P-cores` / `E-cores` variants. On a symmetric dual-CCD
part they are `CCD0` / `CCD1`, and the app tells you it cannot determine which is better. On a
single-CCD part it says plainly that no split exists and only the SMT masks will help.

A profile is a game plus a list of "heavy apps". The game and its descendants get one mask;
the heavy apps get the other, for as long as the game runs. Optionally, any process that
sustains more than N% CPU for M seconds while the game is in the foreground is moved to the
background mask too.

Configuration is a plain INI file at `%LOCALAPPDATA%\GameOptimizer\config.ini`, editable by hand.

### Upgrading from the "CoreDirector" builds

The product was renamed. Two things move on first start of a renamed build, automatically and
once, each logged to `GameOptimizer.log`:

- **Settings.** If `%LOCALAPPDATA%\GameOptimizer\config.ini` does not exist and
  `%LOCALAPPDATA%\CoreDirector\config.ini` does, `config.ini` and `applied.journal` are
  **copied** across. The old folder is **not** deleted — roll back by running the old build.
- **Start with Windows.** The old `HKCU\…\Run` value named `CoreDirector` pointed at an exe
  that no longer exists, so Windows would fail it silently at every login and leave the value
  behind. It is deleted, and a `GameOptimizer` value pointing at the *current* exe is written
  in its place — but **only if the old value was actually there**, so autostart is never
  switched on for someone who had turned it off.

## Windows Game Mode

The first-run wizard checks Game Mode and reports what it finds. **It never changes the setting
and never requires a particular value.**

Worth understanding: Game Mode does not park CCDs itself. On X3D parts, AMD's *3D V-Cache
Performance Optimizer* driver watches Xbox Game Bar's "this process is a game" signal and parks
the non-cache CCD. That is a different mechanism making the same kind of decision as
Game Optimizer, on Windows' idea of what counts as a game rather than yours. Running both means
two systems placing your game by different rules.

Separately, "AMD/ASUS Turbo Game Mode" and Gigabyte's "X3D Turbo Mode" are **BIOS** features
unrelated to Windows Game Mode; they hard-disable SMT and the second CCD at boot.

## What has actually been tested

Being specific here rather than implying more coverage than exists.

**Measured**, on one machine — AMD Ryzen 9 9950X3D, Windows 11 Home build 26200:

- Topology detection, mask derivation, and the CPU Set Id / logical-processor mapping.
- That child processes do **not** inherit their parent's default CPU Sets.
- That on this build CPU Sets behave as a **hard** restriction, not a soft hint: 8 busy
  threads on a one-processor set stayed 100% on that processor with no spillover.
- That a mask made entirely of **parked** processors is **silently ignored** — the setter
  returns TRUE, the getter echoes every id back, and the threads run elsewhere at full speed.
- That an invalid CPU Set Id returns error **813** (`ERROR_CPU_SET_INVALID`) and leaves the
  previous assignment intact.
- End-to-end, via `build\gateb_probe.exe`: a masked process runs **only** on the assigned
  processors; a **grandchild spawned after the parent was masked** is confined too when the
  engine is running, and runs on all 32 logical processors when it is not — which is the
  product's whole reason to exist, demonstrated rather than asserted.
- **That Game Optimizer is not the only program on this machine writing default CPU Sets.**
  50 processes were found carrying an assignment Game Optimizer had not made (no config on disk,
  empty journal, nothing in the log); cleared, they came back within 75 seconds with nothing of
  ours running. Something else — plausibly the AMD 3D V-Cache stack, **not proven** — assigns
  processes to the second CCD. Since this API is last-writer-wins with no ownership and no
  notification, that program and Game Optimizer can silently overwrite each other. Game Optimizer
  only ever clears masks it applied itself, and has no "clear everything" action, precisely
  because clearing is indiscriminate.

**Not tested by anyone, anywhere in this work:**

- **Any anti-cheat.** No game was running during any measurement. Game Optimizer requests the
  minimum access rights (`PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION`),
  never reads or writes game memory, and never injects anything — but that is a design
  intention, not a vendor assurance. BattlEye's FAQ notes they may **kick** (not ban) players
  for using third-party programs; that outcome cannot be prevented or detected by this app.
  Use it on anti-cheat-protected titles at your own risk.
- Intel hybrid, symmetric dual-CCD, single-CCD, and machines with more than 64 logical
  processors. Those code paths exist and are covered by synthetic unit tests only.
- Laptops, and any behaviour on battery power.

If a mask cannot be applied to a process, the app says so by name rather than skipping quietly.
If a mask is applied but ignored by the scheduler, no API reports that — which is why the core
map shows live parked state and Settings offers *Verify placement*.

## Documentation

- [Product spec](docs/spec/01-product-spec.md) — who it is for, profiles, first run, non-goals
- [Architecture](docs/spec/02-architecture.md) — topology, watcher, inheritance, config schema
- [Risks](docs/spec/03-risks.md) — anti-cheat, elevation, Game Mode, wrong-CCD detection
- [Measurements](docs/spec/04-measurements.md) — the evidence, including two claims it overturned

## Credits and licence

Game Optimizer is MIT licensed — see `LICENSE`.

The idea comes from [CPUSetSetter](https://github.com/SimonvBez/CPUSetSetter) by SimonvBez,
which is also MIT licensed. Game Optimizer is a clean reimplementation in C++ rather than a fork,
and shares no code with it; see `NOTICE.md`.
