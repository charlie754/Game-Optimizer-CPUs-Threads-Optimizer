// Game Optimizer - process discovery, the parent/child graph, CPU% sampling, foreground.
//
// See procwatch.h for the rationale behind polling rather than an event source. This TU is
// deliberately free of any tray/engine dependency so the unit-test harness can link it.
#include "procwatch.h"

#include <tlhelp32.h>

#include <climits>
#include <set>
#include <vector>

namespace cd {

namespace {

// FILETIME is not naturally aligned inside the structures we read it from, so go through
// ULARGE_INTEGER rather than reinterpret_cast<ULONGLONG*>.
inline ULONGLONG FileTimeToU64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

// Ordinal case-insensitive compare. Ordinal (not linguistic) is the right call for paths
// and image names: no locale can change the answer, and the Turkish-I problem cannot bite.
bool IEqualsOrdinal(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

// ---- Foreground tracking state ---------------------------------------------
// Written from the WinEvent callback (which runs on whichever UI thread installed the
// hook), read from the watcher thread. Interlocked, so no lock is needed on either side.
volatile LONG   g_foregroundPid = 0;
HWINEVENTHOOK   g_foregroundHook = nullptr;

void CALLBACK ForegroundWinEventProc(HWINEVENTHOOK /*hook*/,
                                     DWORD  event,
                                     HWND   hwnd,
                                     LONG   idObject,
                                     LONG   idChild,
                                     DWORD  /*idEventThread*/,
                                     DWORD  /*dwmsEventTime*/) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (hwnd == nullptr) return;
    // Only the top-level window itself; ignore the child/element notifications that share
    // this event id, otherwise a control gaining focus would be read as a foreground change.
    if (idObject != OBJID_WINDOW) return;
    if (idChild != 0 /* CHILDID_SELF */) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        InterlockedExchange(&g_foregroundPid, static_cast<LONG>(pid));
    }
}

}  // namespace

// ---- ProcessSnapshot --------------------------------------------------------

const ProcInfo* ProcessSnapshot::Find(DWORD pid) const {
    std::map<DWORD, ProcInfo>::const_iterator it = procs_.find(pid);
    if (it == procs_.end()) return nullptr;
    return &it->second;
}

