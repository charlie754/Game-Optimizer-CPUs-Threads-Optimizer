# Game Optimizer

**CPU threads optimizer.**

![Game Optimizer in use](video/gameoptimizer.gif)

Per-game CPU Set isolation for split-topology CPUs — the isolation of BIOS "Turbo Game Mode",
applied only to the game you chose, and only while it is running.

Please turn off
- Turbo Game Mode, optimize CCD Parking services in BIOS.
- Game Mode and AMD's 3D V-Cache optimizer in OS.

On a dual-CCD AMD part (7950X3D, 7900X3D, 9950X3D) one CCD carries 3D V-Cache and the other
clocks higher. Some games run better pinned to the cache CCD with nothing else on it. The
existing ways to arrange that — a BIOS game mode, or letting Windows and AMD's driver decide —
are session-wide: they cost you half the machine for Discord, OBS, browsers and compilers too.

Game Optimizer applies the isolation to one process tree, keeps the other CCD free for everything
else, and clears every mask the moment the game exits.

- **CPU Sets only.** `SetProcessDefaultCpuSets`, never `SetProcessAffinityMask`.
- **Child processes included.** A worker spawned twenty minutes into a session is picked up
  within one poll period. This is not optional — CPU sets are *not* inherited by children.
- **Nothing global by default.** No CCD parking, no system policy, no driver, no reboot. The one
  exception is opt-in: the AMD 3D V-Cache setting described below, which changes a driver's start
  type and needs a restart.
- **No injection, no overlay.** One native exe, no .NET runtime. Nothing needs elevation except
  the optional AMD 3D V-Cache setting, which asks for it once.
- **Local config only.** No account, no telemetry, and the app itself makes no network
  requests.
- **One optional runtime dependency: WebView2.** It renders the sponsor strip at the
  bottom of the Settings window and nothing else. If it is absent the strip is drawn
  with GDI instead and nothing else changes — the app always starts. It is never
  touched while the app sits in the tray. See `NOTICE.md` and the file table below.

**Runs on:** Windows 10 or 11, x64. The manifest declares no older Windows, and there is no
32-bit or ARM build.

## Which CPUs this helps

The app works this out for itself. On first run it asks Windows for the machine's core and cache
layout, classifies it, and tells you on screen which of the cases below you are in, along with a
confidence level and a map of which logical processors are in each group. There is no list of CPU
models anywhere in the app — the decision comes from what Windows reports, so nothing here goes
stale. Run it and read the first-run screen; that answer is authoritative for your machine and this
table is only a guide. The first matching row wins, so a machine reporting more than one core type
is classified by core type and never by cache size.

| What Windows reports | Shown as | Masks you get | How much it helps |
|---|---|---|---|
| More than one distinct efficiency class | Intel hybrid (P/E cores) | `P-cores`, `E-cores`, `All`, plus `no SMT` variants where they differ | A real split. Game on P-cores, background apps on E-cores. High confidence |
| Two or more last-level-cache domains, sizes differ | AMD asymmetric cache (X3D) | `Cache`, `Freq`, `Freq 2`, …, `All` | A real split. Game on the largest-L3 domain, background apps on the rest. High confidence |
| Two or more last-level-cache domains, same size | Multi-CCD symmetric | `CCD0`, `CCD1`, …, `All` | A real split, but which domain gets called `CCD0` is an ordering choice, not a measurement. Medium confidence — check the core map |
| Exactly one last-level-cache domain | Single cache domain | `All`, and `All no SMT` if the CPU has SMT | Very little. There is no second group to move background work onto |
| No cache domains reported at all | Unknown | the same as a single domain | Very little, and the first-run screen says so |

Most CPUs are a single cache domain, and on those this app can only give the game one thread per
physical core. That is the whole of it — no isolated cores, no background separation. If the CPU has
no SMT either, `All no SMT` is not offered at all, both defaults fall back to `All` — every
processor — and the app changes nothing on that machine.

