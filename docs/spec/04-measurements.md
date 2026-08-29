# Game Optimizer — Measurements

**Date:** 2026-08-28
**Machine:** AMD Ryzen 9 9950X3D, 16C/32T, Windows 11 Home build 26200, one processor group
**Purpose:** the evidence behind every **[M]** claim in the other three documents, and the
record of two claims that measurement **overturned**.

Reproduce with `tools\build-probe.bat` then `build\topology_probe.exe`, and
`tools\build-behaviour.bat` then `build\behaviour_probe.exe`.

> **On the name.** The product was renamed from **CoreDirector** to **Game Optimizer** after
> these measurements were taken. Every measurement below was captured against a binary,
> a config folder and a window class carrying the **old** name, so the old name is left
> **verbatim** wherever it appears inside a quoted command, path, log line, program output or
> observed class name — §2.5's `CoreDirector.exe --bench` and its log block,
> §2.6's `GATE_B4` assertion, `%LOCALAPPDATA%\CoreDirector\config.ini`, the "no
> `CoreDirector.exe` running" control, and §2.7's `CoreDirectorCoreMap`. Editing those to
> match the new name would make this document lie about what was actually observed. Only the
> surrounding prose was updated. Under the new name the equivalents are
> `GameOptimizer.exe`, `%LOCALAPPDATA%\GameOptimizer\` and `GameOptimizerCoreMap`.

---

## 1. Topology — `topology_probe.exe`

Source: `tools\probe\topology_probe.cpp`. Compiled with MSVC 2019 Build Tools, Windows SDK
10.0.19041.0, `BUILD_EXIT=0`.

```
ActiveProcessorCount(ALL)=32  GroupCount=1
Id         LP   Core LLC  NUMA Grp  Eff  Parked
256        0    0    0    0    0    0    0
257        1    0    0    0    0    0    0
258        2    2    0    0    0    0    0
...
271        15   14   0    0    0    0    0
272        16   16   16   0    0    0    1
273        17   16   16   0    0    0    1
...
287        31   30   16   0    0    0    1
total cpuset entries = 32

Cache L3 type=0 size=98304KB line=64  grp=0 mask=0x000000000000ffff
Cache L3 type=0 size=32768KB line=64  grp=0 mask=0x00000000ffff0000
```

| Fact | Value | Consequence for the design |
|---|---|---|
| CPU Set `Id` base | **256**, not 0 | `Id != LogicalProcessorIndex`. Every API takes `Id`, all UI shows LP. Conflating them applies masks to the wrong cores. |
| `EfficiencyClass` | **0 on all 32 LPs** | Useless for CCD detection on AMD. A classifier keying on it finds no split on exactly the hardware that has one. |
| `CoreIndex` | 0,0,2,2,…,30,30 | SMT siblings share it. This is what `ReduceToNoSmt` keys on. |
| `LastLevelCacheIndex` | 0 for LP 0–15, 16 for LP 16–31 | The only field that separates the CCDs. |
| L3 size | **98304 KB** vs **32768 KB** | The cache CCD is identified by *largest L3*, never by domain order. |
| `Parked` | **0 on CCD0, 1 on all 16 LPs of CCD1** | A global policy had CCD1 entirely parked at probe time. |

## 2. Behaviour — `behaviour_probe.exe`

Source: `tools\probe\behaviour_probe.cpp`. **No affinity mask is set anywhere in this probe** —
a restrictive affinity mask is documented to override a conflicting CPU Set assignment, so
setting one would invalidate the result by construction.

### 2.1 Invalid CPU Set Id

```
--- Q3 invalid id 99999 ---
return=FALSE GetLastError=813
assignment after the failed call: 0 ids
```

**813 is `ERROR_CPU_SET_INVALID`** — Windows ships a dedicated error code for this, and the
prior assignment is left intact rather than partially applied. The error mapping in
`src\applier.cpp` handles 813 specifically; mapping only `ERROR_INVALID_PARAMETER` would have
misclassified the commonest real failure (a stale Id after a topology change).

### 2.2 A single unparked logical processor, heavily oversubscribed

```
--- Q1 single unparked LP, 8 busy threads ---
SetProcessDefaultCpuSets(1 ids) -> TRUE
GetProcessDefaultCpuSets read back 1 ids
8 busy threads for 2000 ms, 26796 samples.
   LP  1 :  26796  (100.0%)
   => 1 distinct logical processors used
