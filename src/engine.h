// Game Optimizer - the decision loop.
//
// Threading contract, and it is not negotiable:
//   * The UI thread owns the tray icon, the settings window, the WinEvent hook and the
//     message loop. It NEVER calls the applier.
//   * The watcher thread owns snapshots, the CPU% table and the applier. It publishes an
//     immutable EngineStatus under a mutex and posts notifyMsg to notifyHwnd; the UI
//     thread only ever reads a copy.
//   * SetConfig swaps a whole immutable Config under the same mutex, so no tick can ever
//     observe a half-written config.
//   * Shutdown order is fixed: signal stop -> JOIN THE WATCHER -> clear every applied mask ->
//     truncate the journal -> remove the tray icon.
//     The join comes BEFORE the clear on purpose. Clearing while the watcher is still alive
//     races its own apply path: it can re-apply a mask microseconds after the shutdown path
//     cleared it, stranding that process with a mask and no journal entry - the exact failure
//     the journal exists to prevent, reintroduced by the cleanup.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "applier.h"
#include "config.h"
#include "procwatch.h"
#include "profile_select.h"
#include "topology.h"

namespace cd {

struct GovernedProcess {
    DWORD pid = 0;
    std::wstring name;
    std::wstring maskName;
    bool blocked = false;        // access denied - reported, never silently skipped
    // The setter's exact outcome. The UI must not turn autoPinned (rule intent) into a
    // success claim when the applier actually refused the process.
    ApplyResult applyResult = ApplyResult::OtherError;

    // This pid is on the heavy mask because RULE 4 CHOSE IT, not because the user named it.
    // Published because a working auto-pin and a broken one are otherwise indistinguishable
    // from the UI: maskName alone says "heavy mask" for both, so a user who never sees which
    // rows the app picked reasonably concludes the rule never fired. The set is decided in
    // ComputeDesired and carried here rather than re-derived by the window, which would be a
    // second implementation of rule 4 free to disagree with the first.
    bool autoPinned = false;

