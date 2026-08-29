# Game Optimizer — Architecture

**Date:** 2026-08-28
**Target:** Windows 10 1607+ / Windows 11, x64
**Language:** C++17, Win32, MSVC. No third-party dependencies, no runtime redistributable.

Provenance tags: **[M]** measured this turn · **[S]** sourced · **[A]** assumed, not checked.

---

## 1. Why C++ and not C# / .NET

This box has `Microsoft.WindowsDesktop.App` **8.0.22** and **10.0.11** runtimes but **no
.NET SDK at all** **[M]** — `dotnet --list-sdks` prints nothing and
`C:\Program Files\dotnet\sdk\` does not exist, while `dotnet --list-runtimes` succeeds. It
does have **MSVC 2019 Build Tools** with Windows SDK 10.0.19041.0 on `D:` **[M]** — a
probe was compiled and run with it this turn.

So C++ is the stack that can be built and verified today with zero installs, and it
independently produces the better artifact: one x64 exe, no runtime prompt for the end
user, ~200 KB, and a smaller surface for anti-cheat heuristics than a managed process with
a JIT.

## 2. Topology detection

### 2.1 Inputs

Two API calls, both unprivileged:

```c
BOOL GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP, ..., PDWORD);
```

From `GetSystemCpuSetInformation`, per logical processor: `Id`, `LogicalProcessorIndex`,
`CoreIndex`, `LastLevelCacheIndex`, `NumaNodeIndex`, `Group`, `EfficiencyClass`, and the
`Parked` / `Allocated` / `RealTime` flags.

From `GetLogicalProcessorInformationEx(RelationCache)`, per cache: `Level`, `Type`,
`CacheSize`, and the `GROUP_AFFINITY` of processors sharing it.

### 2.2 Three facts measured on the reference machine that drive the algorithm

**[M]** Ryzen 9 9950X3D, from `tools\probe\topology_probe.cpp` compiled and run this turn:

1. **`EfficiencyClass` is `0` on all 32 logical processors.** On AMD dual-CCD it carries no
   information. Any detector keying on it finds nothing. Intel hybrid is the only place it
   is useful.
2. **CPU Set `Id` starts at 256, not 0.** `Id` 256 ↔ `LogicalProcessorIndex` 0. Treating
   `Id` as an LP index applies masks to the wrong cores, or fails outright. All APIs take
   `Id`; all UI shows `LogicalProcessorIndex`; the two are converted at the boundary and
   never mixed.
3. **The two CCDs are distinguished by L3 size, and only by L3 size.**
   `LastLevelCacheIndex` 0 → L3 = 98304 KB (16 LPs). `LastLevelCacheIndex` 16 → L3 =
   32768 KB (16 LPs). Same `NumaNodeIndex`, same `Group`, same `EfficiencyClass`.
   `CoreIndex` is the first LP of each physical core (0,0,2,2,4,4,…), so SMT siblings share
   a `CoreIndex`.

### 2.3 The classifier

```
1. Read all CPU sets. Read all RelationCache entries with Level == 3
   (fall back to the highest Level present if no L3 exists).
2. domain[llcIndex] = { set of LPs with that LastLevelCacheIndex, l3Bytes }
3. effClasses = distinct EfficiencyClass values over all LPs

CASE A  effClasses.size() > 1                       -> INTEL_HYBRID
        perf  = LPs with max EfficiencyClass        -> "P-cores"
        eff   = LPs with min EfficiencyClass        -> "E-cores"
        default game mask = "P-cores", background = "E-cores"
        confidence = HIGH

CASE B  domains.size() > 1 and L3 sizes differ      -> AMD_ASYMMETRIC (X3D)
        largest L3 domain  -> "Cache"
        all other domains  -> "Freq"   (or "Freq 2", "Freq 3", ... if >2)
        default game mask = "Cache", background = "Freq"
        confidence = HIGH

CASE C  domains.size() > 1 and L3 sizes equal       -> MULTI_CCD_SYMMETRIC
        name them "CCD0", "CCD1", ... by ascending lowest LP index
        default game mask = "CCD0", background = "CCD1"
        confidence = MEDIUM   (which CCD is "better" is not observable here)

