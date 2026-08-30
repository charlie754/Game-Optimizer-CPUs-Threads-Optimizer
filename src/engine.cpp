// Game Optimizer - the decision loop and the threading contract.
//
// See engine.h for the contract this file implements. Two things are worth repeating here
// because they are the parts a later edit is most likely to break:
//
//   * ComputeDesired and BuildTooltip are PURE. No Win32, no globals, no I/O. They are the
//     seam the unit tests drive with synthetic snapshots, which is the only way the
//     precedence rules get covered without a running game. The All Games profile does NOT
//     change that: its "is this a game?" answer arrives as DATA in Profile::game (a
//     pipe-separated candidate list) that the WATCHER refreshes before each call - see
//     Rule 1b and Engine::Impl::RefreshAllGamesSpec.
//   * The applier is NEVER called while the status mutex is held. OpenProcess on a busy
//     machine can block for milliseconds; the UI thread reads status under that same mutex
//     and must never be parked behind a syscall.
#define WIN32_LEAN_AND_MEAN
#include "engine.h"

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>

#include "applier.h"
#include "games.h"
#include "procwatch.h"
#include "util.h"

namespace cd {

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

namespace {

// PIDs 0 (System Idle) and 4 (System) are never governed under any configuration, including
// a user-edited exclusion list that omits them.
inline bool IsReservedPid(DWORD pid) {
    return pid == 0 || pid == 4;
}

// NOTIFYICONDATA::szTip is 128 wide chars INCLUDING the terminator. A longer string is
// silently rejected by the shell - the icon simply keeps its old tip - so truncate here
// rather than discovering it as a missing-tooltip bug.
std::wstring ClampTip(std::wstring s) {
    const size_t kMax = 127;
    if (s.size() <= kMax) return s;
    s.resize(kMax - 3);
    s += L"...";
    return s;
}

bool SameGoverned(const std::vector<GovernedProcess>& a,
                  const std::vector<GovernedProcess>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].pid != b[i].pid) return false;
        if (a[i].blocked != b[i].blocked) return false;
        if (a[i].applyResult != b[i].applyResult) return false;
        // Compared, or the settings window is never told that the auto-pinned SET moved while
        // the count and the masks stayed the same - which is the one transition the new
        // readout exists to show.
        if (a[i].autoPinned != b[i].autoPinned) return false;
        if (a[i].maskName != b[i].maskName) return false;
        if (a[i].name != b[i].name) return false;
    }
    return true;
}

// Everything the tray would render. lastTickMs is deliberately excluded: it jitters by a
// millisecond on every tick and would otherwise post a window message four times a second
// forever.
bool StatusEquivalent(const EngineStatus& a, const EngineStatus& b) {
    return a.active == b.active
        && a.paused == b.paused
        && a.gamePid == b.gamePid
        && a.gameProcCount == b.gameProcCount
        && a.heavyCount == b.heavyCount
        && a.blockedCount == b.blockedCount
        && a.staleTopology == b.staleTopology
        && a.profileName == b.profileName
        && a.gameMaskName == b.gameMaskName
        && a.heavyMaskName == b.heavyMaskName
        && a.tooltip == b.tooltip
        && SameGoverned(a.governed, b.governed);
}

// Resolve a mask NAME to CPU Set Ids. An empty name means "clear" and is legal. A non-empty
// name that is not in the config is a broken config: report it, do not guess, and above all
// do not fall through to the empty-ids path, which would silently CLEAR a process the user
// asked to have pinned.
// QueryPerformanceFrequency is fixed for the lifetime of the system (documented since
// Windows XP), so it is read once. Returned as ticks-per-microsecond so the hot path is a
// single division. A failed or absurd frequency degrades to 1.0, which produces obviously
// wrong numbers rather than a divide by zero.
double QpcTicksPerMicro() {
    static const double kPerUs = []() -> double {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return 1.0;
        return static_cast<double>(f.QuadPart) / 1000000.0;
    }();
    return kPerUs;
}

// The candidate executables a profile matches on.
//
// A normal profile has exactly one: Profile::game, verbatim (it may be a full path, and a
// path is never split - '|' is illegal in a Windows filename, which is precisely why
// config.h uses it as its list separator).
//
// An ALL GAMES profile (Profile::isAllGames) instead carries a PIPE-SEPARATED list of
// candidate basenames in the same field. That is not a hack, it is the mechanism that keeps
// ComputeDesired pure: see the long comment on Rule 1b below.
std::vector<std::wstring> GameSpecs(const Profile& p) {
    std::vector<std::wstring> specs;
    if (p.game.empty()) return specs;
    if (!p.isAllGames) {
        specs.push_back(p.game);
        return specs;
    }
    return Split(p.game, L'|');   // empties dropped
}