    // This pid is on the heavy mask because RULE 4b - EXTREME GAME MODE - swept it there:
    // nobody named it and nothing measured it, the blanket sweep simply took everything that
    // was not the game. Published for the same reason autoPinned is, and the operator's
    // complaint was sharper: extreme mode moves ~150 processes and NOT ONE of them appeared
    // anywhere in the window, so a working sweep and a switched-off one were indistinguishable
    // - "it's 0 show". Decided in ComputeDesired (rule 5 has already given gameSet, heavySet
    // and autoSet their precedence) and carried here rather than re-derived by the window,
    // which would be a second implementation of rule 4b free to disagree with the first.
    //
    // MUTUALLY EXCLUSIVE WITH autoPinned by construction: rule 5 inserts a pid into exactly
    // one of the two published sets, so a row can never claim both explanations.
    bool extremeSwept = false;
};

struct EngineStatus {
    bool active = false;             // a profile's game is running
    bool paused = false;
    std::wstring profileName;
    std::wstring gameMaskName;
    std::wstring heavyMaskName;
    DWORD gamePid = 0;
    int gameProcCount = 0;           // game + descendants actually governed
    int heavyCount = 0;              // heavy list + auto-pinned + extreme-mode sweep
    int blockedCount = 0;
    bool staleTopology = false;      // an apply was rejected as an INVALID CPU Set Id
                                     // (ERROR_CPU_SET_INVALID, measured as 813): the stored
                                     // ids no longer match this machine and the topology
                                     // needs re-detecting. Raised by the watcher, never
                                     // acted on by it - re-detection is a user action.
    // AMD's V-Cache policy agent was running the last time a game was pinned. Re-checked on the
    // tick rather than only at startup, because the agent can appear after this app launched.
    bool amdVCacheAgentActive = false;
    int lastTickMs = 0;              // measured cost of the last tick, for --bench
    std::vector<GovernedProcess> governed;
    std::wstring tooltip;            // pre-rendered, <= 127 chars (NOTIFYICONDATA szTip)
};

// Pure decision function - no Win32 calls, no side effects. This is the seam the unit
// tests drive: given a snapshot, a config and the foreground pid, what mask name should
// each pid end up with? An empty mask name means "clear".
//
// Rules, in order:
//   1. EVERY enabled specific profile whose `game` matches a live process is a CANDIDATE,
//      and the one that owns the FOREGROUND wins - see profile_select.h for the whole of
//      that decision, including the three-second dwell that stops alt-tabbing from
//      re-pinning the machine twice a second. If none matches, the result is empty and the
//      engine clears everything.
//
//      IT USED TO BE "the first one in config-file order", and that is the bug this rule
//      was rewritten to fix: with two games running, the one further up config.ini took the
//      V-Cache mask whatever was on screen. WITH ONE GAME RUNNING THE BEHAVIOUR IS
//      IDENTICAL to what it always was, which is why this needed no new setting.
//   2. gameSet  = game pid + Descendants(game pid), minus Config::IsExcluded, minus
//      pids 0 and 4. Excluded descendants matter: an anti-cheat service IS a descendant
//      of the game and belongs on the full machine, not on the game CCD.
//   3. heavySet = every live process matching a `heavy` entry. An explicit heavy entry is
//      the user's own instruction and is honoured even if the name is also excluded.
//   4. autoSet  = when profile.autoPin AND the foreground pid is in gameSet: a qualifying
//      process admits its executable NAME, then every live FindBySpec(name) match is pinned
//      unless reserved, excluded, in gameSet, or in our own process subtree. `sticky`
//      carries admitted executable names so the group does not flap and later processes of
//      that app join automatically; an admitted name stays sticky until the game exits.
//   4b. extremeSet = EXTREME GAME MODE. Only when profile.extremeMode AND the profile has a
//      non-empty heavyMask: EVERY live process that ExtremeSweepEligible accepts also goes on
//      the heavy mask, whether it is busy or not and whether the user named it or not. This
//      is the rule that makes the game's group the game's alone; rules 3 and 4 only ever move
//      processes somebody named or something measured.
//   5. gameSet wins over heavySet wins over autoSet wins over extremeSet when a pid is in
//      more than one. Only the LABEL differs below rule 3 - heavy, auto and extreme all
//      resolve to the same heavyMask - but the label is what the UI reports, so a pid rule 4
//      chose must not be re-attributed to the blanket sweep.
//
// `selfPid` is THIS process, and passing it excludes selfPid plus every descendant from
// rule 4. MEASURED 2026-08-29: the app was auto-pinning the msedgewebview2.exe that renders
// its own sponsor panel, which is a child of GameOptimizer.exe. Excluding it BY NAME would
// have been wrong - Windows' own SearchHost.exe spawns an unrelated msedgewebview2 tree on
// this machine, and that one is a genuine background load the user may well want moved. The
// rule is PARENTAGE, resolved through ProcessSnapshot::Descendants so it inherits that
// function's pid-reuse guard rather than introducing a second tree walk with its own bugs.
// 0 means "no self process", which is what the tests pass and what pid 0 already means
// everywhere else here.
//
// `autoPinnedOut`, when non-null, receives exactly the pids whose mask in the returned map
// came from rule 4 - i.e. after gameSet and heavySet have taken precedence, so it is the set
// the UI can honestly label "the app chose this one".
//
// `extremeSweptOut`, when non-null, receives exactly the pids whose mask came from rule 4b,
// after every rule above it has taken precedence. The watcher uses it to keep the log
// readable: a blanket sweep of a whole desktop is REFUSED by every elevated process on it,
// which is the expected outcome rather than news, and one log line each would bury the
// failures that are news. Those processes are still counted and still named on the General
// page - see the blocked line there - so nothing is skipped silently.
//
// `selection`, when non-null, is the CALLER'S state for rule 1: which profile is currently
// governing, and how long a challenger has been holding the foreground. It is read and
// updated on every call, exactly as `sticky` is, and for exactly the same reason - this
// function has no statics and never will, so anything that has to survive a tick belongs to
// the watcher. Passing null is legal and means "no memory": the function then behaves as a
// single-shot chooser, which is what all but the rule-1 tests want. THE CODE PATH IS THE
// SAME either way - a null pointer is replaced by a local default-constructed state, so
// there is no second implementation of the rule to disagree with the first.
//
// The dwell length is NOT a parameter. It is derived from Config::pollMs through
// ForegroundSwitchDwellTicks, so the watcher cannot pass one number while sleeping on
// another; the watcher already overwrites its config copy's pollMs with the clamped value it
// actually sleeps on, and that is the value used here.
std::map<DWORD, std::wstring> ComputeDesired(const ProcessSnapshot& snap,
                                             const Config& cfg,
                                             DWORD foregroundPid,
                                             std::vector<std::wstring>& sticky,
                                             const Profile** matchedProfile,
                                             DWORD selfPid = 0,
                                             std::set<DWORD>* autoPinnedOut = nullptr,
                                             std::set<DWORD>* extremeSweptOut = nullptr,
                                             ProfileSelection* selection = nullptr);

// ---------------------------------------------------------------------------
// EXTREME GAME MODE - rule 4b, split out so the whole of it is one testable predicate.
// ---------------------------------------------------------------------------

// Pure. Is the blanket sweep switched on for this profile AT ALL?
//
// TWO conditions, and the second is not padding. A profile with an empty heavyMask resolves
// to "clear this process's assignment", and sweeping every process on the machine into a
// CLEAR would strip assignments this program never made - measured 2026-08-29, 49 of 289
// readable processes already carried a default CPU Set assignment with Game Optimizer not
// running. A blanket sweep to nowhere is not what "extreme game mode" means, so it is not
// what it does.
bool ExtremeSweepActive(const Profile& p);

// Pure. May the blanket sweep move THIS process? Every "no" here is a hard gate:
//
//   * pid 0 and pid 4 are never governed under any configuration.
//   * a process whose basename we could not read is NOT swept. The exclusion list is matched
//     BY NAME, so an unreadable name means the exclusion list could not be consulted - and a
//     rule that moves 200 processes must refuse the ones it cannot check rather than assume
//     they are safe. This is the one place the sweep is deliberately more cautious than
//     rules 3 and 4, which only ever look at processes somebody already named.
//   * the game's own set is not swept: it is already on the game mask and rule 5 says so.
//   * our own subtree is not swept, by PARENTAGE and not by name - see the long note on rule
//     4 below about the sponsor panel's msedgewebview2.exe children.
//   * an EXCLUDED name is not swept, and there is NO override. Rule 3 honours an explicit
//     heavy-list entry even when the name is excluded, because that is a user naming one
//     process; a blanket sweep is not a user naming anything. DefaultExclusions() exists
//     because moving those processes causes black screens, anti-cheat trips and live-audio
//     dropouts, and this rule is exactly the one that would otherwise reach all of them.
bool ExtremeSweepEligible(const Config& cfg,
                          DWORD pid,
                          const std::wstring& exeBaseName,
                          const std::set<DWORD>& gameSet,
                          const std::set<DWORD>& selfSet);

// Pure. The auto-pinned processes AS A LIST OF EXECUTABLES, which is the shape the settings
// window shows them in: distinct basenames of every GovernedProcess::autoPinned entry,
// case-insensitively de-duplicated, sorted, with anything in `alreadyListed` removed.
//
// BY EXECUTABLE AND NOT BY PID, because a browser contributes a dozen pids of one name and a
// list of pid numbers answers no question a user has. It also makes the rows the same KIND of
// thing as the manual heavy list, which is a list of executables, so the two can sit in one
// control and be compared. `alreadyListed` is that manual list: an executable the user named
// themselves must not appear twice with two different explanations.
//
// The DISPLAY CAP is deliberately not applied here. How many rows fit is a layout question
// that belongs to the window; what the set IS belongs here, next to the rule that built it.
std::vector<std::wstring> AutoPinnedExeNames(const EngineStatus& st,
                                             const std::vector<std::wstring>& alreadyListed);

// Pure. THE EXTREME-MODE SWEEP AS A LIST OF EXECUTABLES WITH THEIR PROCESS COUNTS, which is
// the shape the settings window reports them in.
//
// BY EXECUTABLE AND WITH A COUNT, and the count is what makes this different from
// AutoPinnedExeNames rather than a copy of it. Rule 4 moves a handful of processes and naming
// the app is the whole answer; rule 4b moves the desktop, and "chrome.exe" alone would hide
// that fourteen of them moved. The pair (app, how many) is the smallest honest summary of a
// blanket sweep, and it is what lets the window print a count AND a list without pasting a
// hundred pid numbers nobody can read.
//
// Sorted by COUNT DESCENDING, then by the lowercased name, so a display that can only show
// the first few shows the ones that moved the most. AutoPinnedExeNames sorts alphabetically
// because its list is short enough to show whole; this one is not, so the order has to earn
// the cap.
//
// `alreadyListed` is the user's own heavy list: an executable they named themselves is their
// configuration and must not be reported a second time as something the sweep chose. Same
// argument, same parameter, as AutoPinnedExeNames.
//
// The DISPLAY CAP is deliberately not applied here - see AutoPinnedExeNames for why.
struct SweptExe {
    std::wstring name;   // the casing the snapshot reported
    size_t count = 0;    // live processes of that executable the sweep moved
};
std::vector<SweptExe> ExtremeSweptExes(const EngineStatus& st,
                                       const std::vector<std::wstring>& alreadyListed);

// Pure. How many PROCESSES rule 4b moved, i.e. the total the list above is grouped from.
// Counted over the same GovernedProcess vector rather than summed from the grouped list,
// because a process whose name could not be read is dropped from the grouping and still
// moved - reporting a total that excludes it would under-claim what the app actually did.
size_t ExtremeSweptProcessCount(const EngineStatus& st);

// Pure. Builds the tooltip, truncated to 127 chars.
//   idle      "Game Optimizer - idle"
//   active    "Overwatch: Cache no SMT - 4 apps on Freq"
//   degraded  "Overwatch: Cache no SMT - 2 of 6 apps blocked"
std::wstring BuildTooltip(const EngineStatus& st);

class Engine {
public:
    Engine();
    // Releases this object's own resources only: signals the stop event, joins the watcher,
    // closes the handles. It touches no static, no file and no log, so it is safe to run at
    // CRT teardown. It is NOT a shutdown path.
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Spawns the watcher thread. The engine posts notifyMsg to notifyHwnd whenever the
    // status changes in a way the tray should reflect.
    void Start(HWND notifyHwnd, UINT notifyMsg);

    // Clears every applied mask, joins the watcher, truncates the journal.
    // Safe to call more than once - the second call repeats the clear and the truncate; it
    // is not a no-op. MUST be called while the program is still running: it reaches the
    // journal lock, the log lock and the config-dir string, all of which are function-local
    // statics destroyed before any namespace-scope object. Never call it from the destructor
    // of an object with static storage duration.
    void Stop();

    void SetConfig(const Config& c);
    void SetTopology(const Topology& t);
    void SetPaused(bool paused);      // clears all masks while paused
    bool IsPaused() const;

    EngineStatus GetStatus() const;

    // Runs exactly one tick on the CALLING thread, for --bench and for tests.
    // Returns the tick cost in milliseconds.
    int TickOnce();

    // Cost of the most recent tick in MICROSECONDS, from QueryPerformanceCounter.
    // The millisecond figure above cannot measure a tick at all: GetTickCount64 advances
    // once per timer interrupt (~15.6 ms here), which is larger than a whole tick, so it
    // can only ever report 0 or ~15.6 depending on whether the tick straddled one.
    double LastTickMicros() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cd