CASE D  domains.size() == 1                         -> SINGLE_DOMAIN
        no split is available; offer "All" and "All no SMT" only
        confidence = NONE, and the UI says so in plain words
```

**In cases A, B and C the default game mask is the WHOLE first group, never its `no SMT`
reduction** (operator decision 2026-08-29; it was `Cache no SMT` / `P-cores no SMT` /
`CCD0 no SMT` until then). Halving the thread count of the cache domain is a tuning choice
with a real cost to any game that scales past eight cores, and a first run has measured
nothing yet. The `no SMT` masks are still derived and still one combo-box selection away. The
background default is unchanged.

**Case D is the exception and keeps `All no SMT`.** There `All` is every processor, which is
the same thing as having no assignment at all, so the shipped profile would be inert — and the
first-run wizard already tells the user that the SMT masks are the only thing that can help on
such a machine. The rule is "stop halving a domain we already isolated", not "stop doing
anything".

The chosen name is checked against the derived mask list before it is stored: a default naming
a mask that was not derived would leave the engine with nothing to apply. It falls back to
`<group> no SMT`, then `All`, then the first derived mask.

Two rules applied after every case:

- **`no SMT` reduction:** for a set of LPs, keep the lowest-indexed LP of each distinct
  `CoreIndex`. Where a topology has no SMT, this is the identity and the duplicate mask is
  not offered.
- **`All` and `All no SMT` are always present.**

`EfficiencyClass` ordering: Microsoft documents a higher value as the more performant class
**[S]** *(to be confirmed against Microsoft Learn before Intel hybrid ships — the tracked
open item in* [risks](03-risks.md) *)*. Because CASE A is the only consumer and the core map
lets a user swap the two masks in two clicks, a wrong ordering is a cosmetic label error,
not a silent misapplication.

### 2.4 Multi-group machines

Above 64 logical processors Windows uses multiple processor groups. CPU Set `Id` is unique
**across** groups, so `SetProcessDefaultCpuSets` takes cross-group ids without extra plumbing.

**It does not simply fall out for free, and an earlier draft claimed it did.** Microsoft
documents **[S]**: "On multi-group systems, CPU assignments are ignored if they are in groups
that do not match the group in the thread's affinity mask." On Windows 10 a process is
confined to a single group by default, so cross-group ids would be silently ignored — the
§6.0 failure class again. Windows 11 changed this: processes span all groups by default, and
Microsoft now recommends CPU Sets as the way to manage placement across groups **[S]**.
Windows 11 also adds `SetProcessDefaultCpuSetMasks`, a `GROUP_AFFINITY`-based variant built
for this case; it is **not** present in SDK 10.0.19041.0 **[M]** and is not used.

**Position taken:** v1 derives masks **within a single processor group** and says so in the
UI when it finds more than one. The core map renders one panel per group. **[A]** None of
this is measured — no such machine here — and it is carried as an open item in
[risks](03-risks.md) rather than claimed working.

## 3. Module layout

| File | Responsibility |
|---|---|
| `src\topology.cpp` | Enumerate CPU sets + caches, run the classifier, produce `Topology` and the derived `Mask` list. Pure: no Win32 UI, no globals. |
| `src\config.cpp` | Load/save `config.ini`. Defaults. Migration by `version=` key. |
| `src\applier.cpp` | `SetProcessDefaultCpuSets` / clear, handle open + close, error classification, the restore journal. The **only** file allowed to call a CPU Sets setter. |
| `src\procwatch.cpp` | Process snapshot, parent→child graph, descendant walk, CPU% sampling, foreground tracking. |
| `src\engine.cpp` | The decision loop: profile match → desired mask per PID → diff against applied state → call the applier. |
| `src\tray.cpp` | Shell_NotifyIcon, menu, tooltip text, runtime-drawn icons. |
| `src\settings.cpp` | Settings window, profile editor, process picker. |
| `src\coremap.cpp` | Owner-drawn core map control, shared by Settings and the first-run wizard. |
| `src\firstrun.cpp` | Three-page wizard. |
| `src\util.cpp` | Paths, string helpers, registry, single-instance mutex, DPI. |
| `src\main.cpp` | WinMain, wiring, message loop, shutdown ordering. |

`topology.cpp` and `config.cpp` are deliberately free of Win32 UI so they are unit-testable
from a console harness — see §8.

## 4. Why CPU Sets and never affinity masks

An earlier draft of this section claimed CPU Sets are "a scheduler preference with a legal
fallback". **Measurement overturned that** — see
[measurements §2.2](04-measurements.md). On this build, 8 busy threads assigned a one-LP set
with no affinity mask in play produced **100.0% of samples on that single processor, zero
spillover, and 7.6× less work done**. CPU Sets behaved as a hard restriction. Microsoft's
wording ("will typically execute on one of the processors in its list") permits either
behaviour; this build enforces it.

So the reasons for choosing CPU Sets are **not** "they are softer". They are:

1. **Lower privilege.** `SetProcessDefaultCpuSets` needs only
   `PROCESS_SET_LIMITED_INFORMATION`; `SetProcessAffinityMask` needs the stronger
   `PROCESS_SET_INFORMATION` **[S]**. On a process an anti-cheat is guarding, the weaker
   right is the one more likely to be granted.
2. **It degrades to a no-op instead of a stall.** Assigning a set of entirely *parked*
   processors let the threads run normally on what was available **[M]**
   ([measurements §2.3](04-measurements.md)). An affinity mask naming only parked processors
   has no such escape.
3. **It is not inherited**, so it cannot silently leak into unrelated descendants the way an
   affinity mask does. For this app that is a cost — it is why the watcher exists — but it
   also means the app's blast radius is exactly the processes it chose.
4. **It is per-process and reversible** with one call, leaving no residue.

Practical consequences the implementation depends on:

- Clearing is `SetProcessDefaultCpuSets(h, NULL, 0)`. The SDK annotates the pointer
  `_In_reads_opt_` **[M]**, i.e. NULL is legal, and this is the documented clear path.
- **An affinity mask set by anything else silently defeats a CPU Set.** Microsoft **[S]**:
  "If a thread or process has a restrictive affinity mask set, the affinity mask is respected
  above any conflicting CPU Set assignment." The app therefore **reads** the mask and warns;
  it never writes one.
- Per-thread `SetThreadSelectedCpuSets` overrides the process default for that thread. The
  app never sets thread-level sets, so a game managing its own thread CPU sets keeps control.
- Because the restriction is real rather than advisory, **a wrong mask genuinely starves a
  process.** That is what makes the core map and mask editing load-bearing rather than
  cosmetic.

**Hard rule for review:** `SetProcessAffinityMask` must appear in exactly zero source files.
`tools\gate-a.bat` greps for it.


## 5. Process watching and child inheritance

### 5.1 Why a watcher is required

**[M]** Neither `JOBOBJECTINFOCLASS` nor `PROC_THREAD_ATTRIBUTE_*` in Windows SDK
10.0.19041.0 exposes any CPU-set mechanism, so there is no inheritance to lean on and no
create-time attribute to set. Scoped precisely: *that SDK version, those two enumerations.*
A newer SDK adding a class would not change the design, because Windows 10 support requires
the watcher regardless.

Job objects were considered and rejected for a second reason: a running game is frequently
already inside a job created by Steam, the Epic launcher or Game Bar, and assigning it to
another job is constrained by nesting rules **[A]** — but this is moot, since even a job
would carry an affinity mask, not a CPU set, and affinity masks are banned by §4.

### 5.2 The loop

One worker thread, default 250 ms period, configurable 100–2000 ms.

```
tick:
  1. snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)
       -> for each: pid, ppid, exeName
  2. for each new pid: OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)
       -> GetProcessTimes -> creationTime;  QueryFullProcessImageName -> full path
  3. parent validation: a ppid is only honoured when
       parent is live AND parent.creationTime <= child.creationTime
       (guards PID reuse, which is common on a long-uptime desktop)
  4. activeProfile = first enabled profile whose game matches a live process
  5. gameSet   = { gamePid } + transitive descendants, minus exclusions
     heavySet  = live processes whose basename is in profile.heavy
     autoSet   = processes that tripped the CPU% rule (see 5.3)
  6. desired[pid] = gameMask for gameSet, heavyMask for heavySet + autoSet
  7. diff desired against appliedState; call the applier only on change
  8. for pids in appliedState but no longer desired (or dead): clear