```

Baseline with no assignment, same 8 threads: **204,237 samples across LP 0–15**.

**This overturns a claim made in an earlier draft of the architecture document.** That draft
said CPU Sets are "a scheduler preference with a legal fallback … the system retains the
freedom to run it elsewhere rather than not at all." On this machine that is false:
**8 threads on a 1-LP set produced 100.0% of samples on that one processor, with zero
spillover, and 7.6× less work done.** Microsoft's wording ("will typically execute on one of
the processors in its list") permits either behaviour; what this build actually does is
enforce it.

**Product consequence:** a badly chosen mask genuinely starves a process. It is not a
self-correcting hint. That is why the core map is shown before anything is applied and why
masks are editable — those stopped being nice-to-haves when this measurement came in.

**Scope of the claim:** one machine, one Windows build, a single-LP set, a 2 s window. Whether
a *multi-LP* unparked set also shows zero spillover was not separately isolated. Not
generalised beyond this build.

### 2.3 A set made entirely of PARKED processors

```
--- Q2 ENTIRELY PARKED set, 8 busy threads ---
SetProcessDefaultCpuSets(16 ids) -> TRUE
GetProcessDefaultCpuSets read back 16 ids
8 busy threads for 2000 ms, 271316 samples.
   LP  0 :  10830 (4.0%)  ...  LP 15 : 20007 (7.4%)
   => 16 distinct logical processors used
