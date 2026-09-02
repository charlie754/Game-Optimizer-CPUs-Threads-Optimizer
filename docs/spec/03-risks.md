# Game Optimizer — Risk Register

**Date:** 2026-08-28
Severity: **BLOCKER** ship-stopping · **HIGH** must be mitigated before public release ·
**MEDIUM** mitigate or document · **LOW** document.

Provenance tags: **[M]** measured this turn · **[S]** sourced · **[A]** assumed, not checked.

---

## 1. Wrong CCD detection — **HIGH**

**The failure:** the app labels the frequency CCD "Cache", pins the game to the wrong half,
and the user gets *worse* frame times while believing the tool is working. This is the
worst class of bug here because it is silent and it inverts the product's whole value.

**Why it is plausible.** **[M]** On the reference 9950X3D, `EfficiencyClass` is `0` on all
32 logical processors and `NumaNodeIndex` is `0` on all of them. The *only* signal
separating the CCDs is L3 size — 98304 KB against 32768 KB. A detector that reached for the
usual fields would find nothing and would have to guess.

**Mitigations**
1. Detection keys on L3 size, which is the field that actually carries the distinction, and
   the classifier records which CASE fired (`AMD_ASYMMETRIC`, `MULTI_CCD_SYMMETRIC`, …)
   together with a confidence level.
2. **The core map is shown before anything is applied**, on first run, with L3 size printed
   per domain. The user confirms against what they know about their own CPU.
3. Every mask is editable by clicking logical processors. A wrong guess costs two clicks,
   not a reinstall.
4. `MULTI_CCD_SYMMETRIC` reports confidence MEDIUM and names the masks `CCD0`/`CCD1` rather
   than inventing a cache/frequency claim it cannot support.
5. `[topology] signature` in the config detects a changed machine (BIOS SMT toggle, a CCD
   disabled, a different PC) and re-derives instead of applying stale `Id`s.

**Residual.** On a symmetric dual-CCD part (7950X, 9950X) the app genuinely cannot tell
which CCD boosts higher. It says so rather than guessing. **[A]** Whether the preferred CCD
is reliably reported by any documented Windows API on symmetric parts is **not established
here** — treated as an open question, not as a settled impossibility.

## 2. Anti-cheat — **HIGH**

**The failure:** a kernel anti-cheat treats the tool's process handle as tampering and
blocks it, blocks the game, or bans the user.

**Design decisions taken specifically to minimise this**
1. **No code ever enters a game process.** No DLL injection, no hooks, no overlay, no
   `WriteProcessMemory`, no driver, no kernel component. The only thing the app does to a
   game is call a documented Win32 scheduling API on a handle.
2. **Minimum access rights.** `OpenProcess` requests only
   `PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION`. It never requests
   `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION` or `PROCESS_ALL_ACCESS` —
   those are the rights that read as memory tampering.
3. **Anti-cheat services are on the default exclusion list**, so the app does not call
   anything on `EasyAntiCheat.exe`, `BEService.exe` or `vgc.exe` even though they appear in
   the game's descendant tree.
4. **No elevation is requested**, so the app is not running at a privilege level that would
   make its handle interesting.
5. If `OpenProcess` on the game fails, the app reports it plainly in the tooltip and stops
   trying — it does not escalate, retry with wider rights, or attempt a workaround.

**Three outcomes, not two.** An earlier draft framed this as ban-or-block. There is a third,
and a vendor documents it: BattlEye's FAQ **[S]** says they "only ever ban for the use of
actual cheats/hacks", that non-cheat enhancement tools "are generally supported unless desired
otherwise by the game developers" — but also that BattlEye "might decide to **kick** (not ban)
you at some point for using a specific program (such as macro tools), but that won't
automatically flag you as a cheater."

| Outcome | Likelihood | What the app does |
|---|---|---|
| **Ban** | Very low. No vendor policy targets scheduling tools, and Riot's Vanguard FAQ draws its line at tools *reading game memory* **[S]** — which this app never does. | Nothing to do. |
| **Block** | Real and per-vendor. A kernel anti-cheat's `ObRegisterCallbacks` pre-operation callback strips rights from the `OpenProcess` mask and the call returns `ERROR_ACCESS_DENIED`. | Reports the game as blocked by name, stops trying, never escalates rights. |
| **Kick** | Documented by BattlEye for benign tooling. Mid-session, no ban. | Cannot be prevented or detected by the app. Must be stated in the README so a user can decide. |