The multi-domain cases are the ones worth installing for: parts such as AMD's dual-chiplet X3D
desktop processors, where 3D V-Cache sits on one chiplet only, and Intel's hybrid parts with both
performance and efficient cores. Those are examples rather than a specification — a single-chiplet
X3D part is one cache domain, some chips in Intel's hybrid generations ship with no efficient cores
at all, and a dual-chiplet part with V-Cache on both chiplets reports two equal domains and lands in
the symmetric row instead. The group names borrow vendor vocabulary but the tests do not: no branch
anywhere checks who made the CPU, so any processor Windows reports with two efficiency classes is
shown under the Intel name whoever built it.

If Windows cannot describe the topology at all, the app says so at startup and governs nothing —
that is a failure to detect, not the Unknown row above.

Two things are untested rather than unsupported. Machines with more than 64 logical processors,
which Windows splits across several processor groups, have never been run: the masks are CPU Set
Ids, which are not scoped to a processor group, so this is expected to work, but expected is not
measured. Windows on Arm has never been run either — there is no Arm64 build, and while Windows 11
on Arm emulates x64 applications, nobody has checked what topology this app sees under emulation.

### Example CPUs by class

These are examples, not a compatibility list — the app reads your machine's topology at first run, and that screen is the authoritative answer for your CPU.

**Intel hybrid (P/E cores)**
- Core Ultra 9 285K, Ultra 7 265K, Ultra 5 245K — Arrow Lake-S
- Core i9-14900K, i7-14700K, i5-14600K, i5-14400F — Raptor Lake Refresh
- Core i9-13900K, i7-13700K, i5-13600K — Raptor Lake
- Core i9-12900K, i7-12700K, i5-12600K — Alder Lake

🔴 **Trap — a hybrid generation is not a hybrid CPU.** Plenty of 12th–14th gen parts ship with zero E-cores and land in the single cache domain row instead: i5-12400, i3-12100, i3-12300, i3-13100, i3-14100, Pentium Gold G7400, Celeron G6900. Generation name tells you nothing.

**AMD asymmetric cache (X3D)**
- Ryzen 9 9950X3D — measured directly on a 9950X3D: 96 MB of L3 on one chiplet, 32 MB on the other
- Ryzen 9 9900X3D
- Ryzen 9 7950X3D, 7900X3D

Two chiplets, 3D V-Cache stacked on one of them. This is the layout the app exists for.

🔴 **Trap — most X3D parts are not in this row.** Single-chiplet X3D CPUs have one cache domain and nothing to steer between: 9800X3D, 9850X3D, 7800X3D, 5800X3D, 5700X3D, 5600X3D. The X3D name says cache, not asymmetry.

🔴 **Trap — the 9950X3D2 Dual Edition is not here either.** V-Cache sits under both chiplets, 96 MB each, so it is Multi-CCD symmetric.

**Multi-CCD symmetric**
- Ryzen 9 9950X, 9900X, PRO 9965, PRO 9955, PRO 9945
- Ryzen 9 9950X3D2 Dual Edition — 96 MB per chiplet, equal
- Ryzen 9 7950X, 7900X, 7900, PRO 7945
- Ryzen 9 5950X, 5900X, 5900XT, 5900, PRO 5945
- Ryzen 9 3950X, 3900X and Ryzen 7 3700X, 2700X — see the note below

Zen 2 and earlier split each die into two four-core complexes with their own L3, so a cache domain there is four cores, not eight. The 3700X has one chiplet and two domains; the 2700X has no chiplets at all and still has two. Confining a game to one of those domains gives it four cores.

**Single cache domain**
- Ryzen 7 9700X, 9700F and Ryzen 5 9600X, 9600, 9500F
- Ryzen 7 7700X, 7700 and Ryzen 5 7600X, 7600, 7500F
- Ryzen 7 5800X, 5800XT, 5700X and Ryzen 5 5600X, 5600
- Ryzen 7 8700G, Ryzen 5 8600G — monolithic APUs
- Single-chiplet X3D parts: 9800X3D, 9850X3D, 7800X3D, 5800X3D, 5700X3D, 5600X3D
- Intel parts without E-cores: i5-12400, i3-12100, i3-13100, i3-14100, i9-9900K, i7-9700K, i5-9600K, i7-2600K

One cache domain means there is nothing to steer between. The app will say so rather than pretend otherwise.