```

All 16 assigned processors were parked (CCD1, LP 16–31). The threads ran on **LP 0–15** — the
*unparked* CCD — at full baseline throughput. Two readings, both useful:

1. **An all-parked mask does not stall a process.** It falls back to what is available. The
   feared failure ("the app pins a game to a parked CCD and the game hangs") **did not occur**.
2. The parked CCD did not un-park to service the request within the 2 s window.

### 2.4 The finding that matters most: success that silently does nothing

Section 2.3 is the whole hazard in one place:

- `SetProcessDefaultCpuSets` returned **TRUE**
- `GetProcessDefaultCpuSets` read back **all 16 requested ids**
- and **not one of those 16 processors ran a single sample**

**Both the setter and the getter reported complete success on an assignment the scheduler
ignored entirely.** The getters echo *stored intent*, never *effective placement*.

This invalidated the release gate as originally written. Gate B item 1 said to verify by
reading the assignment back with `GetProcessDefaultCpuSets` — which would have returned a
green tick for the completely ineffective assignment above. **Verifying the layer next to the
claim is not verifying the claim.** The gate now requires sampling
`GetCurrentProcessorNumberEx` in the governed process and asserting that observed processors
are a subset of the assigned mask. See [architecture §8](02-architecture.md).

## 2.5 Watcher tick cost — and a benchmark that measured its own clock

Running the built `CoreDirector.exe --bench`, which drives `Engine::TickOnce` 40 times:

```
[13:44:17.235] [main] topology: AMD asymmetric cache (X3D), 32 LPs, 2 domain(s), sig=amd:2:16,16:98304,32768
[13:44:17.237] [main] no config.ini yet - starting from defaults
[13:44:17.700] [bench] 40 ticks: min=0 ms median=15 ms max=16 ms
```

Two results, one of them about the instrument rather than the subject.

**The instrument was broken.** `min=0 median=15 max=16` is not a distribution of tick costs; it
is the **15.6 ms `GetTickCount64` timer granularity**. A tick either fell inside one timer
interrupt (0) or straddled one (15–16). The benchmark could not have reported any other numbers
regardless of how fast the code was, so it measured nothing. It now uses
`QueryPerformanceCounter`.

**The real figure comes from the surrounding log timestamps**, which have millisecond resolution
and cover 40 iterations so the quantisation averages out: `17.237` → `17.700` = **463 ms for 40
ticks ≈ 11.6 ms per tick**, on a desktop running roughly 400 processes.

Against a 250 ms poll interval that is **~4.6% of one core**, held continuously. The cause is
`ProcessSnapshot::Take` calling `OpenProcess` + `GetProcessTimes` + `QueryFullProcessImageNameW`
for **every** process on **every** tick — about 1200 syscalls — when `creationTime` and
`fullPath` are immutable per pid and the previous snapshot already holds both.

This overturned a second claim: the architecture document had asserted **[A]** that a tick was
"sub-millisecond … well under 0.5% of one core". It is roughly an order of magnitude worse.

**Also confirmed by this run:** topology detection works end-to-end on live hardware, producing
exactly the classification and signature the synthetic unit tests assert
(`amd:2:16,16:98304,32768`).

### After the fix

Both defects were fixed and re-measured with the `QueryPerformanceCounter` instrument,
**interleaved A/B/C 8 seconds apart** so machine drift cannot account for the difference:

```
BEFORE     40 ticks over 302 processes: min=10867.6 us median=13839.4 us max=17417.5 us
PATHCACHE  40 ticks over 303 processes: min= 6285.7 us median=10215.7 us max=14345.0 us
FINAL      40 ticks over 298 processes: min= 4397.1 us median= 5782.6 us max=12219.9 us
```

`PATHCACHE` skips only `QueryFullProcessImageNameW`; `FINAL` also skips `GetProcessTimes` when
the CPU-percentage rule is off. **Median tick 13.8 ms → 5.8 ms**, about 2.3% of one core at the
250 ms default.

Two cross-checks worth recording: the new instrument's "before" median (11,574 µs on an earlier
run) **independently reproduces the 11.6 ms figure derived from log timestamps** above, which is
what gives confidence that the derivation was sound rather than lucky; and the unit tests still
report `TOTAL 336 PASSED 336 FAILED 0` after the change.

**[A]** The remaining ~5.8 ms is unattributed — not measured, not claimed.

## 2.6 Game Optimizer is not the only writer of this API on the target hardware

**The most consequential finding of the design work, and it was found by a release gate
failing for a reason that was not a product defect.**

`GATE_B4` asserted "no process on the system has a default CPU set that CoreDirector
installed", implemented as "no process has one at all". It failed:

```
processes with a non-empty default CPU set assignment: 51
GATE_B4 FAIL 51 process(es) still carry a default CPU set assignment
```

### What the 51 actually were

A direct scan via `GetProcessDefaultCpuSets` over every readable process:

| Count | Assigned ids | Processes |
|---|---|---|
| 50 | `272,273,…,287` — **all 16 ids of CCD1** | `claude`, `firefox` |
| 1 | `256,258,…,270` — `Cache no SMT` | a live Gate B worker |

### Ruling out Game Optimizer

- **No `config.ini` existed** — `%LOCALAPPDATA%\CoreDirector\config.ini` was absent, so the
  app had never been configured or saved.
- **The journal was empty** (3 bytes, 0 lines).
- **The log contained no apply events** — only `gateb_probe` recovery lines.
- **The Gate B harness config carries no heavy list**: it constructs a default `cd::Config`
  with one profile, `heavyMask = L""` and no `heavy` entries.

### The decisive experiment

1. All 50 assignments were cleared → immediately afterwards the count was **0**.
2. **75 seconds elapsed with no `CoreDirector.exe` and no `gateb_probe.exe` running.**
3. Re-scan: **3 `firefox` processes had acquired an assignment again.**

Control checks in the same session: a **brand-new `notepad`** had none, the scanning
PowerShell process had none, and `explorer` had none — so it is not applied blanket to
everything.

### Conclusion, scoped precisely

**[M]** Something other than Game Optimizer writes default CPU Sets on this machine, targeting
selected processes with exactly the 16 ids of CCD1, and it re-applies within roughly a minute
of being cleared.

**[A]** The author is *not* proven. This machine runs the AMD 3D V-Cache Performance Optimizer
driver with `amd3dvcacheSvc` active, and CCD1 is the frequency CCD, so that stack is the
plausible candidate — but attribution was not established by experiment, and stopping the
service to test would need administrator rights and would alter the user's machine. **It is
recorded as "a third-party writer exists", not as "the AMD driver does it".**

### What it changes

1. **`GATE_B4` was wrong.** "No residue" must mean *no residue Game Optimizer left*, scoped to
   the pids it governed plus the journal — not "the system is pristine". A gate that cannot
   pass on the target hardware for reasons unrelated to the product is a broken gate, and this
   one would have been "fixed" by someone deleting it.
2. **A new HIGH risk**: two writers of the same per-process API, one of them invisible. See
   [risks §2b](03-risks.md).
3. It explains an earlier confusing observation: one Gate B run reported `none` and a later
   one reported 51. The third party reassigns gradually — 3 processes in 75 s — so the count
   depends entirely on when you look.

## 2.7 Two GUI defects that every gate passed over

Found by the operator scrolling the Settings window once, after Gate A was green
(`BUILD_EXIT=0`, 336/336 unit tests) and Gate B was 4/4. **Neither gate draws a pixel**, so
neither could have caught either bug. This section exists so that limitation is on the record
rather than discovered again.

### Defect 1 — scroll repaint corruption

Reproduced with `tools\ui-capture.ps1`, which drives the live app: closes the wizard, opens
Settings via the tray command id, captures, sends `WM_VSCROLL`/`SB_PAGEDOWN`, captures again.

After one page-down: ghost copies of the Add / Duplicate / Remove / Rename buttons remained at
their pre-scroll positions, static text was drawn at two y positions at once, and the fixed
OK / Cancel / Apply footer appeared twice.

**Cause.** `ScrollTo` moved ~42 children with `SetWindowPos` and then called
`InvalidateRect(hwnd, nullptr, TRUE)`. That marks only the **parent** dirty; it does not repaint
children, and it does not defeat the bit-copy `SetWindowPos` performs when moving a window.

**Fix.** `RedrawWindow(..., RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW)` at
every relayout site, `SWP_NOCOPYBITS` on every child move, and the whole batch moved atomically
through `BeginDeferWindowPos` / `EndDeferWindowPos`. A latent instance of the same bug on
`WM_SIZE` was fixed at the same time.

**Verified** by re-running the same capture harness against the rebuilt binary: no ghosting, no
smearing, footer drawn once.

### Defect 2 — the core map was never created at all

The same captures showed the core map area blank. It was blank **before any scrolling**, so it
was a second, independent defect rather than a symptom of the first.

**[M]** Enumerating the Settings window's children at runtime found **43 controls and not one of
class `CoreDirectorCoreMap`**. The control did not exist.

**Cause.** `cd::CoreMapRegister()` is defined in `src\coremap.cpp` and declared in `src\ui.h`,
and **is never called from anywhere**. The class is therefore never registered,
`CreateWindowExW` returns NULL, and the site's own `if (st->hMap)` guard skips setup **without
logging anything**. The first-run wizard creates a core map too and had the same blank panel.

**Why this one matters beyond cosmetics:** the core map is the entire mitigation for risk 1
(wrong CCD detected) and part of risk 2a (a fully parked mask). Without it a user cannot confirm
or edit what was detected — the product's main defence against its worst failure mode was
absent, while every gate reported green.

### What this changes about the gates

1. **A clean build plus green tests says nothing about whether a window renders.** Both defects
   sat behind a successful compile.
2. **`declared → implemented → never called` is invisible to the compiler**, because the
   definition is referenced by nothing and the link still succeeds. A grep for a call site of
   each function declared in `src\ui.h` is a cheap check that would have caught defect 2 at
   build time.
3. **A silent `if (handle)` guard converts a hard failure into a blank rectangle.** Creation
   failures now log `GetLastError` and show a visible message in the space instead.
4. `tools\ui-capture.ps1` makes the observation repeatable, so a fix is verified with the same
   instrument that found the defect.

## 3. Environment

| Probe | Result |
|---|---|
| `HKCU\Software\Microsoft\GameBar\AutoGameModeEnabled` | **0** — Windows Game Mode is OFF |
| `HKCU\Software\Microsoft\GameBar\AllowAutoGameMode` | 1 |
| AMD 3D V-Cache Performance Optimizer driver | present, **v1.0.0.12** |
| Service `amd3dvcacheSvc` | **Running** |
| `ms-settings` protocol | registered |

Note the combination: **Game Mode is off, the AMD service is running, and CCD1 is entirely
parked.** These are three separate facts and the first-run wizard reports them separately.

## 4. Child process inheritance

Measured by a research lane on this machine (Windows 11 build 26200): a PowerShell process set
its own default CPU sets to 2 ids and confirmed `GetProcessDefaultCpuSets` returned `count=2`;
it then spawned `cmd.exe` and queried the child, which returned `ret=True, count=0`.

**Children do not inherit their parent's default CPU Sets.** This is the premise the entire
watcher design rests on, and it is measured rather than assumed.

An adversarial pass then searched specifically for a route that would make inheritance work —
the full reverse-engineered job-object class space, the native `PS_ATTRIBUTE` creation list,
and three CPU-related undocumented `PROCESSINFOCLASS` values. It found unexplored surface
(`NtCreateCpuPartition` and friends are real and implemented; `ProcessAssignCpuPartitions` and
`ProcessDisableSystemAllowedCpuSets` are recognised classes) but **no working inheritance
route**. `JobObjectCpuPartition` (class 52) returns `STATUS_INVALID_INFO_CLASS` on this build,
matching its negative controls.

Recorded as: no route found on build 26200, after a deliberate attempt to find one. Not "there
is no such thing."

## 5. Claims this document does NOT support

Stated so nothing here is over-read:

- Nothing was measured on Intel hybrid, symmetric dual-CCD, single-CCD, >64-logical-processor
  or laptop hardware. Those code paths are covered only by synthetic unit tests.
- No anti-cheat was involved in any measurement. No game was running.
- The attribution of CCD1's parked state specifically to the AMD driver rather than to
  ordinary idle core parking was **not** separated by experiment.
- Tick cost of the 250 ms watcher loop is not measured here; `--bench` exists to measure it.

---

Back: [Product spec](01-product-spec.md) · [Architecture](02-architecture.md) · [Risks](03-risks.md)