// Lowest live, non-reserved pid matching ANY of the profile's candidates, or false when the
// profile matches nothing. Lowest rather than first-seen so the choice is deterministic
// across ticks and across the two callers below.
bool LowestLiveGamePid(const ProcessSnapshot& snap, const Profile& p, DWORD& outPid) {
    outPid = 0;
    bool found = false;
    const std::vector<std::wstring> specs = GameSpecs(p);
    for (size_t s = 0; s < specs.size(); ++s) {
        const std::vector<DWORD> hits = snap.FindBySpec(specs[s]);
        for (size_t h = 0; h < hits.size(); ++h) {
            const DWORD pid = hits[h];
            if (IsReservedPid(pid)) continue;
            if (!found || pid < outPid) { outPid = pid; found = true; }
        }
    }
    if (!found) outPid = 0;
    return found;
}

bool ResolveMask(const Config& cfg, const std::wstring& name, std::vector<ULONG>& ids) {
    ids.clear();
    if (name.empty()) return true;
    const Mask* m = cfg.FindMask(name);
    if (!m) return false;
    ids = m->ids;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// ComputeDesired - pure
// ---------------------------------------------------------------------------

std::map<DWORD, std::wstring> ComputeDesired(const ProcessSnapshot& snap,
                                             const Config& cfg,
                                             DWORD foregroundPid,
                                             std::vector<std::wstring>& sticky,
                                             const Profile** matchedProfile,
                                             DWORD selfPid,
                                             std::set<DWORD>* autoPinnedOut) {
    std::map<DWORD, std::wstring> desired;
    if (matchedProfile) *matchedProfile = nullptr;
    if (autoPinnedOut) autoPinnedOut->clear();

    // --- Rule 1: the first ENABLED SPECIFIC profile whose game matches a live process. ---
    // All Games profiles are skipped here and considered only in Rule 1b, so a specific
    // profile ALWAYS wins over All Games no matter where it sits in the vector.
    const Profile* prof = nullptr;
    DWORD gamePid = 0;
    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        const Profile& p = cfg.profiles[i];
        if (!p.enabled) continue;
        if (p.isAllGames) continue;
        if (p.game.empty()) continue;
        DWORD pid = 0;
        if (!LowestLiveGamePid(snap, p, pid)) continue;
        prof = &p;
        gamePid = pid;
        break;
    }

    // --- Rule 1b: ONLY if nothing above matched, the All Games profile. ------------------
    //
    // READ THIS BEFORE ASSUMING THIS FUNCTION GREW HIDDEN I/O. It did not, and it must not.
    // ComputeDesired is still PURE: no Win32, no filesystem, no registry, no globals. It is
    // the seam the unit tests drive with synthetic snapshots.
    //
    // "Does this process look like a game?" is inherently an I/O question - it needs the
    // Steam/Epic/GOG/Xbox scan in games.h. So the answer ARRIVES AS DATA rather than being
    // computed here: an All Games profile carries a PIPE-SEPARATED list of candidate
    // executable basenames in its `game` field, and this function does nothing cleverer than
    // match a live process against that list.
    //
    // The WATCHER fills that field in - Engine::Impl::RefreshAllGamesSpec, which may call
    // Win32 - on its OWN COPY of the Config, immediately before every ComputeDesired call.
    // The user's on-disk config never carries a generated list, and a test can simply set the
    // field by hand.
    if (!prof) {
        const Profile* all = cfg.AllGamesProfile();
        if (all && all->enabled && !all->game.empty()) {
            DWORD pid = 0;
            if (LowestLiveGamePid(snap, *all, pid)) {
                prof = all;
                gamePid = pid;
            }
        }
    }

    // No profile matches: nothing is governed anywhere, and the sticky auto-pin list is
    // dropped. This is what makes "no game running means no masks anywhere" true rather
    // than merely usually true.
    if (!prof) {
        sticky.clear();
        return desired;
    }
    if (matchedProfile) *matchedProfile = prof;

    // --- Rule 2: gameSet = game pid + descendants, minus exclusions. ---------------------
    // The game ROOT is included even when its own basename is on the exclusion list: the
    // user named it as the game, which is an explicit instruction and outranks the generic
    // list. Excluded DESCENDANTS are dropped - an anti-cheat service is a child of the game
    // and belongs on the whole machine.
    std::set<DWORD> gameSet;
    gameSet.insert(gamePid);
    {
        std::vector<DWORD> fam = snap.Descendants(gamePid);
        for (size_t i = 0; i < fam.size(); ++i) {
            DWORD pid = fam[i];
            if (pid == gamePid) continue;
            if (IsReservedPid(pid)) continue;
            const ProcInfo* pi = snap.Find(pid);
            if (!pi) continue;
            if (cfg.IsExcluded(pi->name)) continue;
            gameSet.insert(pid);
        }
    }

    // --- Rule 3: heavySet, honoured even when the name is also excluded. -----------------
    std::set<DWORD> heavySet;
    for (size_t i = 0; i < prof->heavy.size(); ++i) {
        const std::wstring& spec = prof->heavy[i];
        if (spec.empty()) continue;
        std::vector<DWORD> hits = snap.FindBySpec(spec);
        for (size_t h = 0; h < hits.size(); ++h) {
            DWORD pid = hits[h];
            if (IsReservedPid(pid)) continue;
            heavySet.insert(pid);
        }
    }

    // --- Rule 4: autoSet. ----------------------------------------------------------------
    //
    // OUR OWN SUBTREE IS NEVER A CANDIDATE. GameOptimizer.exe is on the default exclusion
    // list, but an exclusion is matched by NAME and our children do not share our name: the
    // sponsor panel runs in msedgewebview2.exe, which spawns children of its own, and every
    // one of them was being auto-pinned. Adding that name to the exclusion list would spare
    // the wrong processes - the same executable hosts unrelated trees under Windows' own
    // SearchHost.exe - so the test is descent from selfPid, not the image name.
    //
    // Descendants() is reused rather than re-walked here on purpose: it is the one place
    // that knows a ppid is only an edge when the parent is live AND was created no later
    // than the child, and a second walk would be a second chance to get that wrong.
    std::set<DWORD> selfSet;
    if (selfPid != 0 && !IsReservedPid(selfPid) && snap.Find(selfPid) != nullptr) {
        const std::vector<DWORD> mine = snap.Descendants(selfPid);
        selfSet.insert(mine.begin(), mine.end());
    }

    std::set<DWORD> autoSet;
    if (prof->autoPin) {
        // FIXED DEBOUNCE, in ticks. autoPinSeconds is no longer a user setting - the seconds
        // control is gone from the UI - so the qualification is a constant number of
        // consecutive above-threshold samples (config.h::kAutoPinDebounceTicks) rather than a
        // wall-clock dwell derived from pollMs. The debounce still exists because auto-pin is
        // STICKY: firing on a single sample would let a momentary spike strand a process on
        // the background mask for the whole session.

        // New qualifiers are only ADMITTED while the foreground window belongs to the game.
        const bool gameHasForeground = (gameSet.find(foregroundPid) != gameSet.end());
        if (gameHasForeground) {
            const std::map<DWORD, ProcInfo>& all = snap.All();
            for (std::map<DWORD, ProcInfo>::const_iterator it = all.begin(); it != all.end(); ++it) {
                DWORD pid = it->first;
                if (IsReservedPid(pid)) continue;
                if (gameSet.find(pid) != gameSet.end()) continue;
                if (selfSet.find(pid) != selfSet.end()) continue;
                if (cfg.IsExcluded(it->second.name)) continue;
                if (it->second.aboveThresholdTicks < kAutoPinDebounceTicks) continue;

                // Admission is per executable because that is the unit the settings list
                // presents and rule 3 already uses. Keep the snapshot's basename verbatim;
                // FindBySpec applies the same ordinal case-insensitive matching as rule 3.
                const std::wstring& exeName = it->second.name;
                if (exeName.empty()) continue;
                bool alreadyAdmitted = false;
                for (size_t i = 0; i < sticky.size(); ++i) {
                    if (IEquals(sticky[i], exeName)) { alreadyAdmitted = true; break; }
                }
                if (!alreadyAdmitted) sticky.push_back(exeName);
            }
        }

        // Once admitted, an executable name stays sticky while the game runs, regardless of
        // current CPU%, foreground, or whether all of its processes briefly exit. Expanding
        // through the same matcher as rule 3 every tick makes later processes join without
        // attaching session state to a reusable pid. Every group member is then vetted on
        // its own: expansion must not smuggle a reserved, game, excluded, or self pid in.
        for (size_t i = 0; i < sticky.size(); ++i) {
            const std::vector<DWORD> hits = snap.FindBySpec(sticky[i]);
            for (size_t h = 0; h < hits.size(); ++h) {
                const DWORD pid = hits[h];
                if (IsReservedPid(pid)) continue;
                if (gameSet.find(pid) != gameSet.end()) continue;
                if (selfSet.find(pid) != selfSet.end()) continue;
                const ProcInfo* pi = snap.Find(pid);
                if (!pi || cfg.IsExcluded(pi->name)) continue;
                autoSet.insert(pid);
            }
        }
    } else {
        sticky.clear();
    }

    // --- Rule 5: gameSet beats heavySet beats autoSet. -----------------------------------
    for (std::set<DWORD>::const_iterator it = gameSet.begin(); it != gameSet.end(); ++it)
        desired[*it] = prof->gameMask;
    for (std::set<DWORD>::const_iterator it = heavySet.begin(); it != heavySet.end(); ++it) {
        if (gameSet.find(*it) != gameSet.end()) continue;
        desired[*it] = prof->heavyMask;
    }
    for (std::set<DWORD>::const_iterator it = autoSet.begin(); it != autoSet.end(); ++it) {
        if (gameSet.find(*it) != gameSet.end()) continue;
        if (heavySet.find(*it) != heavySet.end()) continue;
        desired[*it] = prof->heavyMask;
        // Reported HERE and not from autoSet, so the published set is exactly the pids whose
        // mask this rule actually decided. A pid autoSet also holds but gameSet or heavySet
        // won would otherwise be labelled "the app chose this" in the UI when the user did.
        if (autoPinnedOut) autoPinnedOut->insert(*it);
    }

    return desired;
}