This also **corrects a claim in the first draft** that no anti-cheat vendor publishes a
relevant policy. BattlEye does, it is disinterested, and it is more favourable than the draft
assumed on bans and less favourable on kicks.

**Why CPU Sets are the safer of the available APIs** **[S]**: `SetProcessDefaultCpuSets`
requires only `PROCESS_SET_LIMITED_INFORMATION`, whereas `SetProcessAffinityMask` requires the
stronger `PROCESS_SET_INFORMATION`, and `AssignProcessToJobObject` requires
`PROCESS_TERMINATE` on the target — a right an anti-cheat has every reason to deny. The
job-object route is therefore *more* hostile to anti-cheat than the one chosen, not less.

**Honest statement of what is not known.** **[A]** No measurement in this work involved any
anti-cheat; no game was running. Vanguard (Valorant) is the most aggressive commonly
encountered case and the most likely to refuse the handle. The mitigation for the unknown is
behavioural rather than technical: **the app degrades to doing nothing to that game and says
so.**

**Open item before public release:** the README must state plainly that anti-cheat interaction
is **untested**, and must mention the kick outcome. It must not imply safety.

## 2a. Assignment accepted and silently ignored — **HIGH**

**[M] The single most important finding of the design work**, and it had no entry in the first
draft of this register.

Assigning a set made entirely of parked processors produced: `SetProcessDefaultCpuSets`
returned **TRUE**; `GetProcessDefaultCpuSets` read back **all 16 requested ids**; and **none
of those 16 processors ran a single sample** — the threads ran on the other CCD at full
baseline throughput ([measurements §2.3](04-measurements.md)).

Microsoft documents a second route into the same state **[S]**: an assignment naming CPU sets
"allocated exclusively to other processes" is *ignored*, and a restrictive affinity mask set by
anything else "is respected above any conflicting CPU Set assignment".

**So there are three outcomes, not two: applied, failed, and accepted-then-ignored.** The
third is invisible to the setter's return value *and* to the getter, which means a tool that
checks either one will confidently report success while doing nothing.

**Mitigations**
1. The tray reports what the app **assigned**, never that a mask is "active" — a distinction
   the UI copy has to keep, because the API cannot support the stronger claim.
2. Settings offers **Inspect processes...**, which reports per governed process: the assigned
   mask and its processor count, what Windows reports back, how many of the mask's processors
   are currently parked, and whether something else has set a restrictive affinity mask.
   **It does not confirm placement**, and its own text says so — showing where a process is
   actually running would mean executing code inside it, and this app never injects anything.
3. Settings **warns when a selected mask is entirely parked**, the configuration most likely
   to be ignored, and the core map draws live parked state.
4. The app reads (never writes) each governed process's affinity mask and warns when one is
   set, because that silently defeats the CPU Set.
5. Gate B verifies **placement, not assignment** — this finding is what forced that rewrite.
   The gate can sample placement only because it owns the child process it measures; there is
   no such route into a user's game.

**Residual, and it is larger than an earlier draft of this register claimed.** That draft
credited a Settings action that sampled where a governed process was actually running; **no
such action exists and none was ever built**, so mitigations 2-4 are not a check on placement.
They detect the two *documented* routes into accepted-then-ignored — parked processors, and a
restrictive affinity mask — and a mask ignored for any third reason still reads as applied.
The Inspect report is also a snapshot taken when the user opens it, so a game that sets its own
affinity mask mid-session is caught only if the user looks again. **The only placement evidence
in this product is Gate B**: a child process the harness spawned, on the developer's machine,
before release — never the user's game on the user's machine.

## 2b. Another program writes the same per-process API — **HIGH**

**[M] Measured, and it is the risk that was not anticipated at all.** On the reference machine
**50 processes** (`claude`, `firefox`) carried default CPU Sets of exactly `272…287` — all 16
ids of CCD1 — with **no Game Optimizer config on disk, an empty journal, and no apply events in
the log**. They were cleared; **75 seconds later, with nothing of ours running, three of them
had the assignment back** ([measurements §2.6](04-measurements.md)).

**[A]** The author is not identified. The AMD 3D V-Cache stack is the plausible candidate on
this hardware and is *not* proven; it is recorded as "a third-party writer exists".

