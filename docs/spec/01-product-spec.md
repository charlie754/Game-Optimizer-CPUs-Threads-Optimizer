# Game Optimizer — Product Spec

**Date:** 2026-08-28
**Status:** v1 design, approved for implementation
**Product name:** Game Optimizer — *CPU threads optimizer*
**Renamed:** from the working name "CoreDirector" on 2026-08-28. Display text reads
`Game Optimizer`; identifiers that are not display text — the exe, the config folder, the
log file, the window classes, the mutex and the HKCU Run value — read `GameOptimizer`.

Provenance tags used throughout: **[M]** measured on this machine this turn · **[S]** sourced, citation given · **[A]** assumed, *not checked by anyone*.

---

## 1. The problem, stated precisely

On a dual-CCD AMD part (7950X3D, 7900X3D, 9950X3D) one CCD carries 3D V-Cache and the
other clocks higher. Several games gain frame-time stability when they run on the cache
CCD **and nothing else does**, and gain more when SMT siblings are kept clear so each
game thread owns a whole physical core.

Every existing way to get that is session-wide:

| Method | What it costs you |
|---|---|
| BIOS "Turbo Game Mode" | Parks a whole CCD and disables SMT for the entire boot. Discord, OBS, browsers and compilers all lose half the machine. Changing it needs a reboot. |
| Windows Game Mode + the AMD V-Cache driver | Game Mode does not park anything itself; on X3D parts AMD's 3D V-Cache Performance Optimizer driver watches **Xbox Game Bar's "this is a game" signal** and parks the non-cache CCD **[S]**. It works on Windows' idea of what a game is, with no per-title control. **[M]** On the reference machine that driver (v1.0.0.12, service `amd3dvcacheSvc`, Running) coincided with **all 16 logical processors of CCD1 flagged `Parked=1`** while CCD0 showed `Parked=0`. |
| Task Manager affinity | Uses `SetProcessAffinityMask`, a hard restriction that does not survive a relaunch and does not reach child processes. |

**What is actually wanted:** the isolation applies *only while the chosen game runs*, the
other CCD stays fully available to everything else, and the machine returns to normal the
moment the game exits. Nothing global, nothing needing a reboot.

## 2. Who this is for

1. **Primary — a dual-CCD AMD desktop owner who also works on the machine.** Runs Discord,
   OBS, a browser with forty tabs, an IDE or a coding agent, and wants one competitive
   title to get a clean CCD without surrendering the rest of the box.
2. **Secondary — an Intel hybrid owner, 12th gen and later.** Same shape: pin the game to
   P-cores without SMT, push background load to E-cores.
3. **Tertiary — a single-CCD owner.** Gets the SMT-reduction masks only. The app says so
   plainly instead of inventing a split that is not there.

Explicitly not aimed at: per-thread micro-placement, servers, or laptops on battery
(untested, not blocked).

## 3. Non-goals for v1

- GPU affinity, NUMA placement, encoder/NVENC placement. Stated out of scope up front.
- Priority classes, I/O priority, power plans. CPU Sets only.
- Any call to `SetProcessAffinityMask`. Never called — a hard product rule, not a
  preference. Rationale in [architecture §4](02-architecture.md).
- Overlays, hooks, injected DLLs, kernel drivers. Nothing touches a game's address space.
  **[A]** this is the main reason to expect anti-cheat indifference — a design intent, not
  a vendor assurance, and tracked as such in [risks §2](03-risks.md).
- Telemetry, accounts, cloud sync, auto-update. Local files only. An anonymous crash log
  may be added later, off by default, opt-in only.
- Requiring Windows Game Mode to be either on or off. The app advises; it never enforces
  and never refuses to run.

## 4. Core behaviour

### 4.1 Named masks, derived from real topology

On first run the app enumerates CPU sets and cache relationships and derives named masks.
On the reference machine **[M]** (Ryzen 9 9950X3D, 16C/32T, one processor group):