// ---------------------------------------------------------------------------
// AutoPinnedExeNames - pure
// ---------------------------------------------------------------------------

std::vector<std::wstring> AutoPinnedExeNames(const EngineStatus& st,
                                             const std::vector<std::wstring>& alreadyListed) {
    std::set<std::wstring> skip;
    for (size_t i = 0; i < alreadyListed.size(); ++i) {
        const std::wstring key = ToLower(BaseName(Trim(alreadyListed[i])));
        if (!key.empty()) skip.insert(key);
    }

    // Keyed on the lowercased basename, valued with the casing the SNAPSHOT reported, so the
    // row reads "NVIDIA Broadcast.exe" rather than a flattened one. Sorted by the key, which
    // is what makes the order stable across ticks: a list that re-ordered itself once a
    // second would be unreadable, and this is redrawn once a second.
    std::map<std::wstring, std::wstring> byKey;
    for (size_t i = 0; i < st.governed.size(); ++i) {
        const GovernedProcess& g = st.governed[i];
        if (!g.autoPinned) continue;
        const std::wstring name = BaseName(Trim(g.name));
        if (name.empty()) continue;                   // unreadable: nothing honest to print
        const std::wstring key = ToLower(name);
        if (skip.find(key) != skip.end()) continue;
        if (byKey.find(key) == byKey.end()) byKey[key] = name;
    }

    std::vector<std::wstring> out;
    out.reserve(byKey.size());
    for (std::map<std::wstring, std::wstring>::const_iterator it = byKey.begin();
         it != byKey.end(); ++it) {
        out.push_back(it->second);
    }
    return out;
}