bool ProcessSnapshot::Take(const ProcessSnapshot* prev, int pollMs, int totalLogicalProcessors,
                           int autoPinPercent) {
    procs_.clear();

    // Real elapsed wall clock, not the nominal poll interval. A loaded machine oversleeps,
    // and dividing a real CPU-time delta by a too-small interval inflates every cpuPercent.
    const ULONGLONG now = GetTickCount64();
    if (prev != nullptr) {
        ULONGLONG delta = (now >= prev->takenAt_) ? (now - prev->takenAt_) : 0ULL;
        if (delta == 0ULL) delta = 1ULL;                       // never divide by zero
        if (delta > static_cast<ULONGLONG>(INT_MAX)) delta = static_cast<ULONGLONG>(INT_MAX);
        elapsedMs_ = static_cast<int>(delta);
    } else {
        elapsedMs_ = pollMs;
    }
    takenAt_ = now;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (!Process32FirstW(snap, &pe)) {
        CloseHandle(snap);
        return false;
    }

    // One buffer for the whole walk; the extended-length path limit is 32767 characters.
    std::vector<wchar_t> pathBuf(32768);

    const double denomBase =
        static_cast<double>(elapsedMs_) * 10000.0 * static_cast<double>(totalLogicalProcessors);
    const bool canComputePercent = (elapsedMs_ > 0) && (totalLogicalProcessors > 0);

    // cpuTime is the ONLY volatile per-process field here, and the only thing that reads it
    // is cpuPercent, which only the auto-pin CPU% rule consumes. autoPinPercent == 0 means
    // the caller has that rule switched off for this tick, so cpuTime is not needed at all
    // and the OpenProcess/GetProcessTimes pair can be skipped for every pid we already know.
    // (A configured profile is clamped to 1..100 by ValidateAndRepair, so 0 is unambiguous.)
    const bool needCpuTime = (autoPinPercent > 0);

    do {
        ProcInfo info;
        info.pid  = pe.th32ProcessID;
        info.ppid = pe.th32ParentProcessID;
        info.name = pe.szExeFile;

        const ProcInfo* old = (prev != nullptr) ? prev->Find(info.pid) : nullptr;

        // A pid we already read successfully last tick, still reporting the same image name
        // and the same parent pid. creationTime and fullPath are IMMUTABLE for the lifetime
        // of a pid, so re-reading them every 250 ms is pure waste - and the path read is the
        // expensive one. accessDenied entries are deliberately excluded so a process that
        // becomes readable is picked up rather than being written off forever.
        const bool known = (old != nullptr)
                        && !old->accessDenied
                        && old->creationTime != 0
                        && old->ppid == info.ppid
                        && old->name == info.name;

        if (known && !needCpuTime) {
            // Zero syscalls for this process: everything this tick needs is immutable and
            // already held.
            //
            // PID-REUSE GUARD, and this is the one branch where creationTime is not
            // re-verified, so state the reasoning: for a stale creationTime to be carried
            // here the pid must have been recycled AND the replacement must report the same
            // image name AND the same parent pid as the process it replaced. That is the
            // same program relaunched by the same parent, and every downstream decision -
            // the parent/child edges in Descendants, FindBySpec, the mask a pid is given -
            // resolves identically for it. A replacement that differs in either field, which
            // is what an unrelated process inheriting the pid looks like, fails `known` and
            // goes down the OpenProcess path below where its creationTime IS re-verified.
            //
            // cpuTime is deliberately NOT carried: a stale sample divided by one tick's
            // elapsed time would read as a huge cpuPercent the moment the rule is switched
            // back on. Left at 0, which the percent block below treats as "no baseline yet".
            info.creationTime = old->creationTime;
            info.fullPath     = old->fullPath;
            info.accessDenied = old->accessDenied;   // false, by construction of `known`
        } else {
            // PROCESS_QUERY_LIMITED_INFORMATION and nothing wider. No VM_READ, no VM_WRITE,
            // no VM_OPERATION, no ALL_ACCESS: an anti-cheat watching handle rights must see
            // a request that cannot be used to read or alter the game's memory.
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.pid);
            if (h == nullptr) {
                // Keep the entry. A process we cannot read is still part of the tree, and the
                // user needs to be told it is blocked rather than have it silently vanish.
                info.accessDenied = true;
            } else {
                FILETIME ftCreate, ftExit, ftKernel, ftUser;
                if (GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    info.creationTime = FileTimeToU64(ftCreate);
                    info.cpuTime      = FileTimeToU64(ftKernel) + FileTimeToU64(ftUser);
                }

                // QueryFullProcessImageNameW dominates the cost of a tick. The path cannot
                // change under a live pid, so it is only read for a pid we have never seen
                // or one whose creationTime does NOT match the one we hold - i.e. exactly
                // the recycled-pid case, which must not inherit the old path.
                if (known && info.creationTime != 0 && info.creationTime == old->creationTime) {
                    info.fullPath = old->fullPath;
                } else {
                    DWORD chars = static_cast<DWORD>(pathBuf.size());
                    if (QueryFullProcessImageNameW(h, 0, &pathBuf[0], &chars) && chars > 0) {
                        info.fullPath.assign(&pathBuf[0], chars);
                    }
                }
                CloseHandle(h);
            }
        }

        // CARRY-FORWARD GUARD: same pid is not the same process. Only inherit CPU history
        // when creationTime matches, so a recycled pid starts clean instead of inheriting a
        // stranger's CPU time and being auto-pinned on its very first tick.
        if (old != nullptr && old->creationTime == info.creationTime) {
            info.aboveThresholdTicks = old->aboveThresholdTicks;
            // old->cpuTime == 0 means there is no usable baseline - either it was never
            // read, or the previous tick ran with the CPU% rule off. Measuring against it
            // would charge this process its whole lifetime of CPU in one tick.
            if (canComputePercent && old->cpuTime != 0 && info.cpuTime > old->cpuTime) {
                const double used = static_cast<double>(info.cpuTime - old->cpuTime);
                double pct = (used / denomBase) * 100.0;
                if (pct < 0.0)   pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                info.cpuPercent = pct;
            }
        }

        // With the rule off, no dwell accrues at all. Without this guard cpuPercent 0.0 >= 0
        // is true, so every process on the machine would bank a tick of dwell each poll and
        // the whole machine would qualify for auto-pinning the instant the rule came back on.
        if (autoPinPercent > 0 && info.cpuPercent >= static_cast<double>(autoPinPercent)) {
            ++info.aboveThresholdTicks;
        } else {
            info.aboveThresholdTicks = 0;
        }

        procs_[info.pid] = info;
    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return true;
}