| Mask | Logical processors | CPU Set Ids | Derived from |
|---|---|---|---|
| `Cache` | 0–15 | 256–271 | `LastLevelCacheIndex` 0, L3 = 98304 KB |
| `Cache no SMT` | 0,2,4,…,14 | 256,258,…,270 | one LP per `CoreIndex` |
| `Freq` | 16–31 | 272–287 | `LastLevelCacheIndex` 16, L3 = 32768 KB |
| `Freq no SMT` | 16,18,…,30 | 272,274,…,286 | one LP per `CoreIndex` |
| `All`, `All no SMT` | — | — | always offered |

The naming is derived, never hardcoded: the last-level-cache domain with the **largest L3
becomes `Cache`**. Full decision table — including Intel hybrid, and the symmetric-CCD case
where no cache-versus-frequency distinction exists and the masks are named `CCD0`/`CCD1`
instead — in [architecture §2](02-architecture.md).

**The user sees the core map before anything is applied, and can edit any mask by clicking
logical processors.** Detection being wrong is a designed-for case, not an error case.

### 4.2 Profiles

A profile is:

```
name          Overwatch
game          Overwatch.exe        (basename, or a full path to disambiguate)
game_mask     Cache
heavy         claude.exe, node.exe, obs64.exe, firefox.exe, ...
heavy_mask    Freq
auto_pin      off | on + threshold % + dwell seconds
```

- The **game and every descendant it spawns** get `game_mask`.
- Every running process whose name is in **heavy** gets `heavy_mask`, for as long as the
  game runs.
- When no profile's game is running, **every mask the app set is cleared.** The app never
  parks a CCD, never sets a system-wide policy, and leaves no residue.

New installs start empty. One example profile (Overwatch) ships present but inactive, as a
worked example to adopt or delete.

### 4.3 Child processes are the whole point

**[M] Measured directly, not inferred.** On this machine a process set its own default CPU
sets to 2 ids and confirmed the readback returned `count=2`; it then spawned `cmd.exe` and
queried the child, which returned `count=0`. **Children do not inherit their parent's default
CPU Sets** ([measurements §4](04-measurements.md)).

Nor is there a mechanism to make them. `JOBOBJECTINFOCLASS` contains no CPU-set class — the
only CPU-related member is `JobObjectCpuRateControlInformation`, which is rate control — and
`PROC_THREAD_ATTRIBUTE_*` contains no CPU-set attribute; verified in the headers of SDK
10.0.19041.0 **and** 10.0.26100.0 **[M]**. An adversarial pass then went looking specifically
for a route the first search might have missed — the reverse-engineered job class space, the
native `PS_ATTRIBUTE` creation list, and three undocumented CPU-related process info classes —
and **found no working inheritance route** on build 26200.

So a launcher-spawned worker does not inherit, and a parent-only implementation reproduces
exactly the reported failure: an Electron or Node helper spawned two minutes in lands on the
game CCD and hitches frame times until it exits.

Game Optimizer tracks the **transitive descendant set** of the game and re-applies on a
250 ms cadence, so a new child is corrected within a quarter second of existing. Mechanism
and the PID-reuse guard: [architecture §5](02-architecture.md).

### 4.4 The optional foreground CPU rule

Off by default. When enabled: while the **game owns the foreground window**, any process
sustaining more than *N%* CPU for *M* seconds is moved to the background mask.

- Defaults when enabled: N = 8%, M = 5 s.
- Never applies to the game or its descendants.
- Never applies to anything on the exclusion list, which ships pre-populated with
  anti-cheat services, game launchers, GPU vendor containers, `audiodg.exe` and core
  Windows processes, and is fully user-editable.
- A pinned process stays pinned until the game exits. It does not flap.

This covers the case a user cannot enumerate in advance: a build kicking off, a browser tab
going rogue, an update service waking up mid-match.

