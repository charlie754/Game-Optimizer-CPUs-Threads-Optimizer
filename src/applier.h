// Game Optimizer - the ONLY translation unit permitted to call a CPU Sets setter.
//
// HARD PRODUCT RULE, enforced by the build gate:
//   SetProcessAffinityMask must not appear anywhere in src\. CPU Sets are a scheduler
//   PREFERENCE with a legal fallback; an affinity mask is a HARD restriction that is
//   inherited by children and can leave a process with nowhere to run when the other CCD
//   is parked. "Prefer this CCD" is what a game wants; "forbid that CCD" is not.
//
// Access rights are the minimum that works, deliberately - see docs\spec\03-risks.md §2.
// PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION only. Never
// PROCESS_VM_READ / PROCESS_VM_WRITE / PROCESS_VM_OPERATION / PROCESS_ALL_ACCESS.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

// Mask, by value, for the readback classifier at the bottom of this file. It used to be
// enough to forward-declare Topology here; a function that answers "which named mask is this
// process on" needs the names themselves, so the include is now real.
#include "topology.h"

namespace cd {

enum class ApplyResult {
    Ok,
    AccessDenied,       // ERROR_ACCESS_DENIED - elevated or protected process
    Gone,               // process exited between snapshot and apply; benign
    InvalidParameter,   // ERROR_INVALID_PARAMETER - stale CPU Set Id, re-detect topology
    OtherError
};

struct ApplyOutcome {
    ApplyResult result = ApplyResult::OtherError;
    DWORD lastError = 0;
};

const wchar_t* ApplyResultName(ApplyResult r);

// Apply a set of CPU Set Ids to a process. An EMPTY ids vector clears the assignment,
// which is SetProcessDefaultCpuSets(h, NULL, 0) - the SDK annotates the pointer
// _In_reads_opt_, so NULL is the documented clear path.
ApplyOutcome ApplyCpuSets(DWORD pid, const std::vector<ULONG>& ids);
ApplyOutcome ClearCpuSets(DWORD pid);

// Read back what the OS believes this process's default CPU sets are.
// Used by Settings' verify action and by Gate B tests: never trust our own bookkeeping
// when the question is "did it actually take effect".
bool ReadCpuSets(DWORD pid, std::vector<ULONG>& out);

// Diagnostic only. The app never WRITES an affinity mask; it reads one so it can warn the
// user that something else has restricted the process and the mask will intersect badly.
bool ReadAffinityMask(DWORD pid, ULONG_PTR& processMask, ULONG_PTR& systemMask);

// ---- Inspection ------------------------------------------------------------
// What Settings' "Inspect" action can honestly report about one governed process.
//
// READ THIS BEFORE ADDING A FIELD. There are THREE outcomes for an assignment, not two:
// applied, failed, and ACCEPTED-THEN-IGNORED - and the third is invisible to both the
// setter and the getter. MEASURED on this machine (docs\spec\04-measurements.md 2.3/2.4):
// SetProcessDefaultCpuSets returned TRUE, GetProcessDefaultCpuSets echoed all 16 assigned
// ids, and not one of those 16 processors ran a single sample.
//
// So nothing in this struct observes PLACEMENT, and nothing built from it may say a mask is
// "working", "active" or "verified". Sampling GetCurrentProcessorNumberEx would require
// executing code inside the target process; this app never injects anything, and the
// undocumented routes are not used. What is left are the two OBSERVABLE PROXIES for the
// known routes into silent-ignore: assigned processors that are PARKED, and a restrictive
// AFFINITY MASK, which Microsoft documents as respected above a conflicting CPU Set
// assignment. Honest phrasing is "what was assigned, plus what might prevent it taking
// effect" - never a claim that it took effect.

struct InspectionResult {
    bool  opened = false;            // could we open the process at all
    bool  assignmentMatches = false; // readback == what we believe we assigned
    std::vector<ULONG> actualIds;    // what GetProcessDefaultCpuSets returns
    int   parkedInMask = 0;          // how many ids in the mask are currently parked
    int   totalInMask = 0;
    bool  hasRestrictiveAffinity = false; // process affinity mask != system affinity mask
    ULONG_PTR processAffinity = 0, systemAffinity = 0;
};

// Read-only. Opens with PROCESS_QUERY_LIMITED_INFORMATION and nothing wider, and writes
// nothing to the target. `topo` supplies the Parked flags - pass a freshly detected one if
// the answer is meant to be current, because parked state changes under load.
InspectionResult InspectProcess(DWORD pid, const std::vector<ULONG>& expectedIds,
                                const Topology& topo);

bool GetProcessCreationTime(DWORD pid, ULONGLONG& out);

// ---- Which mask a process is ON RIGHT NOW ----------------------------------
// The one question Settings asks every second, and it is answered by ASKING WINDOWS, never
// by repeating what this app meant to apply. Two measured reasons, both of which make our
// own bookkeeping the wrong source:
//
//   * Windows can accept an assignment and then ignore it (see InspectionResult above), so
//     "we set it" and "it is set" are different claims.
//   * WE ARE NOT THE ONLY WRITER. Measured 2026-08-29 on this machine with Game Optimizer
//     NOT RUNNING: of 377 live processes, 289 could be opened and 49 of those already
//     carried a default CPU Set assignment that nothing in this product had made.
//
// So this reports what GetProcessDefaultCpuSets says, and where that disagrees with intent
// the readback is what the user is shown. There is no state here meaning "as configured".
enum class CpuSetStage {
    NotRunning,   // nothing live matched the name at all
    NoAccess,     // live, but every process refused PROCESS_QUERY_LIMITED_INFORMATION
    AllCores,     // live, readable, NO assignment - which is precisely what "all cores" means
    Named,        // the assignment is exactly one of the masks we have a name for
    Custom,       // an assignment we do not recognise: someone else's, or a partial set
    Mixed         // several live processes and they do not agree with each other
};

struct CpuSetStageInfo {
    CpuSetStage stage = CpuSetStage::NotRunning;
    std::wstring name;   // the mask name, and ONLY when stage == Named
    int probed = 0;      // live processes looked at
    int failed = 0;      // of those, how many could not be opened or read
};

// The word to put on screen. Never a mask name unless the readback produced one.
std::wstring CpuSetStageLabel(const CpuSetStageInfo& info);

// One process's answer. `ok == false` is "we could not ask", which is NOT the same as
// "no assignment" and must never be folded into it - ids.empty() with ok == true is.
struct CpuSetReadback {
    bool ok = false;
    std::vector<ULONG> ids;
};

// Pure, and the seam the unit tests drive. One `reads` entry per live process.
//
// Failed reads are counted in `failed` and then EXCLUDED from the verdict: one protected
// child in a family of eight would otherwise turn a perfectly uniform "Cache" into "Mixed",
// which reads as a problem where there is none. When every read failed there is no verdict
// left to give and the answer is NoAccess.
CpuSetStageInfo ClassifyCpuSetStage(const std::vector<Mask>& masks,
                                    const std::vector<CpuSetReadback>& reads);

// ReadCpuSets over each pid, then ClassifyCpuSetStage. Read-only, and the only access right
// asked for is PROCESS_QUERY_LIMITED_INFORMATION - each handle is closed before the next is
// opened, so the cost is one handle at a time however long the list is.
//
// `maxProbe` bounds the work when one executable has a great many instances. MEASURED
// 2026-08-29: one full readback - OpenProcess, the two-call GetProcessDefaultCpuSets, and
// CloseHandle - costs 1.43-1.72 us, so every process on a 377-process desktop reads in
// ~0.6 ms. The cap is therefore not a performance necessity; it is a ceiling on the
// pathological case, and callers should say in a comment what they chose and why.
CpuSetStageInfo ReadCpuSetStage(const std::vector<DWORD>& pids,
                                const std::vector<Mask>& masks,
                                size_t maxProbe);

// ---- Restore journal -------------------------------------------------------
// %LOCALAPPDATA%\GameOptimizer\applied.journal, one TAB-separated line per governed process:
//     <pid>\t<creationTime>\t<exeBaseName>
// Written BEFORE the first apply to a pid. The point is that a crash, a kill, or a power
// loss cannot strand a background app on half the machine: the next launch clears whatever
// the journal lists.
//
// A malformed trailing line (an interrupted write) MUST be discarded, not treated as an
// error - otherwise the recovery path becomes the bug it exists to prevent.
struct JournalEntry {
    DWORD pid = 0;
    ULONGLONG creationTime = 0;
    std::wstring name;
};

void JournalAdd(DWORD pid, ULONGLONG creationTime, const std::wstring& name);
void JournalRemove(DWORD pid);
void JournalClearAll();
std::vector<JournalEntry> JournalRead();

// Startup recovery: for every journal entry whose pid is still live AND whose creation
// time still matches, clear its CPU sets; then truncate the journal. The creation-time
// match is what stops a RECYCLED pid from having an unrelated process cleared.
// Returns how many processes were cleared.
int RecoverFromJournal();

}  // namespace cd
