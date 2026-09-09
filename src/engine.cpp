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

#include "agent_transition.h"
#include "applier.h"
#include "apply_rules.h"
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
        // Compared for exactly the reason autoPinned is. The extreme-mode sweep churns - a
        // background app starts, an installer exits - and if only the COUNT were compared the
        // window would keep showing the set it saw when the game launched.
        if (a[i].extremeSwept != b[i].extremeSwept) return false;
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
        // Included so the tray is told when AMD's agent appears or disappears mid-session.
        // It does not jitter - it changes only when a process starts or exits - so it cannot
        // cause the per-tick message storm lastTickMs would.
        && a.amdVCacheAgentActive == b.amdVCacheAgentActive
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

bool ExtremeSweepActive(const Profile& p) {
    return p.extremeMode && !p.heavyMask.empty();
}

bool ExtremeSweepEligible(const Config& cfg,
                          DWORD pid,
                          const std::wstring& exeBaseName,
                          const std::set<DWORD>& gameSet,
                          const std::set<DWORD>& selfSet) {
    if (IsReservedPid(pid)) return false;
    // No name means the exclusion list could not be consulted. See engine.h.
    if (exeBaseName.empty()) return false;
    if (gameSet.find(pid) != gameSet.end()) return false;
    if (selfSet.find(pid) != selfSet.end()) return false;
    if (cfg.IsExcluded(exeBaseName)) return false;
    return true;
}