### 4.5 What happens when the game exits

Every mask the app applied is cleared, on every process, immediately. A small restore
journal on disk means a crash or a forced kill of Game Optimizer itself does not strand a
background app on half the machine: the next launch clears anything the journal lists that
is still alive and whose process creation time still matches.

## 5. First run

Three pages. Skippable, never blocking.

1. **"Here is what we found."** Core map, derived masks, detection confidence. *Looks right*
   / *Let me edit*. On a machine with no split topology this page says so outright and
   explains that only the SMT masks will be useful.
2. **"About Windows Game Mode."** Reads `HKCU\Software\Microsoft\GameBar\AutoGameModeEnabled`
   **[M]** — the key exists and reads `0` on the reference machine.
   - On if the value is 1 **and** the CPU is AMD multi-CCD: explain that Game Mode applies
     its own global CCD preference, that this fights per-game CPU Sets, and offer a button
     that opens the Windows setting.
   - Otherwise: one confirming line, no action requested.
   - **The app never changes the setting itself.**
   - **[M]** The reference machine also has the AMD 3D V-Cache Performance Optimizer driver
     installed with its service running. This page reports that separately, because it is a
     second, independent global scheduling influence.
3. **"Pick your first game."** Running processes with icons and window titles, plus *Browse
   for .exe*, plus *Use the Overwatch example*. Skippable — the tray works with zero
   profiles and simply reads Idle.

## 6. UX

Quiet by default. No toasts unless the user turns them on.

**Tray tooltip is the whole status UI:**
- Idle — `Game Optimizer - idle`
- Active — `Overwatch: Cache no SMT · 4 apps on Freq`
- Degraded — `Overwatch: Cache no SMT · 2 of 6 apps blocked (access denied)`

**Tray menu:** status line (disabled) · Settings… · Pause/Resume · Start with Windows (✓) ·
Open config folder · Exit. Double-click opens Settings.

**Settings** is one resizable window: profiles list · selected-profile editor with a *Pick
from running processes* dialog · the auto-pin rule · the interactive core map · general
options. Everything is also plain text in `%LOCALAPPDATA%\GameOptimizer\config.ini` for
people who would rather edit a file.

**No elevation is required and the app does not ask for it.** Processes it cannot touch are
reported honestly in the tooltip and in Settings rather than silently skipped. An opt-in
"run elevated" switch exists for people who need to reach elevated launchers.

## 7. Distribution

A single native `GameOptimizer.exe`, x64, with no runtime dependency — deliberately unlike
the reference project, whose README lists the .NET Desktop Runtime 10 as a requirement
**[S]**. Config lives in `%LOCALAPPDATA%`, so the exe can sit anywhere, including a USB
stick.

## 8. Relationship to CPUSetSetter

The idea comes from **SimonvBez/CPUSetSetter**, which is **MIT licensed** **[M]** — fetched
from `raw.githubusercontent.com/SimonvBez/CPUSetSetter/master/LICENSE`, reading
`MIT License` / `Copyright (c) 2025 Simon`. MIT permits a fork, a derivative and closed
redistribution, provided the copyright notice and permission notice travel with any
substantial portion of the original.

**Decision: clean reimplementation in C++, not a fork.** In order:

1. The feature set diverges — profiles, heavy-app lists, a foreground CPU rule and
   descendant tracking are not the reference project's model.
2. Dropping the .NET runtime dependency is a real distribution win and is unreachable
   while forking a C# codebase.
3. Nothing needs to be copied. The shared substrate is the Windows CPU Sets API, which is
   Microsoft's.

`NOTICE.md` credits the project and reproduces its MIT notice. That is courtesy while no
code is reused. **If any code is later lifted, reproducing the notice becomes mandatory** —
recorded here so a future contributor cannot get it wrong.

---

Next: [Architecture](02-architecture.md) · [Risks](03-risks.md) · [Measurements](04-measurements.md)