**Why this matters more than the Game Mode risk above.** Game Mode influences scheduling by a
different mechanism. This is *the same call on the same objects*: `SetProcessDefaultCpuSets` is
last-writer-wins per process, with no ownership, no arbitration and no notification. Two
consequences follow directly:

1. **The other writer can silently overwrite a Game Optimizer mask** at any moment, and nothing
   in the API tells us. This is risk §2a arriving by a third route: the assignment we made is
   simply no longer there, while every call we made succeeded.
2. **Game Optimizer can silently overwrite theirs.** Worse, *clearing* is indiscriminate —
   `SetProcessDefaultCpuSets(h, NULL, 0)` removes whatever is there, not "ours". If
   Game Optimizer governs `firefox.exe` as a heavy app and then clears it on game exit, it also
   erases an assignment it never made.

**Mitigations**
1. **Clear only what we applied.** The engine already tracks applied state per pid and the
   journal records it; recovery clears only journalled pids. This was already the design and
   it is now load-bearing rather than tidy.
2. **Never clear on a blanket sweep.** There must be no "clear all CPU sets on the system"
   action anywhere in the product, however tempting as a reset button.
3. **The Inspect report shows the actual readback**, so a mask that another writer replaced is
   visible as a mismatch rather than assumed present.
4. **Gate B4 is scoped to pids Game Optimizer governed**, and reports other processes'
   assignments as information rather than as a failure.

**Residual, and it is not solved.** There is no way to take ownership of a process's CPU Set
assignment, no change notification, and no way to tell "nobody set this" from "we set it and
someone replaced it with the same value". On hardware carrying such a driver, Game Optimizer and
that driver are in an unarbitrated race for any process both care about. **[A]** How often that
actually bites during a game session was not measured.

**Open question worth answering before public release:** whether the other writer targets only
background processes (both observed names are on the default heavy list, which may be
coincidence — they are also simply the busiest non-game processes on this machine) or would
also contend for the game itself.

## 3. Windows Game Mode and the AMD V-Cache driver — **HIGH**

**The failure:** two schedulers fight. The user sets a per-game CPU Set; a global policy
contradicts it; frame times get worse and the tool looks broken.

**The mechanism, corrected.** An earlier draft of this document said Windows Game Mode "parks
the second CCD". That is not accurate **[S]**. Game Mode does not park anything itself. On
dual-CCD X3D parts the **AMD 3D V-Cache Performance Optimizer driver** (with the AMD PPM
Provisioning driver) watches **Xbox Game Bar's "this process is a game" signal** and parks the
non-V-Cache CCD so the game stays on the cache die. Game Bar and Game Mode are the
*detection prerequisite* the AMD mechanism depends on — with Game Mode off, that automatic
parking does not engage.

That inverts the reason for the advisory but not the advice. The wizard's message is therefore
about **which of two mechanisms is placing your game**, not about Game Mode being harmful:
Game Optimizer does per-game placement explicitly, so leaving Windows' automatic mechanism on
means two systems making the same decision by different rules.

The old Windows 10 Game Mode API that granted a game *exclusive CPU sets*
(`GetExpandedResourceExclusiveCpuCount`, `ReleaseExclusiveCpuSets`) was **deprecated in
Windows 10 1809** **[S]**; today's consumer Game Mode has no documented CPU Sets behaviour. So
"Game Mode will override my CPU sets" is not the real conflict — CCD parking underneath is.

Separately, **"AMD/ASUS Turbo Game Mode" and Gigabyte's "X3D Turbo Mode" are BIOS features
unrelated to Windows Game Mode** **[S]**: they hard-disable SMT and, on Ryzen 9 dual-CCD parts,
hard-disable the second CCD at boot. That is the session-wide cost this product exists to
avoid, and it is worth not conflating the two in user-facing copy.

**[M] Measured on the reference machine, and this is not hypothetical:**
- `HKCU\Software\Microsoft\GameBar\AutoGameModeEnabled` = `0` (Game Mode off) and
  `AllowAutoGameMode` = `1`.
- The **AMD 3D V-Cache Performance Optimizer** driver v1.0.0.12 is installed and the
  `amd3dvcacheSvc` service is **Running**.
- At probe time **every one of the 16 logical processors on CCD1 reported `Parked=1`**,
  while all 16 on CCD0 reported `Parked=0`.