// ---------------------------------------------------------------------------
// BuildTooltip - pure
// ---------------------------------------------------------------------------

std::wstring BuildTooltip(const EngineStatus& st) {
    if (st.paused || !st.active)
        return ClampTip(L"Game Optimizer - idle");

    std::wstring head = st.profileName + L": " + st.gameMaskName;

    if (st.blockedCount > 0) {
        int total = st.gameProcCount + st.heavyCount;
        return ClampTip(head + L" - " + std::to_wstring(st.blockedCount)
                        + L" of " + std::to_wstring(total) + L" apps blocked");
    }
    return ClampTip(head + L" - " + std::to_wstring(st.heavyCount)
                    + L" apps on " + st.heavyMaskName);
}

// ---------------------------------------------------------------------------
// Engine::Impl
// ---------------------------------------------------------------------------

struct Engine::Impl {
    // What we believe is currently applied to a pid. `blocked` means the apply itself
    // failed, so nothing landed on that process - we keep the record anyway so the failure
    // is reported instead of silently retried four times a second.
    struct AppliedRec {
        std::wstring maskName;
        std::wstring name;
        ULONGLONG creationTime = 0;
        bool blocked = false;
        ApplyResult applyResult = ApplyResult::OtherError;
    };

    // --- guarded by mu ---------------------------------------------------------------
    mutable std::mutex mu;
    Config cfg;
    Topology topo;
    EngineStatus status;
    double lastTickMicros = 0.0;   // QPC-measured cost of the most recent tick
    bool paused = false;
    HWND notifyHwnd = nullptr;
    UINT notifyMsg = 0;

    // --- guarded by tickMu (tick-local state; TickOnce may run on a foreign thread) ----
    std::mutex tickMu;
    ProcessSnapshot prevSnap;
    bool havePrev = false;
    std::vector<std::wstring> sticky;
    std::map<DWORD, AppliedRec> applied;
    std::wstring lastProfileName;

    // Known game executables, lowercased basenames, for the All Games profile. Refreshed
    // lazily and only while an enabled All Games profile actually exists, because
    // DiscoverGames() walks the filesystem and the registry and costs tens to hundreds of
    // milliseconds. Tick-local state, so it is guarded by tickMu like everything else here.
    std::set<std::wstring> knownGameExes;
    ULONGLONG knownGamesAtMs = 0;
    bool knownGamesLoaded = false;

    // Raised when an apply is rejected as an invalid CPU Set Id, and STICKY for the rest of
    // this run. Sticky on purpose: after the first rejection the pid is recorded blocked and
    // the mask name stops changing, so nothing re-applies and a per-tick flag would clear
    // itself on the very next tick - the user would see the warning flicker once and vanish.
    // Cleared only by Stop(), i.e. by a restart after the topology has been re-detected.
    bool staleTopology = false;

    // --- thread plumbing ---------------------------------------------------------------
    HANDLE stopEvent = nullptr;   // manual-reset
    HANDLE wakeEvent = nullptr;   // auto-reset; SetConfig/SetTopology/SetPaused ring it
    std::thread worker;

    Impl() {
        status.tooltip = BuildTooltip(status);
    }

    int PollMs() const {
        int p = cfg.pollMs;
        if (p < 100) p = 100;
        if (p > 2000) p = 2000;
        return p;
    }

    int Tick();
    void WatcherLoop();
    void ClearAllApplied();   // caller holds tickMu
    void EnsureKnownGames();  // caller holds tickMu; MAY hit the filesystem and registry
    void RefreshAllGamesSpec(Config& cfgCopy, const ProcessSnapshot& snap);
};