std::map<DWORD, std::wstring> ComputeDesired(const ProcessSnapshot& snap,
                                             const Config& cfg,
                                             DWORD foregroundPid,
                                             std::vector<std::wstring>& sticky,
                                             const Profile** matchedProfile,
                                             DWORD selfPid,
                                             std::set<DWORD>* autoPinnedOut,
                                             std::set<DWORD>* extremeSweptOut,
                                             ProfileSelection* selection) {
    std::map<DWORD, std::wstring> desired;
    if (matchedProfile) *matchedProfile = nullptr;
    if (autoPinnedOut) autoPinnedOut->clear();
    if (extremeSweptOut) extremeSweptOut->clear();

    // A caller with no state of its own gets a fresh one. ONE code path, not two: every rule
    // below runs against `sel` whether the watcher owns it or this line invented it, so a
    // stateless call cannot drift away from the stateful one.
    ProfileSelection localSel;
    ProfileSelection& sel = selection ? *selection : localSel;

    // --- Rule 1: the ENABLED SPECIFIC profile that owns the FOREGROUND. -------------------
    //
    // EVERY matching profile is gathered, not just the first. Stopping at the first one - in
    // config-file order - is exactly the defect this rule was rewritten to fix: on the
    // reference machine Palworld sits above Overwatch 2 in config.ini, so with both games
    // alive Palworld took the V-Cache mask no matter which one was on screen.
    //
    // All Games profiles are skipped here and considered only in Rule 1b, so a specific
    // profile ALWAYS wins over All Games no matter where it sits in the vector.
    std::vector<SelectionCandidate> cands;
    std::vector<const Profile*> candProf;
    std::vector<DWORD> candPid;
    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        const Profile& p = cfg.profiles[i];
        if (!p.enabled) continue;
        if (p.isAllGames) continue;
        if (p.game.empty()) continue;
        DWORD pid = 0;
        if (!LowestLiveGamePid(snap, p, pid)) continue;
        SelectionCandidate c;
        c.name = p.name;
        c.ownsForeground = false;
        cands.push_back(c);
        candProf.push_back(&p);
        candPid.push_back(pid);
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
    //
    // IT JOINS THE SAME CANDIDATE LIST rather than being chosen separately afterwards, and
    // that is deliberate. It only ever joins an EMPTY list, so the precedence is unchanged -
    // a specific profile still always wins. What it buys is that the selection state above
    // stays honest across the boundary: when a specific game later starts, the incumbent
    // named in `sel` is not in the new candidate list, so the specific profile takes over at
    // once (Released) instead of having to serve a three-second dwell it never earned.
    if (cands.empty()) {
        const Profile* all = cfg.AllGamesProfile();
        if (all && all->enabled && !all->game.empty()) {
            DWORD pid = 0;
            if (LowestLiveGamePid(snap, *all, pid)) {
                SelectionCandidate c;
                c.name = all->name;
                c.ownsForeground = false;
                cands.push_back(c);
                candProf.push_back(all);
                candPid.push_back(pid);
            }
        }
    }

    // WHOSE WINDOW IS ON SCREEN. The foreground window is very often NOT the game process
    // itself - a launcher, a child window, an anti-cheat shim or a second executable the
    // game spawned - so membership is tested against the game's whole family.
    //
    // Descendants() is reused rather than re-walked, for the same reason rule 4 reuses it:
    // it is the one place that knows a ppid is only an edge when the parent is live AND was
    // created no later than the child, and a second walk would be a second chance to get the
    // pid-reuse guard wrong.
    //
    // THE WALK IS SKIPPED ENTIRELY BELOW TWO CANDIDATES, and that is a proof rather than an
    // optimisation: with one candidate ChooseProfile returns index 0 whether or not it owns
    // the foreground - there is no incumbent to defend and no challenger to prefer - so the
    // flag cannot change the answer. ONE GAME RUNNING THEREFORE COSTS EXACTLY NOTHING, which
    // is the strongest form of "the single-game behaviour is unchanged". Test AA13 pins the
    // invariant this rests on, so the shortcut cannot outlive its own justification.
    //
    // ponytail: Descendants() rebuilds its child index on every call, so the worst case here
    // is one index build per candidate on the ticks where the foreground belongs to none of
    // them (a browser, the desktop). Measured shape: ~250 processes, and it stops at the
    // first owner. If a machine ever runs enough enabled profiles for that to show in
    // lastTickMs, the upgrade is one shared child index built per tick and passed in.
    if (cands.size() > 1 && foregroundPid != 0 && !IsReservedPid(foregroundPid)) {
        for (size_t i = 0; i < cands.size(); ++i) {
            if (candPid[i] == foregroundPid) { cands[i].ownsForeground = true; break; }
            const std::vector<DWORD> fam = snap.Descendants(candPid[i]);
            bool owns = false;
            for (size_t f = 0; f < fam.size(); ++f) {
                if (fam[f] == foregroundPid) { owns = true; break; }
            }
            if (owns) { cands[i].ownsForeground = true; break; }
        }
    }

    const int pick = ChooseProfile(cands, ForegroundSwitchDwellTicks(cfg.pollMs), sel);

    const Profile* prof = nullptr;
    DWORD gamePid = 0;
    if (pick >= 0) {
        prof = candProf[static_cast<size_t>(pick)];
        gamePid = candPid[static_cast<size_t>(pick)];
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

    // --- Rule 4b: EXTREME GAME MODE. -----------------------------------------------------
    //
    // THE ONE RULE THAT MOVES A PROCESS NOBODY NAMED AND NOTHING MEASURED. Rules 3 and 4
    // between them still leave the whole rest of the machine unmasked - measured on the
    // reference desktop, 226 live processes of which 135 had used CPU time - and every one of
    // those is free to be scheduled onto the game's own group. This rule closes that gap by
    // putting EVERY remaining process on the profile's heavy mask for as long as the profile
    // is governing.
    //
    // IT ONLY RUNS WHILE A PROFILE IS GOVERNING. That is not a check written here: control
    // never reaches this point unless `prof` matched, because the no-match path returned an
    // empty map far above. Stop the game and the diff in Engine::Impl::Tick clears every one
    // of these on the next tick, exactly as it does for rules 3 and 4.
    //
    // EVERY exclusion is honoured, default and user, with no override - see
    // ExtremeSweepEligible. That is the single most important line in this rule.
    std::set<DWORD> extremeSet;
    if (ExtremeSweepActive(*prof)) {
        const std::map<DWORD, ProcInfo>& all = snap.All();
        for (std::map<DWORD, ProcInfo>::const_iterator it = all.begin(); it != all.end(); ++it) {
            if (!ExtremeSweepEligible(cfg, it->first, it->second.name, gameSet, selfSet))
                continue;
            extremeSet.insert(it->first);
        }
    }

    // --- Rule 5: gameSet beats heavySet beats autoSet beats extremeSet. ------------------
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
    for (std::set<DWORD>::const_iterator it = extremeSet.begin(); it != extremeSet.end(); ++it) {
        if (gameSet.find(*it) != gameSet.end()) continue;
        if (heavySet.find(*it) != heavySet.end()) continue;
        if (autoSet.find(*it) != autoSet.end()) continue;
        desired[*it] = prof->heavyMask;
        // Same reasoning as autoPinnedOut, and it matters more here: a process rule 4 picked
        // on measured CPU% carries the AUTO tag the settings window shows, and re-attributing
        // it to the blanket sweep would take that tag away from the rule that earned it.
        if (extremeSweptOut) extremeSweptOut->insert(*it);
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
// ExtremeSweptExes / ExtremeSweptProcessCount - pure
// ---------------------------------------------------------------------------

namespace {

// Count-descending, then by the LOWERCASED name so the order cannot change when the same
// executable is reported with different casing by two snapshots. A stable, total order
// matters more than it looks: this list is rebuilt once a second and a tie broken by
// anything unstable would make the rows dance.
bool SweptExeLess(const SweptExe& a, const SweptExe& b) {
    if (a.count != b.count) return a.count > b.count;
    return ToLower(a.name) < ToLower(b.name);
}

}  // namespace

std::vector<SweptExe> ExtremeSweptExes(const EngineStatus& st,
                                       const std::vector<std::wstring>& alreadyListed) {
    std::set<std::wstring> skip;
    for (size_t i = 0; i < alreadyListed.size(); ++i) {
        const std::wstring key = ToLower(BaseName(Trim(alreadyListed[i])));
        if (!key.empty()) skip.insert(key);
    }

    // Keyed on the lowercased basename, valued with the casing the SNAPSHOT reported - the
    // same rule AutoPinnedExeNames follows, so "NVIDIA Broadcast.exe" reads as itself.
    std::map<std::wstring, SweptExe> byKey;
    for (size_t i = 0; i < st.governed.size(); ++i) {
        const GovernedProcess& g = st.governed[i];
        if (!g.extremeSwept) continue;
        const std::wstring name = BaseName(Trim(g.name));
        if (name.empty()) continue;                   // unreadable: nothing honest to print
        const std::wstring key = ToLower(name);
        if (skip.find(key) != skip.end()) continue;
        std::map<std::wstring, SweptExe>::iterator it = byKey.find(key);
        if (it == byKey.end()) {
            SweptExe e;
            e.name = name;
            e.count = 1;
            byKey[key] = e;
        } else {
            ++it->second.count;
        }
    }

    std::vector<SweptExe> out;
    out.reserve(byKey.size());
    for (std::map<std::wstring, SweptExe>::const_iterator it = byKey.begin();
         it != byKey.end(); ++it) {
        out.push_back(it->second);
    }
    std::stable_sort(out.begin(), out.end(), SweptExeLess);
    return out;
}

size_t ExtremeSweptProcessCount(const EngineStatus& st) {
    size_t n = 0;
    for (size_t i = 0; i < st.governed.size(); ++i)
        if (st.governed[i].extremeSwept) ++n;
    return n;
}

size_t ExtremeSweptNotAppliedCount(const EngineStatus& st) {
    size_t n = 0;
    for (size_t i = 0; i < st.governed.size(); ++i) {
        // applyResult, never `blocked`. They agree today, but `blocked` is a flag the UI
        // reads and the setter's own answer is the fact - and a row the engine never even
        // attempted carries the default OtherError with blocked still false.
        if (st.governed[i].extremeSwept &&
            st.governed[i].applyResult != ApplyResult::Ok) ++n;
    }
    return n;
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
    // What we believe about one pid. `blocked` means the apply itself failed, so nothing
    // landed on that process THIS TIME - we keep the record anyway so the failure is
    // reported instead of silently retried four times a second.
    //
    // IT HAS TO CARRY THREE FACTS THAT ARE NOT THE SAME FACT. Until v0.4.4 it carried only
    // the LATEST ATTEMPT, which made a failed re-assignment indistinguishable from "nothing
    // is assigned" - and "blocked" was then read as "there is nothing on this process",
    // which is false for any pid whose EARLIER assignment succeeded. See apply_rules.h.
    struct AppliedRec {
        // The LATEST ATTEMPT: which mask this pid was last asked to move to, and how that
        // attempt ended. The two together are the gate that stops an identical mask being
        // re-issued every tick, so they must track the attempt and not the success.
        //
        // 🔴 BOTH THE NAME AND THE IDS, AND THE IDS ARE THE HALF THAT WAS MISSING. Until
        // v0.4.4 only the name was kept, so editing mask "Cache" on the Core map and
        // keeping its name left every already-governed process on the OLD processors for
        // the life of the run - the gate compared "Cache" with "Cache" and skipped. See
        // rule 5 in apply_rules.h. Stored NORMALISED (ascending, unique) so the comparison
        // is a plain vector == on every tick that changes nothing.
        std::wstring maskName;
        std::vector<ULONG> maskIds;
        std::wstring name;
        ULONGLONG creationTime = 0;
        bool blocked = false;
        ApplyResult applyResult = ApplyResult::OtherError;

        // THE LAST SUCCESSFUL ASSIGNMENT, tracked apart from the latest attempt. A process
        // moved to Cache and then REFUSED a move to Freq is still on Cache: the attempt
        // failed, the assignment did not go away, and something has to know that before
        // this pid stops being wanted or the mask is never taken off.
        bool everApplied = false;        // a setter has succeeded for this pid at least once
        std::wstring liveMaskName;       // the mask that succeeded; empty until one does

        // Is there a recovery record on disk for this pid RIGHT NOW? The journal is the
        // only thing that can undo an assignment after an unclean exit, and this is what
        // lets a later attempt notice that an earlier failure took the record away.
        bool journalled = false;
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

    // RULE 1's memory: which profile is governing, and how long a challenger has held the
    // foreground. It lives HERE and not in ComputeDesired because that function has no
    // statics and must not grow one - see profile_select.h. Tick-local, so tickMu guards it
    // exactly as it guards `sticky`, which it sits beside for the same reason.
    ProfileSelection selection;

    // What the last completed probe said about AMD's V-Cache agent, so the tick logs the
    // transition and not the state. Starts Never: the first probe of a run is itself news.
    AgentSeen lastAgentSeen = AgentSeen::Never;

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
    // Returns the pids whose recovery record may now leave the journal: cleared, or gone.
    // A pid whose clear FAILED is not in the list and its record is KEPT - see Stop().
    std::vector<DWORD> ClearAllApplied();   // caller holds tickMu
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

// Clears every mask we believe we applied. The journal is rewritten ONCE by the caller with
// the pids this returns, so no per-pid JournalRemove here - that would rewrite the file once
// per process.
//
// IT NO LONGER CLEARS THE WHOLE MAP. A clear that FAILED leaves a live process still on our
// mask; discarding its record and then truncating the journal - which is exactly what this
// pair did until v0.4.4 - throws away the only two things that could ever undo it. Those
// records stay, so the next launch's RecoverFromJournal finds them.
std::vector<DWORD> Engine::Impl::ClearAllApplied() {
    std::vector<DWORD> done;
    std::map<DWORD, AppliedRec>::iterator it = applied.begin();
    while (it != applied.end()) {
        const DWORD pid = it->first;
        const AppliedRec& rec = it->second;

        ApplyResult clearResult = ApplyResult::Ok;   // the "there was nothing to clear" answer
        if (NeedsClearOnLeaving(rec.everApplied)) {
            ApplyOutcome oc = ClearCpuSets(pid, rec.creationTime);
            clearResult = oc.result;
            if (oc.result != ApplyResult::Ok && oc.result != ApplyResult::Gone) {
                LogLine(L"engine: shutdown clear of pid %lu failed: %s (err %lu) - mask '%s' "
                        L"is STILL APPLIED; its journal entry is kept so the next launch "
                        L"undoes it",
                        static_cast<unsigned long>(pid),
                        ApplyResultName(oc.result),
                        static_cast<unsigned long>(oc.lastError),
                        rec.liveMaskName.c_str());
            }
        }

        if (MayDropRecoveryRecord(rec.everApplied, clearResult)) {
            done.push_back(pid);
            applied.erase(it++);
        } else {
            ++it;
        }
    }
    return done;
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
    std::set<DWORD> extremeSwept;
    if (pausedCopy) {
        // Paused means "govern nothing". An empty desired map makes the diff below clear
        // every applied mask on this very tick, and keeps clearing it until resumed.
        //
        // The selection is dropped for the same reason the sticky list is: on resume the
        // game in front should be pinned AT ONCE, not three seconds later, and a stale
        // incumbent left over from before the pause would be defended by rule 4 against the
        // game that is actually on screen.
        sticky.clear();
        selection = ProfileSelection();
    } else {
        // The Win32 half of the All Games rule, done HERE and not inside ComputeDesired, so
        // the decision function stays pure and unit-testable. No-op unless an enabled All
        // Games profile exists.
        RefreshAllGamesSpec(cfgCopy, fresh);
        // GetCurrentProcessId is the other impure half, for the same reason and by the same
        // route: rule 4 must not pin the app's own sponsor-panel browser subtree, and the
        // pure function is told which pid is ours rather than asking Win32 itself.
        desired = ComputeDesired(fresh, cfgCopy, GetForegroundPid(), sticky, &matched,
                                 GetCurrentProcessId(), &autoPinned, &extremeSwept,
                                 &selection);
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
    // The pids whose recovery record may now leave the file. IT IS NOT `toClear`: a clear
    // that failed leaves the mask on a live process, and its record is the only thing that
    // can still undo it. See MayDropRecoveryRecord in apply_rules.h.
    std::vector<DWORD> clearedPids;
    for (size_t i = 0; i < toClear.size(); ++i) {
        const DWORD pid = toClear[i];
        std::map<DWORD, AppliedRec>::iterator a = applied.find(pid);
        if (a == applied.end()) continue;
        const AppliedRec& rec = a->second;

        ApplyResult clearResult = ApplyResult::Ok;   // the "there was nothing to clear" answer
        // NOT "was it blocked". A pid whose LAST attempt failed may still be carrying an
        // EARLIER mask that succeeded, and the old test skipped its clear entirely.
        if (NeedsClearOnLeaving(rec.everApplied)) {
            ApplyOutcome oc = ClearCpuSets(pid, rec.creationTime);
            clearResult = oc.result;
            if (oc.identityMismatch) {
                LogLine(L"engine: pid %lu was recycled before its mask could be cleared; "
                        L"the process behind that pid now is a stranger and is left alone",
                        static_cast<unsigned long>(pid));
            } else if (oc.result != ApplyResult::Ok && oc.result != ApplyResult::Gone) {
                LogLine(L"engine: clear pid %lu failed: %s (err %lu) - mask '%s' is STILL "
                        L"APPLIED, so its record and journal entry are kept and the next "
                        L"tick tries again",
                        static_cast<unsigned long>(pid),
                        ApplyResultName(oc.result),
                        static_cast<unsigned long>(oc.lastError),
                        rec.liveMaskName.c_str());
            }
        }

        // ponytail: a permanently un-clearable live pid is retried every tick for the life
        // of the run - one OpenProcess each. If that is ever measured as a cost, cap the
        // retries per pid; it is not capped now because giving up means abandoning a process
        // on half the machine, which is the worse failure.
        if (MayDropRecoveryRecord(rec.everApplied, clearResult)) {
            clearedPids.push_back(pid);
            applied.erase(a);
        }
    }
    // ONE rewrite for the whole batch, AFTER every clear. Per-pid JournalRemove cost 4-7 ms
    // each (see applier.h), which is invisible at the six processes rules 2-4 govern and is
    // 0.8 s of watcher thread at the ~200 extreme game mode governs - and this loop runs in
    // full the moment the game exits or the profile changes.
    //
    // AFTER the clears, not before, because that ordering IS the journal's contract: an entry
    // may only leave the file once the assignment it records is gone. Removing first and
    // crashing mid-loop would leave processes masked with nothing on disk to recover them.
    JournalRemoveMany(clearedPids);

    // --- diff: new pids, and pids whose desired mask CONTENT changed ---------------------
    // Re-issuing an identical mask every 250 ms would be pointless syscall traffic, so a
    // pid whose record already says what is wanted is skipped - but "what is wanted" is the
    // NAME AND THE PROCESSORS, never the name alone. See rule 5 in apply_rules.h for the
    // bug that cost: a mask edited in place, keeping its name, never reached a single
    // already-governed process.
    //
    // TWO PASSES SINCE EXTREME GAME MODE, and the split is a cost fix rather than a redesign.
    // The first pass decides what to do and journals EVERY new pid in one rewrite; the second
    // does the applying. The journal-before-apply guarantee is unchanged and is in fact
    // stronger - every entry is on disk before the FIRST setter call, rather than each one
    // just before its own - and the 4-7 ms per-entry cost measured in applier.h is paid once
    // instead of once per process.
    struct Pending {
        DWORD pid;
        std::wstring want;
        std::vector<ULONG> ids;
        bool needsRecord;      // a recovery record must reach disk before the setter may run
        std::wstring name;
        ULONGLONG creationTime;
    };
    std::vector<Pending> pending;
    std::vector<JournalEntry> toJournal;
    pending.reserve(desired.size());

    // THE RESOLVE HAS TO HAPPEN BEFORE THE GATE NOW, because the gate compares processors
    // and a name cannot supply them. Done naively that is one ResolveMask per DESIRED
    // PROCESS per tick - ~200 scans of the mask list every 250 ms under extreme game mode,
    // for an answer that is the same every time. Memoised per mask NAME instead, so the
    // cost is one resolve per DISTINCT mask - a governing profile names at most two, plus
    // the empty "clear" name - however many processes it governs.
    //
    // Tick-local by construction: it is declared here and dies at the end of the tick, so a
    // config edit between ticks is always seen. That is the whole point of the fix and a
    // memo that outlived the tick would re-introduce the bug it exists to close.
    struct ResolvedMask {
        bool ok = false;
        std::vector<ULONG> ids;   // normalised: ascending, no duplicates
    };
    std::map<std::wstring, ResolvedMask> resolvedByName;

    // Hoisted so the gate call below allocates nothing for a pid that has no record yet.
    const std::wstring kNoMaskName;
    const std::vector<ULONG> kNoMaskIds;

    for (std::map<DWORD, std::wstring>::const_iterator it = desired.begin();
         it != desired.end(); ++it) {
        const DWORD pid = it->first;
        const std::wstring& want = it->second;

        std::map<std::wstring, ResolvedMask>::iterator r = resolvedByName.find(want);
        if (r == resolvedByName.end()) {
            ResolvedMask rm;
            std::vector<ULONG> raw;
            rm.ok = ResolveMask(cfgCopy, want, raw);
            // NORMALISED ONCE, HERE. config.ini is hand-editable and ParseMaskValue keeps a
            // mask's ids in file order, so "8,1,3" arrives verbatim; normalising at the one
            // place they are read keeps the gate's comparison a plain vector == and makes
            // an order-only edit correctly a non-change. The setter takes these ids as a
            // SET (applier.cpp - SetProcessDefaultCpuSets), so ordering them changes what
            // is asked of Windows not at all.
            if (rm.ok) rm.ids = NormalizedMaskIds(raw);
            r = resolvedByName.insert(std::make_pair(want, rm)).first;
            // ONE line per distinct missing mask per tick, not one per process: the sweep
            // can hold two hundred pids and they would all name the same absent mask.
            if (!rm.ok) {
                LogLine(L"engine: profile names mask '%s' which is not in the config; "
                        L"the processes it would govern are left alone",
                        want.c_str());
            }
        }
        if (!r->second.ok) continue;
        const std::vector<ULONG>& wantIds = r->second.ids;

        std::map<DWORD, AppliedRec>::iterator a = applied.find(pid);
        const bool haveRecord = (a != applied.end());
        if (!NeedsReissue(haveRecord,
                          haveRecord ? a->second.maskName : kNoMaskName,
                          haveRecord ? a->second.maskIds : kNoMaskIds,
                          want, wantIds)) {
            continue;
        }

        Pending p;
        p.pid = pid;
        p.want = want;
        p.ids = wantIds;

        const ProcInfo* pi = fresh.Find(pid);
        p.name = pi ? pi->name : std::wstring();
        p.creationTime = pi ? pi->creationTime : 0;

        // 🔴 NOT "is this pid new to the map". A pid whose FIRST assignment was refused
        // keeps a map entry - blocked - and LOSES its journal entry, so `isNew` said false
        // on the next attempt and a later success was never recorded. The question the
        // journal actually asks is whether a record exists on disk.
        //
        // A record keyed on a creation time of ZERO is not a record: recovery matches on pid
        // AND creation time, so such an entry could never be matched to anything and would
        // sit in the file for ever. We do not write one - and nothing can land without one,
        // because the setter itself REFUSES a process it cannot identify (see applier.h).
        const bool recordable = (p.creationTime != 0);
        p.needsRecord = recordable &&
                        NeedsRecoveryRecord(haveRecord, haveRecord && a->second.journalled);
        pending.push_back(p);

        if (p.needsRecord) {
            JournalEntry e;
            e.pid = pid;
            e.creationTime = p.creationTime;
            e.name = p.name;
            toJournal.push_back(e);
        }
    }

    // 🔴 WRITTEN BEFORE ANY APPLY BELOW, AND THE RESULT IS OBEYED. Ordering alone was never
    // the guarantee: this call returned void until v0.4.4 and only LOGGED a failed write, so
    // a full disk still pinned every process in the batch with nothing on disk to undo them.
    // A false here holds those assignments back - see MayApplyAssignment in apply_rules.h.
    const bool journalOk = JournalAddMany(toJournal);

    // The blanket sweep's refusals, collected rather than logged one line each - see the note
    // on extremeSweptOut in engine.h.
    std::vector<std::wstring> extremeDenied;
    // Pids whose entry has to come back OUT: the process was gone, or the apply was refused
    // and NOTHING OF OURS HAS EVER LANDED ON IT. Batched like the adds, and the window that
    // opens between the failure and the removal is safe in the one direction that matters -
    // the journal briefly names a pid that carries no assignment, so a crash inside it costs
    // a redundant clear on the next launch.
    //
    // THE REVERSE MISTAKE - a masked process with no journal entry - USED TO BE REACHABLE
    // FROM HERE, in two ways, and a comment claiming otherwise is what let both survive
    // since v0.3.4. Both are now closed: an entry is only dropped when `everApplied` is
    // false, and an assignment is only attempted when its record reached disk.
    std::vector<DWORD> journalDrop;
    // Assignments not even attempted because their recovery record could not be written.
    int heldBack = 0;

    for (size_t i = 0; i < pending.size(); ++i) {
        const Pending& p = pending[i];

        if (!MayApplyAssignment(p.needsRecord, journalOk)) {
            // NOT recorded in `applied`, deliberately: nothing was attempted and nothing
            // landed, so the next tick must see this pid exactly as this one did and try
            // the journal write again. The process is left ON ALL CORES, which is the
            // machine's own default and needs no recovery record to undo.
            ++heldBack;
            continue;
        }

        std::map<DWORD, AppliedRec>::const_iterator prev = applied.find(p.pid);
        // CARRIED, NOT REBUILT. everApplied / liveMaskName / journalled describe history
        // this attempt does not erase - a failed move does not un-apply the mask that is
        // already on the process, nor delete the record that can undo it.
        AppliedRec rec;
        if (prev != applied.end()) rec = prev->second;
        rec.maskName = p.want;
        // 🔴 WITH the name, never instead of it. This is the value the next tick's gate
        // compares against a freshly resolved mask, so a mask edited in place stops looking
        // identical the moment its processors change.
        rec.maskIds = p.ids;
        rec.name = p.name;
        rec.creationTime = p.creationTime;
        rec.blocked = false;
        if (p.needsRecord) rec.journalled = true;   // the batch write above is on disk

        ApplyOutcome oc = p.want.empty() ? ClearCpuSets(p.pid, p.creationTime)
                                         : ApplyCpuSets(p.pid, p.ids, p.creationTime);
        rec.applyResult = oc.result;
        if (oc.result == ApplyResult::Ok) {
            rec.everApplied = true;
            rec.liveMaskName = p.want;
            applied[p.pid] = rec;
        } else if (oc.result == ApplyResult::Gone) {
            journalDrop.push_back(p.pid);
            applied.erase(p.pid);
        } else {
            // AccessDenied / InvalidParameter / OtherError. Recorded, never silently
            // skipped: it is counted into blockedCount and listed by name in Settings.
            rec.blocked = true;
            // ONLY when nothing of ours has ever landed on this pid. If an earlier mask
            // succeeded it is still on the process, and its record is the only thing that
            // can take it off again.
            if (!rec.everApplied) {
                journalDrop.push_back(p.pid);
                rec.journalled = false;
            }
            applied[p.pid] = rec;

            // ONE LINE PER PROCESS IS RIGHT FOR RULES 2-4 AND WRONG FOR RULE 4b. An app the
            // user named, or one the CPU% rule measured, being refused is news. A blanket
            // sweep of the whole desktop being refused by every elevated process on it is the
            // EXPECTED outcome - this app runs unelevated on purpose - and a hundred identical
            // AccessDenied lines would bury the failures that are news, in a log that rotates
            // by truncation. Those are counted here and summarised in one line below. They are
            // still counted into blockedCount and still named on the General page, so the
            // promise that nothing is skipped silently is kept where the user reads it.
            const bool sweptAndDenied =
                (oc.result == ApplyResult::AccessDenied) &&
                (extremeSwept.find(p.pid) != extremeSwept.end());
            if (sweptAndDenied) {
                extremeDenied.push_back(p.name.empty() ? std::wstring(L"(unnamed)") : p.name);
            } else {
                LogLine(L"engine: apply pid %lu mask '%s' failed: %s (err %lu)",
                        static_cast<unsigned long>(p.pid), p.want.c_str(),
                        ApplyResultName(oc.result),
                        static_cast<unsigned long>(oc.lastError));
            }

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
                            static_cast<unsigned long>(p.pid), p.want.c_str(),
                            static_cast<unsigned long>(oc.lastError));
                }
                staleTopology = true;
            }
        }
    }
    JournalRemoveMany(journalDrop);

    if (heldBack > 0) {
        // ONE LINE, AND IT IS NEWS EVERY TIME. A journal write that fails is a disk or a
        // permission problem, not an expected refusal, and the user is entitled to know that
        // the app deliberately did nothing rather than that it quietly did the wrong thing.
        LogLine(L"engine: %d assignment(s) NOT applied this tick - their recovery records "
                L"could not be written, and an assignment that cannot be undone after a "
                L"crash is not one this app will make. Those processes stay on all cores.",
                heldBack);
    }

    if (!extremeDenied.empty()) {
        // Distinct executables, so the line says what a user can act on rather than repeating
        // one name forty times. The count is of PROCESSES and the list is of APPS, and the
        // sentence says which is which.
        std::set<std::wstring> distinct;
        for (size_t i = 0; i < extremeDenied.size(); ++i) distinct.insert(extremeDenied[i]);
        std::wstring names;
        size_t shown = 0;
        for (std::set<std::wstring>::const_iterator it = distinct.begin();
             it != distinct.end() && shown < 12; ++it, ++shown) {
            if (!names.empty()) names += L", ";
            names += *it;
        }
        if (distinct.size() > shown)
            names += L" and " + std::to_wstring(distinct.size() - shown) + L" more";
        LogLine(L"engine: extreme game mode - %d processes in %d apps refused the mask "
                L"(access denied; this app runs unelevated on purpose): %s",
                static_cast<int>(extremeDenied.size()),
                static_cast<int>(distinct.size()), names.c_str());
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

    // AMD's V-Cache policy agent, RE-CHECKED HERE and not only at startup. amd3dvcacheSvc
    // launches amd3dvcacheUser.exe into the interactive session, so the agent can appear after
    // this app did and a startup-only probe would never notice. Probed ONLY while a profile's
    // game is actually governed: that is the moment two schedulers would be steering the same
    // threads, and it keeps the process snapshot off the idle path. Reuses this tick's snapshot
    // instead of taking a second one.
    //
    // THIS RAISES NOTHING. The watcher thread owns the applier and publishes an immutable
    // status; it does not own the UI and must never open a window, least of all during a game.
    // The entire visible effect is this field and one log line per transition.
    if (st.active) {
        const std::vector<DWORD> agentPids = fresh.FindBySpec(kVCacheAgentImage);
        bool agentNow = false;
        for (size_t i = 0; i < agentPids.size(); ++i) {
            if (IsProcessInOurSession(agentPids[i])) { agentNow = true; break; }
        }
        st.amdVCacheAgentActive = agentNow;
        if (ShouldLogAgentChange(lastAgentSeen, agentNow)) {
            const wchar_t* who =
                st.profileName.empty() ? L"(unnamed profile)" : st.profileName.c_str();
            if (agentNow) {
                LogLine(L"engine: AMD V-Cache agent now running while '%s' is governed", who);
            } else if (lastAgentSeen == AgentSeen::Present) {
                LogLine(L"engine: AMD V-Cache agent no longer running");
            } else {
                LogLine(L"engine: AMD V-Cache agent not running while '%s' is governed", who);
            }
        }
        lastAgentSeen = AgentSeenFrom(agentNow);
    }
    // No probe while idle, so lastAgentSeen is deliberately left alone: it records what was
    // last MEASURED, and going idle measures nothing. The published flag stays false because
    // nothing is pinned for the agent to be fighting over.

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
        // From the set rule 4b actually decided, never re-derived here. The two flags cannot
        // both be true: ComputeDesired's rule-5 loops insert each pid into at most one of the
        // published sets.
        g.extremeSwept = (extremeSwept.find(it->first) != extremeSwept.end());
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
    //
    // AND IT SAYS WHY. With two games running the profile can change because one of them
    // just started, because one of them just exited, or because the operator alt-tabbed and
    // held the other game in front for the full dwell - and those three read identically in
    // a log that only names the winner. The reason comes from ComputeDesired's own decision
    // (ProfileSelection::reason), so the log cannot describe a rule the engine did not run.
    if (st.profileName != lastProfileName && !st.profileName.empty()) {
        LogLine(L"engine: profile '%s' selected (was '%s'), reason: %s; "
                L"MRU stamp is the UI's to write",
                st.profileName.c_str(),
                lastProfileName.empty() ? L"(none)" : lastProfileName.c_str(),
                SelectReasonText(selection.reason));
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

    std::vector<DWORD> cleared;
    size_t stillApplied = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->tickMu);
        cleared = impl_->ClearAllApplied();
        // Read under the lock that guards it, like everything else in this block. The
        // watcher is joined by now, but a count sampled outside its own lock is a habit
        // this file does not have.
        stillApplied = impl_->applied.size();
        impl_->sticky.clear();
        impl_->havePrev = false;
        impl_->lastProfileName.clear();
        impl_->staleTopology = false;
        // Rule 1's memory is session state like the rest of this block, and leaving it
        // behind would let a reused Engine object defend an incumbent from the last run.
        impl_->selection = ProfileSelection();
    }
    // 🔴 TRUNCATE ONLY WHEN THERE IS PROVABLY NOTHING LEFT TO RECOVER. This used to be an
    // unconditional JournalClearAll(), which threw away the recovery record of every process
    // whose clear had just FAILED - the one case where the file is the only thing standing
    // between the user and a process pinned to half the machine until they notice.
    if (stillApplied == 0) {
        JournalClearAll();
    } else {
        JournalRemoveMany(cleared);
        LogLine(L"engine: shutdown left %d assignment(s) that could not be cleared; their "
                L"journal entries are kept so the next launch undoes them",
                static_cast<int>(stillApplied));
    }

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