```

**Cost — measured, and the first estimate here was wrong.** An earlier draft asserted **[A]**
that a tick was "sub-millisecond … well under 0.5% of one core". Running `--bench` on the built
binary and reading the log timestamps gives **~11.6 ms per tick** (40 ticks in 463 ms) on a
desktop with roughly 400 processes — about **4.6% of one core**, held continuously.

Two things were wrong, and they are worth separating:

1. **The estimate.** A `TH32CS_SNAPPROCESS` walk *is* cheap. What is not cheap is what the
   implementation then does per process: `OpenProcess` + `GetProcessTimes` +
   `QueryFullProcessImageNameW`, roughly 1200 syscalls per tick. `creationTime` and `fullPath`
   are immutable for the life of a pid and the previous snapshot already holds them, so this
   is largely avoidable work.
2. **The instrument.** `--bench` originally reported `min=0 median=15 max=16 ms`, which is not
   tick cost at all — 15.6 ms is the `GetTickCount64` timer granularity. A benchmark whose
   output is quantised to the clock's resolution measures the clock. It now uses
   `QueryPerformanceCounter` and reports microseconds together with the process count, since
   tick cost scales with process count and the number means nothing without it.

**Both were fixed, and the result was measured** — interleaved A/B/C runs 8 s apart so machine
drift cannot explain the difference, on ~300 processes:

| Build | median tick |
|---|---|
| before | **13,839 µs** |
| path cache only (skip `QueryFullProcessImageNameW`) | 10,216 µs |
| **shipped** (also skip `GetProcessTimes` when auto-pin is off) | **5,783 µs** |

So a tick is now **~5.8 ms**, about **2.3% of one core** at the 250 ms default — roughly halved,
and the new instrument independently reproduced the original ~11.6 ms figure (median 11,574 µs).

The PID-reuse guard is unaffected: a carried-forward entry is valid only when the creation time
already matches, and a pid whose creation time cannot be read is marked access-denied rather
than treated as a match.

**[A]** The remaining ~5.8 ms is not attributed. Candidates are the snapshot itself, the
fast-failing `OpenProcess` calls for the ~85 access-denied processes, the ~300 `std::wstring`
allocations, and the whole-map copy at the end of each tick. Not measured, so not claimed.

Rejected alternatives, with the reason:

| Mechanism | Rejected because |
|---|---|
| WMI `__InstanceCreationEvent` | Requires COM and a polling interval internally; historically 1–2 s latency and measurably worse CPU cost than a snapshot. |
| `Win32_ProcessStartTrace` | Requires administrator. Elevation is a product non-goal. |
| ETW kernel process provider | Requires administrator (`SeSystemProfilePrivilege`). Same reason. |
| Job object completion port | Only reports processes joining *our* job; a running game cannot reliably be moved into one, and it would not carry CPU sets anyway. |

### 5.3 The CPU% rule

Per candidate process, each tick:

```
cpuPct = (dKernel + dUser) / (dWallClock * totalLogicalProcessors) * 100
```

so 100% means the whole machine, matching Task Manager's convention. A process is pinned
when `cpuPct >= threshold` on every tick across `dwell` seconds **and** the foreground
window belongs to the game **and** it is not excluded and not in `gameSet`. Once pinned it
stays pinned until the game exits — no unpin hysteresis, because flapping a mask during a
match is worse than leaving a finished compile on the background CCD.

Foreground is tracked by `SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …, WINEVENT_OUTOFCONTEXT)`
on the UI thread, with a `GetForegroundWindow()` read each tick as a cheap backstop.

### 5.4 Exclusions

Ship-default list, applied to `gameSet` and `autoSet` alike (an explicit `heavy` entry is
the user's own instruction and is honoured):

`EasyAntiCheat.exe`, `EasyAntiCheat_EOS.exe`, `BEService.exe`, `vgc.exe`, `vgtray.exe`,
`audiodg.exe`, `dwm.exe`, `csrss.exe`, `services.exe`, `svchost.exe`, `lsass.exe`,
`wininit.exe`, `winlogon.exe`, `explorer.exe`, `MsMpEng.exe`, `NVDisplay.Container.exe`,
`nvcontainer.exe`, `AMDRSServ.exe`, `RadeonSoftware.exe`, `steam.exe`,
`EpicGamesLauncher.exe`, `Battle.net.exe`, `GameOptimizer.exe`.

PIDs 0 and 4 are never touched under any configuration, including a user-edited list.

Anti-cheat exclusion matters specifically because an anti-cheat service **is** a descendant
of the game and would otherwise be swept into `gameSet` by the descendant walk. It is low
CPU and belongs on the full machine.

## 6. Applying and clearing

```c
HANDLE h = OpenProcess(PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                       FALSE, pid);
SetProcessDefaultCpuSets(h, ids, count);   // apply
SetProcessDefaultCpuSets(h, NULL, 0);      // clear
CloseHandle(h);
```

Error classification, because "it silently did nothing" is the failure mode to avoid:

| `GetLastError()` | Meaning surfaced to the user |
|---|---|
| **`813` `ERROR_CPU_SET_INVALID`** | A stale CPU Set Id — the topology changed under us. Triggers re-detection. **[M]** measured: an invalid Id returns FALSE with 813 and **leaves the previous assignment intact**, so the rejection is atomic. |
| `ERROR_ACCESS_DENIED` | Process is elevated or protected. Counted into the "n blocked" tooltip and listed by name in Settings. |
| `ERROR_INVALID_PARAMETER` | Malformed call. Logged; should not occur in normal operation. |
| `ERROR_INVALID_HANDLE` | Process exited between snapshot and apply. Benign, dropped. |

### 6.0 The failure mode that returns success

**[M]** The most dangerous outcome is not an error at all. Assigning a set consisting entirely
of parked processors produced: setter returns **TRUE**, `GetProcessDefaultCpuSets` reads back
**all 16 requested ids**, and **none of those 16 processors ran a single sample** —
the threads ran on the other CCD at full speed ([measurements §2.3](04-measurements.md)).

Microsoft documents a second instance of the same class **[S]**: "If a process attempts to use
a CPU Set assignment which is allocated exclusively to other processes, its request is
ignored."

So there are three outcomes, not two: **applied**, **failed with an error**, and **accepted
and ignored**. The third is invisible to both the setter's return value and the getter.
Consequences carried through the design:

- The app never claims a mask is "active" on the strength of an API return. The tray says what
  it *assigned*; Settings offers a *Verify placement* action that samples where the process is
  actually running.
- The core map draws live `Parked` state, and Settings **warns when a selected mask is
  entirely parked**, because that is the configuration most likely to be silently ignored.
- Gate B verifies placement, not assignment — see §8.

**Requested access rights are the minimum that works, deliberately.**
`PROCESS_SET_LIMITED_INFORMATION` and `PROCESS_QUERY_LIMITED_INFORMATION` only. The app
never requests `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION` or
`PROCESS_ALL_ACCESS` — see [risks §2](03-risks.md).

### 6.1 The restore journal

`%LOCALAPPDATA%\GameOptimizer\applied.journal`, one line per governed process:

```
<pid>\t<creationTimeLow>\t<creationTimeHigh>\t<exeName>
```

Appended before the first apply to a PID, rewritten on clear. On startup the app reads it,
and for every entry whose PID is still live **and whose creation time still matches**, calls
the clear path, then truncates. The creation-time match is what stops a recycled PID from
having an unrelated process cleared.

This is the answer to "Game Optimizer was killed mid-match and now Firefox is stuck on four
cores."

## 7. Configuration

`%LOCALAPPDATA%\GameOptimizer\config.ini`. INI, not JSON — a hand-written JSON parser is a
correctness liability for no gain here, whereas this grammar is three rules and cannot be
ambiguous. `|` is the list separator because it is an illegal character in Windows
filenames, so no path can ever contain one.

```ini
version=1

