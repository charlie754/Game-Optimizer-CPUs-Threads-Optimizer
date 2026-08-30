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
};

struct EngineStatus {
    bool active = false;             // a profile's game is running
    bool paused = false;
    std::wstring profileName;
    std::wstring gameMaskName;
    std::wstring heavyMaskName;
    DWORD gamePid = 0;
    int gameProcCount = 0;           // game + descendants actually governed
    int heavyCount = 0;              // heavy list + auto-pinned
    int blockedCount = 0;
    bool staleTopology = false;      // an apply was rejected as an INVALID CPU Set Id
                                     // (ERROR_CPU_SET_INVALID, measured as 813): the stored
                                     // ids no longer match this machine and the topology
                                     // needs re-detecting. Raised by the watcher, never
                                     // acted on by it - re-detection is a user action.
    int lastTickMs = 0;              // measured cost of the last tick, for --bench
    std::vector<GovernedProcess> governed;
    std::wstring tooltip;            // pre-rendered, <= 127 chars (NOTIFYICONDATA szTip)
};

// Pure decision function - no Win32 calls, no side effects. This is the seam the unit
// tests drive: given a snapshot, a config and the foreground pid, what mask name should
// each pid end up with? An empty mask name means "clear".
//
// Rules, in order:
//   1. The first ENABLED profile whose `game` matches a live process wins. If none
//      matches, the result is empty and the engine clears everything.
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
//   5. gameSet wins over heavySet wins over autoSet when a pid is in more than one.
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
std::map<DWORD, std::wstring> ComputeDesired(const ProcessSnapshot& snap,
                                             const Config& cfg,
                                             DWORD foregroundPid,
                                             std::vector<std::wstring>& sticky,
                                             const Profile** matchedProfile,
                                             DWORD selfPid = 0,
                                             std::set<DWORD>* autoPinnedOut = nullptr);

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