std::vector<DWORD> ProcessSnapshot::FindBySpec(const std::wstring& spec) const {
    std::vector<DWORD> out;
    if (spec.empty()) return out;

    const bool byPath = (spec.find(L'\\') != std::wstring::npos);
    for (std::map<DWORD, ProcInfo>::const_iterator it = procs_.begin(); it != procs_.end(); ++it) {
        const ProcInfo& p = it->second;
        const std::wstring& field = byPath ? p.fullPath : p.name;
        if (field.empty()) continue;          // unreadable path cannot match a path spec
        if (IEqualsOrdinal(field, spec)) out.push_back(p.pid);
    }
    return out;
}

std::vector<DWORD> ProcessSnapshot::Descendants(DWORD root) const {
    std::vector<DWORD> out;

    // Build the child index once, honouring the PID-REUSE GUARD on every edge.
    //
    // THIS IS THE LOAD-BEARING LINE OF THIS FILE. Windows recycles pids aggressively on a
    // long-uptime desktop. A ppid field is just a number that was never invalidated, so an
    // unrelated process can carry the game's old pid as its parent. An edge is only real
    // when the claimed parent is still live AND was created no later than the child.
    std::map<DWORD, std::vector<DWORD> > children;
    for (std::map<DWORD, ProcInfo>::const_iterator it = procs_.begin(); it != procs_.end(); ++it) {
        const ProcInfo& child = it->second;
        if (child.ppid == 0) continue;                 // no parent claimed
        if (child.ppid == child.pid) continue;         // self-edge in a malformed table
        std::map<DWORD, ProcInfo>::const_iterator pit = procs_.find(child.ppid);
        if (pit == procs_.end()) continue;             // parent is not live: edge is stale
        if (!(pit->second.creationTime <= child.creationTime)) continue;  // parent is younger
        children[child.ppid].push_back(child.pid);
    }

    // Breadth-first from root, root included. The visited set both de-duplicates and caps
    // the walk, so a cyclic or malformed graph cannot hang the watcher thread.
    std::set<DWORD> visited;
    std::vector<DWORD> queue;
    queue.push_back(root);
    visited.insert(root);

    const size_t cap = procs_.size() + 1;
    for (size_t i = 0; i < queue.size() && out.size() < cap; ++i) {
        const DWORD cur = queue[i];
        out.push_back(cur);
        std::map<DWORD, std::vector<DWORD> >::const_iterator cit = children.find(cur);
        if (cit == children.end()) continue;
        for (size_t k = 0; k < cit->second.size(); ++k) {
            const DWORD child = cit->second[k];
            if (visited.insert(child).second) queue.push_back(child);
        }
    }
    return out;
}

// ---- Foreground tracking ----------------------------------------------------

void StartForegroundTracking() {
    if (g_foregroundHook != nullptr) return;   // already installed
    // WINEVENT_OUTOFCONTEXT: no DLL is injected into any other process, which is exactly
    // what we want next to an anti-cheat. WINEVENT_SKIPOWNPROCESS: our own tray window
    // coming forward is not a foreground game change.
    g_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                                       nullptr, &ForegroundWinEventProc, 0, 0,
                                       WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void StopForegroundTracking() {
    if (g_foregroundHook != nullptr) {
        UnhookWinEvent(g_foregroundHook);
        g_foregroundHook = nullptr;
    }
    InterlockedExchange(&g_foregroundPid, 0);
}

DWORD GetForegroundPid() {
    const LONG cached = InterlockedCompareExchange(&g_foregroundPid, 0, 0);
    if (cached != 0) return static_cast<DWORD>(cached);

    // The hook has not fired yet (we started while the game was already in front).
    HWND hwnd = GetForegroundWindow();
    if (hwnd == nullptr) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

int GetTotalLogicalProcessors() {
    // ALL_PROCESSOR_GROUPS, so a >64-thread machine is counted whole rather than one group.
    static const int kCount = []() -> int {
        const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        return (n == 0) ? 1 : static_cast<int>(n);
    }();
    return kCount;
}

}  // namespace cd