// Populate knownGameExes from games.h. Refreshed at most once every 10 minutes: a game
// installed mid-session should eventually be recognised, but a full launcher scan on every
// 250 ms tick would be absurd. Never called unless an enabled All Games profile exists.
void Engine::Impl::EnsureKnownGames() {
    const ULONGLONG kRefreshMs = 10ULL * 60ULL * 1000ULL;
    const ULONGLONG now = GetTickCount64();
    if (knownGamesLoaded && (now - knownGamesAtMs) < kRefreshMs) return;

    std::set<std::wstring> exes;
    const std::vector<GameEntry> discovered = DiscoverGames();
    for (size_t i = 0; i < discovered.size(); ++i) {
        if (discovered[i].exe.empty()) continue;
        exes.insert(ToLower(discovered[i].exe));
    }
    // Bundled as well as discovered: a game launched from a shortcut with no launcher
    // manifest still deserves to be recognised, and BundledGames() is a static list.
    const std::vector<GameEntry>& bundled = BundledGames();
    for (size_t i = 0; i < bundled.size(); ++i) {
        if (bundled[i].exe.empty()) continue;
        exes.insert(ToLower(bundled[i].exe));
    }

    knownGameExes.swap(exes);
    knownGamesAtMs = now;
    knownGamesLoaded = true;
}

// Rewrite the All Games profile's `game` field on the WATCHER'S OWN COPY of the config, from
// the discovered game list intersected with what is actually running right now. See the long
// comment on Rule 1b in ComputeDesired: this is the Win32 half, deliberately kept out of the
// pure function.
//
// Only LIVE, non-excluded, known-game basenames go into the list, so the field stays a
// handful of entries rather than the whole catalogue, and ComputeDesired's per-candidate
// FindBySpec sweep stays cheap.
void Engine::Impl::RefreshAllGamesSpec(Config& cfgCopy, const ProcessSnapshot& snap) {
    Profile* all = nullptr;
    for (size_t i = 0; i < cfgCopy.profiles.size(); ++i) {
        if (cfgCopy.profiles[i].isAllGames) { all = &cfgCopy.profiles[i]; break; }
    }
    if (!all) return;

    // Whatever was on disk in this field is generated data, never a user's typing. Clearing
    // first means a disabled All Games profile can never match on a stale list.
    all->game.clear();
    if (!all->enabled) return;

    EnsureKnownGames();
    if (knownGameExes.empty()) return;

    std::vector<std::wstring> live;
    std::set<std::wstring> seen;
    const std::map<DWORD, ProcInfo>& procs = snap.All();
    for (std::map<DWORD, ProcInfo>::const_iterator it = procs.begin(); it != procs.end(); ++it) {
        if (IsReservedPid(it->first)) continue;
        const std::wstring& name = it->second.name;
        if (name.empty()) continue;
        const std::wstring lower = ToLower(name);
        if (seen.find(lower) != seen.end()) continue;
        if (knownGameExes.find(lower) == knownGameExes.end()) continue;
        if (cfgCopy.IsExcluded(name)) continue;
        seen.insert(lower);
        live.push_back(name);
    }
    all->game = Join(live, L'|');
}

// Clears every mask we believe we applied. The journal is truncated by the caller, so no
// per-pid JournalRemove here - that would rewrite the file once per process.
void Engine::Impl::ClearAllApplied() {
    for (std::map<DWORD, AppliedRec>::const_iterator it = applied.begin();
         it != applied.end(); ++it) {
        if (it->second.blocked) continue;   // nothing ever landed; do not spend a syscall
        ApplyOutcome oc = ClearCpuSets(it->first);
        if (oc.result != ApplyResult::Ok && oc.result != ApplyResult::Gone) {
            LogLine(L"engine: clear pid %lu failed: %s (err %lu)",
                    static_cast<unsigned long>(it->first),
                    ApplyResultName(oc.result),
                    static_cast<unsigned long>(oc.lastError));
        }
    }
    applied.clear();
}