That last line is the risk in concrete form: something on this machine already applies a
global CCD preference, with Game Mode *off*. **[A]** Attributing the parking specifically to
the AMD driver is inference, not measurement — `Parked` is a dynamic state and an idle CCD
can park on its own. The two have not been separated by experiment here.

**Mitigations**
1. The first-run wizard reads the Game Mode key and reports it, and separately reports
   whether the AMD V-Cache service is present and running. Two distinct facts, two lines.
2. The app **advises and never enforces.** It does not toggle Game Mode, does not stop the
   AMD service, does not unpark cores, and works whatever their state.
3. The core map shows live `Parked` state, so a user looking at a fully parked CCD can see
   it rather than wondering why a mask "did nothing", and Settings warns outright when a
   selected mask is entirely parked.
4. **[M]** A mask pointing at a fully parked CCD **degrades to normal scheduling rather than
   stalling the process** — measured directly ([measurements §2.3](04-measurements.md)): 8
   threads assigned 16 parked processors ran on the *other* CCD at full baseline throughput.
   The feared "pinned to a parked CCD and the game hangs" outcome did not occur.
   Note this is the opposite of the reasoning in an earlier draft, which reached the same
   conclusion from the *wrong premise* that CPU Sets are a soft hint. They are not soft
   ([measurements §2.2](04-measurements.md)); they are ignored wholesale when unsatisfiable.

**Residual.** If a global policy has parked the CCD a user just assigned a game to, the result
is **indistinguishable from the app doing nothing** — and every API reports success. That is
risk §2a, and surfacing parked state is the whole mitigation — the core map, the
entirely-parked warning, and the parked line in the Inspect report. That neither removes the
conflict nor confirms the mask took effect: the app cannot observe placement. **[A]** AMD's
parking is also reported to be dynamic and load-reactive rather than a hard removal, so a
parked CCD may un-park under load — not observed within the 2 s measurement window, and not
established here.

## 4. Child processes missed — **HIGH**

**The failure:** the reported original problem. A launcher spawns a worker minutes into a
session, it lands on the game CCD, and frame times hitch until it exits.

**[M]** There is no inheritance to lean on: `JOBOBJECTINFOCLASS` in Windows SDK
10.0.19041.0 has no CPU-set class, and `PROC_THREAD_ATTRIBUTE_*` has no CPU-set attribute.
Both read from the headers on disk this turn. Scoped to that SDK version.

**Mitigations**
1. Transitive descendant tracking, re-evaluated every poll period, so the exposure window is
   one tick (250 ms default) rather than unbounded.
2. Parent validation by process creation time, so a **reused PID** cannot graft an unrelated
   process onto the game's descendant tree — the bug that would otherwise pin something
   random to the game CCD on a long-uptime desktop.
3. Gate B item 2 makes "a child spawned *after* the parent was masked is governed within one
   tick" a **release blocker**, verified by the §8.1 placement method on the child itself —
   the harness samples `GetCurrentProcessorNumberEx` inside the child it spawned, which it can
   do only because it owns that process. It is not verified by inspecting the app's internal
   state, which would prove only that the app believes it worked, nor by reading the child's
   default CPU sets back, which is the accepted-then-ignored trap §2a is about.

**Residual.** A process that is a *logical* child but not a *process-tree* child — spawned
through a broker or a pre-existing service, e.g. some launcher and overlay architectures —
is not in the descendant set and must be added to `heavy` by hand. Documented, not solved.

## 5. Elevation and access denied — **MEDIUM**

Non-elevated Game Optimizer cannot open an elevated process. Some launchers and update
services run elevated, and a game started from an elevated launcher inherits that.

**Mitigations:** the tooltip and Settings report `n blocked (access denied)` **by name**
rather than skipping silently; an opt-in elevated mode exists. Autostart is `HKCU\...\Run`,
which cannot elevate — so if a user turns on both elevated mode and start-with-Windows, the
app must say plainly that the autostarted instance will not be elevated, instead of
appearing to work and quietly failing every morning.

## 6. Stale masks after a crash — **MEDIUM**

If Game Optimizer is killed while masks are applied, background apps stay pinned with nothing
left to clear them. Mitigated by the restore journal (architecture §6.1) with a
creation-time match so a recycled PID is never cleared by mistake, plus running the same
clear path on `WM_QUERYENDSESSION`.

