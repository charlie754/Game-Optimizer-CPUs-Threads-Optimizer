// Game Optimizer - process discovery, the parent/child graph, CPU% sampling, foreground.
//
// Why polling and not an event source: WMI __InstanceCreationEvent has 1-2 s latency,
// Win32_ProcessStartTrace and the ETW kernel process provider both require administrator,
// and elevation is a product non-goal. A TH32CS_SNAPPROCESS snapshot at 250 ms bounds the
// exposure window for a newly spawned child to one tick without any privilege at all.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <string>
#include <vector>

namespace cd {

struct ProcInfo {
    DWORD pid = 0;
    DWORD ppid = 0;
    std::wstring name;             // basename, e.g. "Overwatch.exe"
    std::wstring fullPath;         // empty when it could not be read (access denied)
    ULONGLONG creationTime = 0;    // FILETIME as ULONGLONG; 0 when unreadable
    ULONGLONG cpuTime = 0;         // kernel+user, 100ns units, 0 when unreadable
    double cpuPercent = 0.0;       // machine-wide %, i.e. 100.0 means every logical processor
    int  aboveThresholdTicks = 0;  // consecutive ticks at or above the auto-pin threshold
    bool accessDenied = false;     // OpenProcess for QUERY_LIMITED_INFORMATION failed
};

class ProcessSnapshot {
public:
    // Take a snapshot of every process. When `prev` is non-null, cpuPercent is computed
    // from the CPU-time delta over pollMs and aboveThresholdTicks is carried forward for
    // pids that persist. Pass the machine's total logical processor count so cpuPercent
    // uses Task Manager's convention (100% == the whole machine, not one core).
    //
    // A pid is only carried forward from `prev` when its creationTime matches, so a
    // RECYCLED pid starts its CPU history from scratch instead of inheriting a stranger's.
    bool Take(const ProcessSnapshot* prev, int pollMs, int totalLogicalProcessors,
              int autoPinPercent);

    const std::map<DWORD, ProcInfo>& All() const { return procs_; }
    const ProcInfo* Find(DWORD pid) const;
    size_t Count() const { return procs_.size(); }

    // Match a profile's `game` or `heavy` entry. When `spec` contains a backslash it is
    // matched against fullPath, otherwise against name. Case-insensitive either way.
    std::vector<DWORD> FindBySpec(const std::wstring& spec) const;

    // `root` plus every transitive descendant.
    //
    // PID-REUSE GUARD, and it is load-bearing: a ppid is honoured only when the parent is
    // live AND parent.creationTime <= child.creationTime. Without it, a long-uptime desktop
    // recycles a pid, an unrelated process inherits the game's ppid, and something random
    // gets pinned to the game CCD.
    std::vector<DWORD> Descendants(DWORD root) const;

    // Wall-clock milliseconds actually elapsed since `prev` was taken. Used instead of the
    // nominal pollMs when computing cpuPercent, because a loaded machine oversleeps.
    int ElapsedMs() const { return elapsedMs_; }

private:
    std::map<DWORD, ProcInfo> procs_;
    ULONGLONG takenAt_ = 0;
    int elapsedMs_ = 0;
};

// ---- Foreground tracking ---------------------------------------------------
// SetWinEventHook(EVENT_SYSTEM_FOREGROUND, WINEVENT_OUTOFCONTEXT) - no elevation, no
// injection. Install and remove on the UI thread only.
void  StartForegroundTracking();
void  StopForegroundTracking();
// The hooked value; falls back to GetForegroundWindow() when the hook has not fired yet.
DWORD GetForegroundPid();

// Total logical processors across all groups, cached.
int GetTotalLogicalProcessors();

}  // namespace cd