int Engine::Impl::Tick() {
    // QPC, not GetTickCount64: a whole tick is smaller than one timer-interrupt period, so
    // the millisecond clock cannot resolve it.
    LARGE_INTEGER qpc0;
    qpc0.QuadPart = 0;
    QueryPerformanceCounter(&qpc0);

    // Immutable copies, taken once. Nothing below this block reads the shared state, so no
    // tick can observe a half-written config and the applier is never called under mu.
    Config cfgCopy;
    Topology topoCopy;
    bool pausedCopy = false;
    HWND hwnd = nullptr;
    UINT msg = 0;
    int pollMs = 250;
    {
        std::lock_guard<std::mutex> lk(mu);
        cfgCopy = cfg;
        topoCopy = topo;
        pausedCopy = paused;
        hwnd = notifyHwnd;
        msg = notifyMsg;
        pollMs = PollMs();
    }
    // Keep the copy self-consistent with the clamp the watcher actually sleeps on. The
    // auto-pin debounce no longer reads it - that is kAutoPinDebounceTicks now - but anything
    // downstream that asks the config for the poll period should get the effective value.
    cfgCopy.pollMs = pollMs;

    int totalLp = topoCopy.totalLogicalProcessors;
    if (totalLp <= 0) totalLp = GetTotalLogicalProcessors();
    if (totalLp <= 0) totalLp = 1;

    // The CPU% threshold belongs to a profile, but the snapshot needs it BEFORE the profile
    // is known. Prefer the profile matched on the previous tick; fall back to the first
    // enabled one; fall back to the documented default.
    //
    // 0 means "the CPU% rule is off this tick", and it is what lets ProcessSnapshot::Take
    // skip the per-process OpenProcess/GetProcessTimes pair for pids it already knows -
    // cpuPercent is the only consumer of cpuTime. It is only safe to pass when NO ENABLED
    // profile can auto-pin, because any enabled profile may be the one that matches on this
    // tick, and ComputeDesired reads aboveThresholdTicks for whichever one does.
    int autoPct = 0;
    {
        bool anyAutoPin = false;
        for (size_t i = 0; i < cfgCopy.profiles.size(); ++i) {
            if (cfgCopy.profiles[i].enabled && cfgCopy.profiles[i].autoPin) {
                anyAutoPin = true;
                break;
            }
        }
        if (anyAutoPin) {
            autoPct = 8;
            const Profile* pref = nullptr;
            if (!lastProfileName.empty()) pref = cfgCopy.FindProfile(lastProfileName);
            if (!pref) {
                for (size_t i = 0; i < cfgCopy.profiles.size(); ++i) {
                    if (cfgCopy.profiles[i].enabled) { pref = &cfgCopy.profiles[i]; break; }
                }
            }
            if (pref && pref->autoPinPercent > 0) autoPct = pref->autoPinPercent;
        }
    }

    ProcessSnapshot fresh;
    fresh.Take(havePrev ? &prevSnap : nullptr, pollMs, totalLp, autoPct);

    const Profile* matched = nullptr;
    std::map<DWORD, std::wstring> desired;
    std::set<DWORD> autoPinned;
    if (pausedCopy) {
        // Paused means "govern nothing". An empty desired map makes the diff below clear
        // every applied mask on this very tick, and keeps clearing it until resumed.
        sticky.clear();
    } else {
        // The Win32 half of the All Games rule, done HERE and not inside ComputeDesired, so
        // the decision function stays pure and unit-testable. No-op unless an enabled All
        // Games profile exists.
        RefreshAllGamesSpec(cfgCopy, fresh);
        // GetCurrentProcessId is the other impure half, for the same reason and by the same
        // route: rule 4 must not pin the app's own sponsor-panel browser subtree, and the
        // pure function is told which pid is ours rather than asking Win32 itself.
        desired = ComputeDesired(fresh, cfgCopy, GetForegroundPid(), sticky, &matched,
                                 GetCurrentProcessId(), &autoPinned);
    }

    // GAME DETECTION SIGNAL (operator request 9) is NOT implemented, on purpose.
    // Noticing "a live process looks like a game and no enabled profile covers it" is cheap
    // from this thread - games.h::GuessGame is exactly that test - but there is nowhere to PUT
    // the answer. EngineStatus is frozen for this round and has no field for it, and a global
    // shared between the watcher thread and the UI thread without a lock is precisely the bug
    // class this design refuses to introduce; it would be far worse than the feature being
    // late. It needs one field on EngineStatus (candidate pid + exe + display name), published
    // under the same mutex as the rest of the status, after which this is about ten lines.

    // --- diff: pids that left the desired set (including pids that exited) ---------------
    std::vector<DWORD> toClear;
    for (std::map<DWORD, AppliedRec>::const_iterator it = applied.begin();
         it != applied.end(); ++it) {
        if (desired.find(it->first) == desired.end()) toClear.push_back(it->first);
    }
    for (size_t i = 0; i < toClear.size(); ++i) {
        DWORD pid = toClear[i];
        std::map<DWORD, AppliedRec>::iterator a = applied.find(pid);
        const bool wasBlocked = (a != applied.end() && a->second.blocked);
        if (!wasBlocked) {
            ApplyOutcome oc = ClearCpuSets(pid);
            if (oc.result != ApplyResult::Ok && oc.result != ApplyResult::Gone) {
                LogLine(L"engine: clear pid %lu failed: %s (err %lu)",
                        static_cast<unsigned long>(pid),
                        ApplyResultName(oc.result),
                        static_cast<unsigned long>(oc.lastError));
            }
        }
        JournalRemove(pid);
        applied.erase(pid);
    }

    // --- diff: new pids, and pids whose desired mask NAME changed ------------------------
    // Re-issuing an identical mask every 250 ms would be pointless syscall traffic, so the
    // name comparison is the whole gate.
    for (std::map<DWORD, std::wstring>::const_iterator it = desired.begin();
         it != desired.end(); ++it) {
        const DWORD pid = it->first;
        const std::wstring& want = it->second;

        std::map<DWORD, AppliedRec>::iterator a = applied.find(pid);
        const bool isNew = (a == applied.end());
        if (!isNew && a->second.maskName == want) continue;

        std::vector<ULONG> ids;
        if (!ResolveMask(cfgCopy, want, ids)) {
            LogLine(L"engine: profile names mask '%s' which is not in the config; pid %lu left alone",
                    want.c_str(), static_cast<unsigned long>(pid));
            continue;
        }

        const ProcInfo* pi = fresh.Find(pid);
        AppliedRec rec;
        rec.maskName = want;
        rec.name = pi ? pi->name : std::wstring();
        rec.creationTime = pi ? pi->creationTime : 0;
        rec.blocked = false;

        // Written BEFORE the first apply, so a crash between here and the next line cannot
        // strand the process on half the machine.
        if (isNew) JournalAdd(pid, rec.creationTime, rec.name);

        ApplyOutcome oc = want.empty() ? ClearCpuSets(pid) : ApplyCpuSets(pid, ids);
        rec.applyResult = oc.result;
        if (oc.result == ApplyResult::Ok) {
            applied[pid] = rec;
        } else if (oc.result == ApplyResult::Gone) {
            JournalRemove(pid);
            applied.erase(pid);
        } else {
            // AccessDenied / InvalidParameter / OtherError. Recorded, never silently
            // skipped: it is counted into blockedCount and listed by name in Settings.
            rec.blocked = true;
            applied[pid] = rec;
            if (isNew) JournalRemove(pid);   // nothing landed, so nothing to recover
            LogLine(L"engine: apply pid %lu mask '%s' failed: %s (err %lu)",
                    static_cast<unsigned long>(pid), want.c_str(),
                    ApplyResultName(oc.result),
                    static_cast<unsigned long>(oc.lastError));

            // InvalidParameter from the SETTER means the CPU Set Ids themselves were
            // refused (ERROR_CPU_SET_INVALID / ERROR_INVALID_PARAMETER) - the stored ids no
            // longer describe this machine, which is what a topology change looks like from
            // here. The flag is raised for the UI; re-detection is NOT attempted from the
            // watcher thread, because silently re-deriving a user's hand-edited masks
            // behind their back is a worse outcome than telling them.
            if (oc.result == ApplyResult::InvalidParameter) {
                if (!staleTopology) {
                    LogLine(L"engine: pid %lu mask '%s' rejected as an invalid CPU Set Id "
                            L"(err %lu). The stored CPU Set Ids no longer match this "
                            L"machine; the topology needs re-detecting. Not re-detecting "
                            L"automatically - flag raised for the UI.",
                            static_cast<unsigned long>(pid), want.c_str(),
                            static_cast<unsigned long>(oc.lastError));
                }
                staleTopology = true;
            }
        }
    }

    // --- rebuild the published status ----------------------------------------------------
    EngineStatus st;
    st.paused = pausedCopy;
    st.active = (matched != nullptr);
    st.staleTopology = staleTopology;
    if (matched) {
        st.profileName = matched->name;
        st.gameMaskName = matched->gameMask;
        st.heavyMaskName = matched->heavyMask;
        // Same matcher ComputeDesired used, so an All Games profile - whose `game` is a
        // pipe-separated candidate list, not a single spec - reports the same game pid the
        // masks were actually built around. A raw FindBySpec(matched->game) would match
        // nothing at all for that profile and silently report gamePid 0.
        DWORD pid = 0;
        if (LowestLiveGamePid(fresh, *matched, pid)) st.gamePid = pid;
    }

    std::set<DWORD> gameFamily;
    if (st.gamePid != 0) {
        std::vector<DWORD> fam = fresh.Descendants(st.gamePid);
        for (size_t i = 0; i < fam.size(); ++i) gameFamily.insert(fam[i]);
        gameFamily.insert(st.gamePid);
    }

    st.governed.reserve(desired.size());
    for (std::map<DWORD, std::wstring>::const_iterator it = desired.begin();
         it != desired.end(); ++it) {
        GovernedProcess g;
        g.pid = it->first;
        g.maskName = it->second;
        const ProcInfo* pi = fresh.Find(it->first);
        if (pi) g.name = pi->name;
        g.autoPinned = (autoPinned.find(it->first) != autoPinned.end());
        std::map<DWORD, AppliedRec>::const_iterator a = applied.find(it->first);
        g.blocked = (a != applied.end() && a->second.blocked);
        g.applyResult = a != applied.end() ? a->second.applyResult
                                           : ApplyResult::OtherError;
        if (g.blocked) ++st.blockedCount;
        if (gameFamily.find(it->first) != gameFamily.end()) ++st.gameProcCount;
        else ++st.heavyCount;
        st.governed.push_back(g);
    }

    LARGE_INTEGER qpc1;
    qpc1.QuadPart = 0;
    QueryPerformanceCounter(&qpc1);
    const double micros =
        static_cast<double>(qpc1.QuadPart - qpc0.QuadPart) / QpcTicksPerMicro();

    st.lastTickMs = static_cast<int>(micros / 1000.0);
    st.tooltip = BuildTooltip(st);

    // MRU STAMPING. The watcher has just selected a profile that differs from the last one.
    //
    // The stamp itself is deliberately left to the UI. EngineStatus is frozen for this round,
    // so there is no field to publish "I stamped it" through - but the UI does not need one:
    // it already reads EngineStatus::profileName on every notify and can see this exact
    // transition itself. It is also the only side that CAN do the job, because it owns the
    // authoritative Config and the save path. Stamping cfgCopy here would be worse than
    // useless - cfgCopy is a per-tick copy, the shared cfg is overwritten wholesale by the
    // next SetConfig, and Config::MarkProfileUsed would never reach disk.
    //
    // What the watcher CAN do without a header change is make the transition visible, so a
    // missing stamp is diagnosable from the log rather than by guesswork.
    if (st.profileName != lastProfileName && !st.profileName.empty()) {
        LogLine(L"engine: profile '%s' selected (was '%s'); MRU stamp is the UI's to write",
                st.profileName.c_str(),
                lastProfileName.empty() ? L"(none)" : lastProfileName.c_str());
    }
    lastProfileName = st.profileName;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        changed = !StatusEquivalent(status, st);
        status = st;
        lastTickMicros = micros;
    }
    if (changed && hwnd != nullptr) {
        PostMessage(hwnd, msg, 0, 0);
    }

    prevSnap = fresh;
    havePrev = true;
    return st.lastTickMs;
}