**Residual.** A journal write that is itself interrupted could leave a partial final line;
the reader must tolerate and discard a malformed trailing line rather than abort, or the
recovery path becomes the bug.

## 7. Hand-rolled config parsing — **LOW**

A hand-written JSON parser would be a real correctness liability. Avoided by choosing INI
with a three-rule grammar and `|` as the list separator, which is illegal in Windows
filenames and therefore cannot appear inside a value. Unknown keys are preserved on
rewrite so a newer version's config is not destroyed by an older binary.

## 8. Untested hardware classes — **LOW, but must not be claimed as working**

Verified here **[M]**: AMD Zen 5 dual-CCD X3D, single processor group, 32 LPs.

**Not tested anywhere in this work** — Intel hybrid (P/E cores), symmetric dual-CCD, single
CCD, more than 64 logical processors, and any laptop. The Intel path additionally depends on
an `EfficiencyClass` ordering that is **[S]** sourced but not confirmed against Microsoft
Learn in this turn, and not measured on hardware.

**Mitigation:** these paths exist in code and are structurally exercised by the unit tests
via synthetic topologies, but the README states which hardware has actually been run. A
synthetic test proves the classifier maps inputs to outputs; it does not prove the inputs
resemble a real Intel machine.

---

## Open items carried forward

| # | Item | Why it matters | Status |
|---|---|---|---|
| 1 | `EfficiencyClass` ordering on Intel hybrid | Decides which mask is labelled P-cores | **[A]** — confirm against Microsoft Learn before Intel ships |
| 2 | Anti-cheat behaviour on `PROCESS_SET_LIMITED_INFORMATION` | Decides whether the tool works on EAC/BE/Vanguard titles | **[A]** — no anti-cheat was involved in any measurement; README must say untested |
| 3 | Whether the AMD V-Cache driver, not idle parking, parked CCD1 | Decides how loudly the wizard warns | **[A]** — not separated by experiment |
| 4 | Measured tick cost of the 250 ms snapshot loop | Claimed sub-1% of a core | **[A]** — `--bench` exists to measure it; number not yet taken |
| 5 | Behaviour above 64 logical processors / multiple groups | Cross-group ids are documented to be *ignored* when they do not match the thread's group | **[A]** — no such hardware here; v1 derives masks within one group and says so |
| 6 | Whether a multi-LP unparked set also shows zero spillover | Decides how sharp the "a wrong mask starves the process" warning must be | **[A]** — only the single-LP case was isolated |
| 7 | Whether AMD's CCD parking un-parks under sustained load | Decides whether an all-parked mask is permanently or temporarily inert | **[A]** — not observed in a 2 s window |

Every row is an **[A]**. None blocks v1 on the reference hardware, and **none may be written up
as settled until it is measured.**

### Deliberately out of scope, recorded so they are not rediscovered as ideas

Each of these surfaced during research as a real capability and was rejected for v1:

| Route | Why not |
|---|---|
| `ReservedCpuSets` / IoT `SetRTCores` core reservation | Machine-wide, needs administrator **and a reboot**, and Microsoft warns that misconfiguration "results in system malfunction and requires reimaging to recover". Wrong blast radius for a tray app. |
| IFEO `PerfOptions` registry priority | Needs no process handle at all, so it survives anti-cheat handle stripping — but it exposes only priority, I/O priority and working set. **There is no affinity or CPU-set value.** Solves a problem this product does not have. |
| Forcing or clearing Game Bar's per-app "Remember this is a game" flag | Would let the app trigger or suppress AMD's CCD parking per title. It works by manipulating another vendor's classification of the user's software, which is a larger commitment than v1 should make. Genuinely interesting for v2. |
| Launching the game ourselves `CREATE_SUSPENDED` and setting sets on the creation handle | Never calls `OpenProcess`, so no pre-operation callback can strip rights. **[A]** Untested against any anti-cheat, and it turns the app into a launcher. Recorded as a fallback if blocking turns out to be common. |
| `SetProcessDefaultCpuSetMasks` | The Windows 11 `GROUP_AFFINITY` variant. Not in SDK 10.0.19041.0 **[M]**, and only needed above 64 logical processors. Revisit with item 5. |

---

Back: [Product spec](01-product-spec.md) · [Architecture](02-architecture.md) · [Measurements](04-measurements.md)