[general]
start_with_windows=0
poll_ms=250
notifications=0
paused=0

[masks]
Cache=256,257,258,259,260,261,262,263,264,265,266,267,268,269,270,271
Cache no SMT=256,258,260,262,264,266,268,270
Freq=272,273,274,275,276,277,278,279,280,281,282,283,284,285,286,287
Freq no SMT=272,274,276,278,280,282,284,286
All=256,257,...,287
All no SMT=256,258,...,286

[topology]
signature=amd:2:16:16:98304:32768
; if the live machine's signature differs, masks are re-derived and the user is told

[exclusions]
names=EasyAntiCheat.exe|BEService.exe|vgc.exe|audiodg.exe|...

[profile:Overwatch]
enabled=1
game=Overwatch.exe
game_mask=Cache
heavy=claude.exe|node.exe|obs64.exe|NVIDIA Broadcast.exe|firefox.exe
heavy_mask=Freq
auto_pin=0
auto_pin_percent=8
auto_pin_seconds=5
```

The `[topology] signature` is a cheap guard against a config written on one machine being
silently applied on another, or against a BIOS change (SMT toggled, CCD disabled) making
every stored `Id` wrong. On mismatch: re-derive, keep profiles, tell the user once.

## 8. Testing and the release gate

`tests\` builds a console harness linking `topology.cpp` and `config.cpp` directly.

**Gate A — build and hygiene**
1. `tools\build.bat` exits 0, produces `build\GameOptimizer.exe`, and emits zero C4996 or
   C4477 warnings at `/W3`.
2. `grep -r SetProcessAffinityMask src\` returns **nothing**. The §4 rule, enforced.
3. `tests\run-tests.bat` exits 0 and prints its own total; a filtered subset is not a run.

**Gate B — product depth**, each requiring an observation, not a compile:

1. **Placement, not assignment.** A sacrificial child process is spun up, given a mask, and
   made to run busy threads while sampling `GetCurrentProcessorNumberEx`. The observed
   processors must be a **subset of the assigned mask**. Clearing must restore use of the
   full machine.
   **This item was rewritten after measurement.** It previously said to verify by reading the
   assignment back with `GetProcessDefaultCpuSets` — which returns TRUE and echoes all the
   requested ids for an assignment the scheduler ignores completely
   ([measurements §2.4](04-measurements.md)). Reading back the assignment measures the layer
   *next to* the claim, not the claim.
2. **Child inheritance.** A child spawned **after** its parent was masked is governed within
   one poll period, verified by the §8.1 placement method on the child itself.
3. **Crash recovery.** Killing Game Optimizer with masks applied, then restarting it, leaves the
   previously governed processes running on the full machine again — the journal path.
4. **No residue.** With no profile game running, no process on the system has a default CPU
   set that Game Optimizer installed.
5. **Honest degradation.** A process Game Optimizer cannot open is reported by name as blocked,
   not silently skipped.

Item 2 is what distinguishes this from a parent-only toy and item 1 is what stops the suite
grading itself; both are release blockers rather than nice-to-haves.

## 9. Threading and shutdown

- **UI thread** owns the tray icon, the settings window, the WinEvent hook and the message
  loop.
- **Watcher thread** owns snapshots, the CPU% table and the applier. It publishes an
  immutable status snapshot under a lock; the UI thread only ever reads it and posts
  `WM_APP_STATUS` to itself for redraw.
- Config writes happen on the UI thread and are handed to the watcher as a whole new
  immutable config object, swapped under the same lock. No partial-config tick is possible.
- Shutdown order is fixed: signal stop → **join the watcher** → clear every applied mask →
  truncate the journal → stop foreground tracking → save config → remove the tray icon.

  **The join comes before the clear, and an earlier draft of this document had it the other
  way round.** Clearing while the watcher thread is still alive races its own apply path: the
  watcher can re-apply a mask to a pid microseconds after the shutdown path cleared it, and
  the process is then stranded with a mask and no journal entry — the exact failure the
  journal exists to prevent, reintroduced by the cleanup. The implementation is correct; this
  document has been corrected to match it.

- `WM_QUERYENDSESSION` runs the full clear immediately rather than waiting for `WM_ENDSESSION`,
  because once `WM_ENDSESSION` has been delivered the process may be terminated at any moment.
  `WM_ENDSESSION` runs it again; the path is idempotent.

---

Back: [Product spec](01-product-spec.md) · Next: [Risks](03-risks.md) · [Measurements](04-measurements.md)