void Engine::Impl::WatcherLoop() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(tickMu);
            Tick();
        }
        int p;
        {
            std::lock_guard<std::mutex> lk(mu);
            p = PollMs();
        }
        // The stop event doubles as the sleep, so Stop() is immediate instead of waiting out
        // a full period. The wake event lets SetPaused / SetConfig take effect on the next
        // instant rather than up to 2 s later, without the UI thread ever touching the
        // applier itself.
        HANDLE h[2] = { stopEvent, wakeEvent };
        DWORD w = WaitForMultipleObjects(2, h, FALSE, static_cast<DWORD>(p));
        if (w == WAIT_OBJECT_0) break;
    }
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

Engine::Engine() : impl_(new Impl()) {}

Engine::~Engine() {
    // RESOURCE RELEASE ONLY. A destructor may run at CRT teardown, where every
    // function-local static this program owns is already destroyed. Shutdown WORK - mask
    // restoration, journal truncation, logging - belongs to Stop(), which must be called
    // while the program is still running. The join is mandatory: ~std::thread on a joinable
    // thread calls std::terminate.
    if (impl_->stopEvent) SetEvent(impl_->stopEvent);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->stopEvent) { CloseHandle(impl_->stopEvent); impl_->stopEvent = nullptr; }
    if (impl_->wakeEvent) { CloseHandle(impl_->wakeEvent); impl_->wakeEvent = nullptr; }
}

