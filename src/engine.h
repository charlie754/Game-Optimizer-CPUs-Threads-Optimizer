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
#include <string>
#include <vector>
#include "config.h"
#include "procwatch.h"
#include "topology.h"

namespace cd {

struct GovernedProcess {
    DWORD pid = 0;
    std::wstring name;
    std::wstring maskName;
    bool blocked = false;        // access denied - reported, never silently skipped
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
//   4. autoSet  = when profile.autoPin AND the foreground pid is in gameSet: every process
//      with aboveThresholdTicks * pollMs >= autoPinSeconds*1000, not excluded, not in
//      gameSet. `sticky` carries pids already auto-pinned this session so they do not
//      flap; a pinned process stays pinned until the game exits.
//   5. gameSet wins over heavySet wins over autoSet when a pid is in more than one.
std::map<DWORD, std::wstring> ComputeDesired(const ProcessSnapshot& snap,
                                             const Config& cfg,
                                             DWORD foregroundPid,
                                             std::vector<DWORD>& sticky,
                                             const Profile** matchedProfile);

// Pure. Builds the tooltip, truncated to 127 chars.
//   idle      "Game Optimizer - idle"
//   active    "Overwatch: Cache no SMT - 4 apps on Freq"
//   degraded  "Overwatch: Cache no SMT - 2 of 6 apps blocked"
std::wstring BuildTooltip(const EngineStatus& st);

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Spawns the watcher thread. The engine posts notifyMsg to notifyHwnd whenever the
    // status changes in a way the tray should reflect.
    void Start(HWND notifyHwnd, UINT notifyMsg);

    // Clears every applied mask, joins the watcher, truncates the journal. Idempotent.
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