## Status

**Pre-release.** Read [what has actually been tested](#what-has-actually-been-tested) before
using this on anything you care about.

## Anti-cheat has not been tested

**No anti-cheat has been tested with this app by anyone, anywhere in this work. No game was
running during any measurement.**

Game Optimizer requests the minimum access rights
(`PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION`), never reads or writes
game memory, and never injects anything — but that is a design intention, not a vendor
assurance. BattlEye's FAQ notes they may **kick** (not ban) players for using third-party
programs; that outcome cannot be prevented or detected by this app.

Use it on anti-cheat-protected titles at your own risk.

## What is in this download

Eight files. There is no installer, and the app installs no service and no driver. Nothing is
written outside your user profile unless you turn on the AMD 3D V-Cache setting, which writes one
registry value for AMD's own driver and service.

| File | What it is |
|---|---|
| `GameOptimizer.exe` | **The app. Double-click this one.** |
| `WebView2Loader.dll` | Microsoft's WebView2 loader. **Keep it in the same folder as the exe** — that is the first place the app looks for it. It is optional: without it the Settings window draws its sponsor strip with GDI instead and nothing else changes. |
| `README.md` | This file. |
| `LICENSE` | MIT, for Game Optimizer itself. |
| `NOTICE.md` | What is bundled, where it came from, and what is not third-party. |
| `third_party\webview2\LICENSE.txt` | Microsoft's licence for the loader above. |
| `third_party\webview2\NOTICE.txt` | Microsoft's notices for the same. |
| `third_party\webview2\README.md` | Which SDK package version that loader came from. |

The `third_party\webview2\` folder is **documentation only** — nothing in it is loaded at run
time. To remove Game Optimizer: **if you turned on the AMD 3D V-Cache setting, turn it off first and
restart** — that setting disables a driver, and the record of its original value lives in
`config.ini`, so deleting that file first leaves the driver disabled with nothing left to restore
it. Then Exit from the tray icon, delete the folder you unzipped, and delete
`%LOCALAPPDATA%\GameOptimizer\` if you want its settings gone too.

**If Windows says the file is blocked**, that is the mark-of-the-web that lands on anything
downloaded. Right-click the **zip** before extracting, Properties, tick **Unblock**, OK, then
extract.

**Double-clicking it never asks for administrator rights.** The manifest requests `asInvoker`, so
launching it raises no UAC prompt. One optional feature does: turning the AMD 3D V-Cache setting on
or off launches a short-lived elevated copy of this same exe to write a single registry value, and
Windows will ask you to approve it. Because the exe is unsigned, that prompt says "Unknown
publisher" for the same reason SmartScreen does.

## Build from source

None of this is in the download — it is for the repository at
<https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer>.

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

## Windows SmartScreen on first run

The binary is not code-signed yet. Signing through SignPath Foundation, which is free for
open-source projects, has been chosen and is being set up; at the time of writing it is not in
place, so there is no signature — and Windows notices.

The first time you run a downloaded `GameOptimizer.exe`, SmartScreen shows a prompt headed
**"Windows protected your PC"** saying the app is unrecognised. **Run anyway** is not on it
until you click **More info**:

1. **More info**
2. **Run anyway**

That is once, for that copy of the file. Nothing here asks you to turn SmartScreen off, add a
Defender exclusion, or change any other security setting; none of that is needed to run this.

"Unrecognised" is exactly what it says: no signature and no download reputation. It is not a
statement about what is in the file. What this project offers instead of a signature is that
[the whole source is public](https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer)
and builds with MSVC via `tools\build.bat` in that repository (not in this zip), so you can read
what it does and produce the binary yourself. A copy you compiled locally does not normally carry
the downloaded-file marker this check is keyed to, so the prompt is a thing downloaders meet
rather than builders.

## If Windows Defender flags it

Some machines have shown a Defender toast for a Game Optimizer build - "Trojan:Win32 Cloxer" on an
old development build, and "Trojan:Script Wacatac.H!ml" on a user's copy of a release. The "!ml"
suffix marks a machine-learning verdict: no analyst looked at the file. The exe is not signed, it
is new, few machines have seen it, and it does things a scanner scores as suspicious for a good
reason - it enumerates processes, opens the game's process to set its CPU Sets, controls one
AMD service on request, writes a Start-with-Windows entry when you ask for one, and relaunches a
short-lived elevated copy of itself for the one action that needs administrator rights. None of
that is hidden; all of it is in this repository.

What to check before trusting any copy:

1. Recent releases print two SHA256 hashes in their GitHub release notes, one for the zip and
   one for the loose exe. Compare the published zip hash with
   `certutil -hashfile GameOptimizer-vX.Y.Z-x64.zip SHA256` on the file you downloaded. If you
   already unpacked it and only have the loose exe, hash that instead with
   `certutil -hashfile GameOptimizer.exe SHA256`. Older releases do not all list a hash; if the
   release you downloaded does not, you cannot use this check on it.
2. If the hash matches and Defender still quarantines the exe, that is a false positive on this
   build. You can report it to Microsoft at https://www.microsoft.com/en-us/wdsi/filesubmission
   (choose "incorrectly detected", and set the submission priority to **Medium** - the form
   says Medium gets an analyst review within a few days, while Low "may never be processed").
   Expect a few days, and note that a review is not a promise: it may or may not clear the
   detection, for you or for anyone else. Please also open an issue here with the detection
   name and the date.
3. Or build it yourself: `tools\build.bat` with the Visual Studio Build Tools gives you a
   binary you compiled from these sources. It will **not** be byte-identical to the released
   exe - the build is not reproducible bit for bit, so its SHA256 will differ and that is
   expected. `tools\build.bat` currently expects the Build Tools at one hard-coded path and
   stops if it is not there, so you may need to point it at your own `vcvars64.bat`.

Defender's clean-up also deletes the Start-with-Windows entry it finds. After restoring or
reinstalling, switch "Start with Windows" back on in Settings.

Nothing here asks you to add an exclusion or turn Defender off.

## Use

Double-click `GameOptimizer.exe`. The Settings window opens and the app gets a taskbar button,
plus an icon in the notification area.

**Closing the Settings window does not quit the app.** It returns to the tray and keeps working.
The tray icon's **Settings** item brings the window back; **Exit** is what actually quits, and
quitting clears every mask it applied. If you turn on **Start with Windows**, the app starts
quietly at login with no window.

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

**If it is killed rather than closed** — Task Manager, a crash, a power cut — the assignments it
had applied are recorded in `%LOCALAPPDATA%\GameOptimizer\applied.journal`, and the next start
reads that journal and tries to clear each one, writing the result of every attempt to
`%LOCALAPPDATA%\GameOptimizer\GameOptimizer.log`. An entry it cannot clear is **logged as
FAILED and not retried later**, so the log is the place to check rather than an assumption.
Rebooting is the guaranteed reset: a CPU Set assignment belongs to a running process and does
not survive one.

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
Performance Optimizer* makes the same kind of decision Game Optimizer does, by a different
mechanism and on Windows' idea of what counts as a game rather than yours. Running both means two
systems placing your game by different rules.

That optimizer is **three** components, and only one of them does anything:

| component | what it is | what it does |
|---|---|---|
| `amd3dvcacheSvc` | a Windows service, visible in `services.msc` | launches the agent below, and nothing else |
| `amd3dvcacheUser.exe` | a per-session background process, **not** a service | watches which window has focus and decides which CCD to prefer |
| `amd3dvcache` | a kernel driver, **not** shown in `services.msc` | passes that one preference down to firmware |

So the service being stopped does not mean the optimizer is off, and the driver running does not
mean it is on. The process that matters is `amd3dvcacheUser.exe`, and that is the one Game
Optimizer's startup warning checks.

The kernel driver is loaded by Windows PnP against a firmware-declared device, so its start type
reading "Manual" does **not** mean it stays unloaded — it loads every boot. Only setting it to
Disabled stops that, which is what the optional AMD 3D V-Cache setting does, and why it needs a
restart.

Separately, "AMD/ASUS Turbo Game Mode" and Gigabyte's "X3D Turbo Mode" are **BIOS** features
unrelated to Windows Game Mode; they hard-disable SMT and the second CCD at boot.

Some boards also offer an **adaptive** version of this, working differently — a game-aware CCD parking
option that parks the second CCD while a game runs, rather than disabling it at boot. That
one is easy to miss, because it looks exactly like the Windows-side optimizer doing it: the
AMD service can be stopped, Windows core parking can be off, and a CCD still parks. Names
vary by vendor, and no reliable menu location can be given here. Look for a *game-aware* or
*adaptive* CCD parking option — and note that this is **not** the boot-time *CCD Control* or
*SMT Control* setting described above, which disables a CCD outright. **While firmware is
parking a CCD, no application can make it usable** — Game Optimizer's background mask included.
If your background apps are assigned to a mask that parks whenever a game is running, check the
BIOS before assuming the app is at fault.

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
- End-to-end, via `build\gateb_probe.exe` (a probe built from the repository, not shipped
  here): a masked process runs **only** on the assigned
  processors; a **grandchild spawned after the parent was masked** is confined too when the
  engine is running, and runs on all 32 logical processors when it is not — which is the
  product's whole reason to exist, demonstrated rather than asserted.
- **That Game Optimizer is not the only program on this machine writing default CPU Sets.**
  50 processes were found carrying an assignment Game Optimizer had not made (no config on disk,
  empty journal, nothing in the log); cleared, they came back within 75 seconds with nothing of
  ours running. Something else assigns processes to the second CCD.

  An earlier version of this note named the AMD 3D V-Cache stack as the likely culprit, while
  saying it was not proven. **That suspect has since been examined and cleared.** None of the three
  V-Cache binaries — the service, the per-session agent, or the kernel driver — contains the name
  of `SetProcessDefaultCpuSets`, `SetThreadSelectedCpuSets` or `SetProcessAffinityMask`, either as
  an imported function or as a string that could be resolved at run time; a positive control
  confirms the search does find other API names in the same files. The V-Cache agent's only
  outbound call is a single device request that ends at firmware, not at the Windows scheduler.
  **What writes those masks is once again unknown**, and naming a new suspect without evidence is
  how the last one got there. Since this API is last-writer-wins with no ownership and no
  notification, that program and Game Optimizer can silently overwrite each other. Game Optimizer
  only ever clears masks it applied itself, and has no "clear everything" action, precisely
  because clearing is indiscriminate.

**Not tested by anyone, anywhere in this work:**

- **Any anti-cheat.** No game was running during any measurement. The full warning is its own
  section above: [Anti-cheat has not been tested](#anti-cheat-has-not-been-tested).
- Intel hybrid, symmetric dual-CCD, single-CCD, and machines with more than 64 logical
  processors. Those code paths exist and are covered by synthetic unit tests only.
- Laptops, and any behaviour on battery power.

If a mask cannot be applied to a process, the app says so by name rather than skipping quietly.
If a mask is applied but ignored by the scheduler, no API reports that — which is why the core
map shows live parked state and Settings has an **Inspect processes** button on its General
page. That report gives, per governed process, the mask that was assigned, what Windows
reports back, how many of the mask's processors are parked, and whether something else has set
a restrictive affinity mask. It cannot show where a process is actually running, and says so.

## Documentation

- [Product spec](https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer/blob/main/docs/spec/01-product-spec.md) — who it is for, profiles, first run, non-goals
- [Architecture](https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer/blob/main/docs/spec/02-architecture.md) — topology, watcher, inheritance, config schema
- [Risks](https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer/blob/main/docs/spec/03-risks.md) — anti-cheat, elevation, Game Mode, wrong-CCD detection
- [Measurements](https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer/blob/main/docs/spec/04-measurements.md) — the evidence, including two claims it overturned

Source, issues and releases: <https://github.com/charlie754/Game-Optimizer-CPUs-Threads-Optimizer>

## Credits and licence

Game Optimizer is MIT licensed — see `LICENSE`.

The idea comes from [CPUSetSetter](https://github.com/SimonvBez/CPUSetSetter) by SimonvBez,
which is also MIT licensed. Game Optimizer is a clean reimplementation in C++ rather than a fork,
and shares no code with it; see `NOTICE.md`.