void Engine::Start(HWND notifyHwnd, UINT notifyMsg) {
    if (impl_->worker.joinable()) return;   // already running; Start is not a restart

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->notifyHwnd = notifyHwnd;
        impl_->notifyMsg = notifyMsg;
    }

    if (!impl_->stopEvent) impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->wakeEvent) impl_->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->stopEvent || !impl_->wakeEvent) {
        LogLine(L"engine: CreateEvent failed (err %lu); watcher not started",
                static_cast<unsigned long>(GetLastError()));
        return;
    }
    ResetEvent(impl_->stopEvent);

    Impl* p = impl_.get();
    impl_->worker = std::thread([p]() { p->WatcherLoop(); });
}

void Engine::Stop() {
    // Fixed shutdown order: stop accepting work, join the watcher so nothing can apply
    // behind us, clear every applied mask, then truncate the journal. Clearing before the
    // join would race the watcher's own apply path.
    if (impl_->stopEvent) SetEvent(impl_->stopEvent);
    if (impl_->worker.joinable()) impl_->worker.join();

    {
        std::lock_guard<std::mutex> lk(impl_->tickMu);
        impl_->ClearAllApplied();
        impl_->sticky.clear();
        impl_->havePrev = false;
        impl_->lastProfileName.clear();
        impl_->staleTopology = false;
    }
    JournalClearAll();

    if (impl_->stopEvent) { CloseHandle(impl_->stopEvent); impl_->stopEvent = nullptr; }
    if (impl_->wakeEvent) { CloseHandle(impl_->wakeEvent); impl_->wakeEvent = nullptr; }

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->notifyHwnd = nullptr;
        impl_->notifyMsg = 0;
        EngineStatus fresh;
        fresh.paused = impl_->paused;
        fresh.tooltip = BuildTooltip(fresh);
        impl_->status = fresh;
    }
}

void Engine::SetConfig(const Config& c) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->cfg = c;            // whole immutable copy swapped in one move
    }
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

void Engine::SetTopology(const Topology& t) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->topo = t;
    }
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

void Engine::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (impl_->paused == paused) return;
        impl_->paused = paused;
    }
    // Waking the watcher is how "clears every applied mask immediately" is honoured without
    // the calling (UI) thread ever entering the applier. When no watcher is running - tests
    // and --bench - the next TickOnce does it.
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

bool Engine::IsPaused() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->paused;
}

EngineStatus Engine::GetStatus() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->status;
}

int Engine::TickOnce() {
    std::lock_guard<std::mutex> lk(impl_->tickMu);
    return impl_->Tick();
}

double Engine::LastTickMicros() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->lastTickMicros;
}

}  // namespace cd
