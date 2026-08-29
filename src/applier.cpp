// Game Optimizer - the ONLY translation unit permitted to call a CPU Sets setter.
//
// Everything here holds to two rules that the rest of the app depends on:
//
//   1. CPU Sets only. A hard affinity restriction is never written, not even as a
//      fallback when the CPU Sets call fails. See applier.h and docs\spec\02-architecture.md
//      section 4 for why "prefer this CCD" and "forbid that CCD" are different products.
//   2. Minimum access rights, always. PROCESS_SET_LIMITED_INFORMATION and
//      PROCESS_QUERY_LIMITED_INFORMATION are the entire set. A failure is reported and the
//      call stops; it is never retried with wider rights, because widening rights on a
//      protected process is exactly the behaviour an anti-cheat driver is looking for.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // CPU Sets APIs are Windows 10 1607+
#endif

#include "applier.h"
#include "topology.h"
#include "util.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <cstdio>

namespace cd {

namespace {

// ---- journal serialisation constants ---------------------------------------
const wchar_t kFieldSep = L'\t';
const wchar_t kLineSep = L'\n';

// ---- journal lock ----------------------------------------------------------
// The watcher thread applies and journals; the UI thread reads the journal for the
// Settings verify action and runs the shutdown clear path. Both are guarded here.
// A CRITICAL_SECTION is re-entrant on the owning thread, which is what lets
// RecoverFromJournal hold the lock while calling JournalRead / JournalClearAll.
struct JournalLock {
    CRITICAL_SECTION cs;
    JournalLock() { InitializeCriticalSection(&cs); }
    ~JournalLock() { DeleteCriticalSection(&cs); }
};

JournalLock& TheJournalLock() {
    static JournalLock lock;
    return lock;
}

class JournalGuard {
public:
    JournalGuard() : cs_(&TheJournalLock().cs) { EnterCriticalSection(cs_); }
    ~JournalGuard() { LeaveCriticalSection(cs_); }
private:
    JournalGuard(const JournalGuard&);
    JournalGuard& operator=(const JournalGuard&);
    CRITICAL_SECTION* cs_;
};

// ---- helpers ---------------------------------------------------------------

// Strictly-decimal unsigned 64-bit parse. Any non-digit, an empty string, or an
// overflow is a parse failure - the caller discards the line rather than guessing.
bool ParseU64(const std::wstring& s, ULONGLONG& out) {
    if (s.empty() || s.size() > 20) return false;
    ULONGLONG v = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c < L'0' || c > L'9') return false;
        ULONGLONG digit = static_cast<ULONGLONG>(c - L'0');
        if (v > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) return false;  // overflow
        v = v * 10ull + digit;
    }
    out = v;
    return true;
}

std::wstring U64ToW(ULONGLONG v) {
    wchar_t buf[32];
    swprintf_s(buf, 32, L"%llu", v);
    return std::wstring(buf);
}

// A tab or a newline inside the name field would make the line unparseable on the
// way back in, so they are neutralised on the way out. Real exe base names never
// contain either; this exists so a hostile or corrupt name cannot corrupt the file.
std::wstring SanitiseName(const std::wstring& name) {
    std::wstring out = name;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L'\t' || out[i] == L'\r' || out[i] == L'\n') out[i] = L'_';
    }
    return out;
}

// ERROR_CPU_SET_INVALID - "The specified CPU Set IDs are invalid."
//
// MEASURED, docs\spec\04-measurements.md 2.1: SetProcessDefaultCpuSets with an invalid Id
// returns FALSE with GetLastError() == 813, and the PREVIOUS assignment is left intact -
// the rejection is atomic, not partial. 813 is what a STALE Id looks like after a topology
// change, which is the commonest real failure here; classifying it as OtherError would stop
// the app ever knowing it should re-detect.
//
// winerror.h in Windows SDK 10.0.19041.0 does define ERROR_CPU_SET_INVALID (as 813L), but
// the literal is kept behind this guard so an older SDK still compiles. The value is the
// measured one, not an assumed one.
#ifdef ERROR_CPU_SET_INVALID
const DWORD kErrorCpuSetInvalid = ERROR_CPU_SET_INVALID;
#else
const DWORD kErrorCpuSetInvalid = 813;   // measured on this machine; see above
#endif

// Map a GetLastError value from the SETTER to an ApplyResult.
ApplyResult ClassifySetError(DWORD err) {
    switch (err) {
    case ERROR_ACCESS_DENIED:     return ApplyResult::AccessDenied;
    case ERROR_INVALID_PARAMETER: return ApplyResult::InvalidParameter;  // stale CPU Set Id
    case kErrorCpuSetInvalid:     return ApplyResult::InvalidParameter;  // 813, measured
    default:                      return ApplyResult::OtherError;
    }
}

// Map a GetLastError value from a failed OpenProcess to an ApplyResult.
// A pid that no longer exists reports ERROR_INVALID_PARAMETER from OpenProcess - that is
// the dead-process signature, and it is benign (the process exited between the snapshot
// and the apply). It is deliberately NOT reported as InvalidParameter, which in this
// enum means "our CPU Set Ids are stale, re-detect topology".
ApplyResult ClassifyOpenError(DWORD err) {
    switch (err) {
    case ERROR_INVALID_PARAMETER: return ApplyResult::Gone;
    case ERROR_NOT_FOUND:         return ApplyResult::Gone;
    case ERROR_ACCESS_DENIED:     return ApplyResult::AccessDenied;
    default:                      return ApplyResult::OtherError;
    }
}

// Serialise entries to the on-disk form: <pid>\t<creationTime>\t<name>\n
std::wstring SerialiseJournal(const std::vector<JournalEntry>& entries) {
    std::wstring text;
    for (size_t i = 0; i < entries.size(); ++i) {
        text += U64ToW(static_cast<ULONGLONG>(entries[i].pid));
        text += kFieldSep;
        text += U64ToW(entries[i].creationTime);
        text += kFieldSep;
        text += SanitiseName(entries[i].name);
        text += kLineSep;
    }
    return text;
}

// Parse the whole file. A line that does not have exactly three fields, or whose
// numeric fields do not parse, is DISCARDED - not an error. An interrupted write is
// the exact scenario this journal exists for, so the reader must survive its own
// half-written last line or the recovery path becomes the bug it was built to prevent.
std::vector<JournalEntry> ParseJournal(const std::wstring& text) {
    std::vector<JournalEntry> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(kLineSep, pos);
        std::wstring line;
        if (nl == std::wstring::npos) {
            line = text.substr(pos);
            pos = text.size();
        } else {
            line = text.substr(pos, nl - pos);
            pos = nl + 1;
        }
        if (!line.empty() && line[line.size() - 1] == L'\r') line.erase(line.size() - 1);
        if (line.empty()) continue;

        size_t t1 = line.find(kFieldSep);
        if (t1 == std::wstring::npos) continue;                 // malformed - discard
        size_t t2 = line.find(kFieldSep, t1 + 1);
        if (t2 == std::wstring::npos) continue;                 // truncated - discard
        if (line.find(kFieldSep, t2 + 1) != std::wstring::npos) continue;  // extra field

        ULONGLONG pidVal = 0;
        ULONGLONG created = 0;
        if (!ParseU64(line.substr(0, t1), pidVal)) continue;
        if (!ParseU64(line.substr(t1 + 1, t2 - t1 - 1), created)) continue;
        if (pidVal == 0 || pidVal > 0xFFFFFFFFull) continue;    // not a plausible pid

        JournalEntry e;
        e.pid = static_cast<DWORD>(pidVal);
        e.creationTime = created;
        e.name = line.substr(t2 + 1);
        out.push_back(e);
    }
    return out;
}

// Both of these assume the journal lock is already held.
std::vector<JournalEntry> JournalReadLocked() {
    std::wstring text;
    if (!ReadFileUtf8(GetJournalPath(), text)) return std::vector<JournalEntry>();
    return ParseJournal(text);
}

bool JournalWriteLocked(const std::vector<JournalEntry>& entries) {
    return WriteFileUtf8Atomic(GetJournalPath(), SerialiseJournal(entries));
}

}  // namespace

// ---- names -----------------------------------------------------------------

const wchar_t* ApplyResultName(ApplyResult r) {
    switch (r) {
    case ApplyResult::Ok:               return L"Ok";
    case ApplyResult::AccessDenied:     return L"AccessDenied";
    case ApplyResult::Gone:             return L"Gone";
    case ApplyResult::InvalidParameter: return L"InvalidParameter";
    case ApplyResult::OtherError:       return L"OtherError";
    default:                            return L"OtherError";
    }
}

// ---- apply / clear ---------------------------------------------------------

ApplyOutcome ApplyCpuSets(DWORD pid, const std::vector<ULONG>& ids) {
    ApplyOutcome outcome;

    if (pid == 0) {
        outcome.result = ApplyResult::Gone;
        outcome.lastError = ERROR_INVALID_PARAMETER;
        return outcome;
    }

    // Minimum rights. Never widened on failure - see the file header.
    HANDLE h = OpenProcess(PROCESS_SET_LIMITED_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (h == NULL) {
        DWORD err = GetLastError();
        outcome.lastError = err;
        outcome.result = ClassifyOpenError(err);
        return outcome;
    }

    // An empty set is the documented clear path: the SDK annotates the pointer
    // _In_reads_opt_, so NULL / 0 means "no default CPU sets" rather than "an empty
    // set the scheduler must honour".
    const ULONG* p = ids.empty() ? NULL : &ids[0];
    ULONG count = static_cast<ULONG>(ids.size());

    SetLastError(ERROR_SUCCESS);
    BOOL ok = SetProcessDefaultCpuSets(h, p, count);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();

    CloseHandle(h);

    if (ok) {
        outcome.result = ApplyResult::Ok;
        outcome.lastError = ERROR_SUCCESS;
    } else {
        outcome.result = ClassifySetError(err);
        outcome.lastError = err;
    }
    return outcome;
}

ApplyOutcome ClearCpuSets(DWORD pid) {
    std::vector<ULONG> none;
    return ApplyCpuSets(pid, none);
}

// ---- read-back -------------------------------------------------------------

namespace {

// The readback itself, on a handle the CALLER owns and closes. Split out of ReadCpuSets so
// the identity check and the mask read can share ONE open handle - see ReadCpuSetStage in
// applier.h for why sharing it is a correctness requirement and not merely a saving.
bool ReadCpuSetsOnHandle(HANDLE h, std::vector<ULONG>& out) {
    out.clear();

    // Two-call pattern, and the first call's contract is NOT the obvious one.
    //
    // MEASURED on this machine, 2026-08-28, raw API, no project code in the way:
    //     4 ids assigned:  GetProcessDefaultCpuSets(h, NULL, 0, &required)
    //                        -> returns FALSE, GetLastError() == 122
    //                           (ERROR_INSUFFICIENT_BUFFER), required == 4
    //     none assigned:   -> returns TRUE, required == 0
    //
    // So FALSE + ERROR_INSUFFICIENT_BUFFER is the SIZING ANSWER, not a failure. Treating it
    // as a failure - which this function did until its first caller was written - made it
    // impossible to read back any NON-EMPTY assignment, i.e. every case anyone would ask
    // about. It reported "this process has no default CPU sets" for a process that had
    // four, which is a silently wrong answer to the one question this function exists for.
    ULONG required = 0;
    SetLastError(ERROR_SUCCESS);
    if (!GetProcessDefaultCpuSets(h, NULL, 0, &required)) {
        const DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER || required == 0) return false;
    }
    if (required == 0) return true;   // empty is the normal unmanaged state

    std::vector<ULONG> buf(static_cast<size_t>(required), 0u);
    ULONG got = 0;
    if (!GetProcessDefaultCpuSets(h, &buf[0], required, &got)) {
        // The set can grow between the two calls. One retry at the newly reported size, and
        // then give up rather than loop against a process that is changing under us.
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || got <= required || got == 0)
            return false;
        required = got;
        buf.assign(static_cast<size_t>(required), 0u);
        got = 0;
        if (!GetProcessDefaultCpuSets(h, &buf[0], required, &got)) return false;
    }

    if (got > required) got = required;   // defensive; the API should never do this
    buf.resize(static_cast<size_t>(got));
    out.swap(buf);
    return true;
}

// The same split for the same reason: the creation time is read through a handle that is
// already open, so it costs no second OpenProcess and cannot be answered by a different
// process than the one the mask came from.
bool CreationTimeOnHandle(HANDLE h, ULONGLONG& out) {
    out = 0;
    FILETIME created, exited, kernelTime, userTime;
    if (!GetProcessTimes(h, &created, &exited, &kernelTime, &userTime)) return false;

    ULARGE_INTEGER u;
    u.LowPart = created.dwLowDateTime;
    u.HighPart = created.dwHighDateTime;
    out = u.QuadPart;
    return true;
}

}  // namespace

bool ReadCpuSets(DWORD pid, std::vector<ULONG>& out) {
    out.clear();
    if (pid == 0) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) return false;

    const bool ok = ReadCpuSetsOnHandle(h, out);
    CloseHandle(h);
    return ok;
}

bool ReadAffinityMask(DWORD pid, ULONG_PTR& processMask, ULONG_PTR& systemMask) {
    processMask = 0;
    systemMask = 0;
    if (pid == 0) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) return false;

    // DIAGNOSTIC ONLY. There is no writer counterpart anywhere in this program, by design.
    ULONG_PTR proc = 0, sys = 0;
    BOOL ok = GetProcessAffinityMask(h, &proc, &sys);
    CloseHandle(h);
    if (!ok) return false;

    processMask = proc;
    systemMask = sys;
    return true;
}

bool GetProcessCreationTime(DWORD pid, ULONGLONG& out) {
    out = 0;
    if (pid == 0) return false;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL) return false;

    const bool ok = CreationTimeOnHandle(h, out);
    CloseHandle(h);
    return ok;
}

bool SameProcessInstance(ULONGLONG observedCreationTime, ULONGLONG liveCreationTime) {
    // Zero means "nobody could read one", on either side, and two unknowns are not a match:
    // an absence of evidence must never leave this function as evidence of sameness. The
    // caller decides what to do with the rejection - see applier.h.
    if (observedCreationTime == 0 || liveCreationTime == 0) return false;
    return observedCreationTime == liveCreationTime;
}

// ---- which mask a process is ON RIGHT NOW ----------------------------------
//
// See applier.h for why this asks Windows instead of consulting our own bookkeeping.

std::wstring CpuSetStageLabel(const CpuSetStageInfo& info) {
    switch (info.stage) {
        // A dash, not an empty cell: a row with nothing in it reads as "not measured yet",
        // and this is a measured answer - we looked, and nothing we can vouch for is live.
        case CpuSetStage::NotRunning: return L"-";
        case CpuSetStage::NoAccess:   return L"No access";
        case CpuSetStage::AllCores:   return L"All cores";
        case CpuSetStage::Named:      return info.name;
        case CpuSetStage::Custom:     return L"Custom";
        case CpuSetStage::Mixed:      return L"Mixed";
    }
    return L"-";
}

CpuSetStageInfo ClassifyCpuSetStage(const std::vector<Mask>& masks,
                                    const std::vector<CpuSetReadback>& reads) {
    CpuSetStageInfo info;
    info.probed = static_cast<int>(reads.size());
    if (reads.empty()) {
        info.stage = CpuSetStage::NotRunning;
        return info;
    }

    // Every read is walked before any verdict is formed, so `failed` is a complete count
    // whatever the outcome. An early return on the first disagreement would leave it a
    // partial one, and a half-counted diagnostic is worse than none.
    bool haveOne = false;
    bool disagree = false;
    CpuSetStage agreed = CpuSetStage::NotRunning;
    std::wstring agreedName;

    for (size_t i = 0; i < reads.size(); ++i) {
        // The identity check comes FIRST, and it is checked even when the read succeeded: a
        // recycled pid answers perfectly well, and the answer is about a process this app has
        // never handled. Letting it through would put a stranger's mask on screen under our
        // name, or - worse, because it looks like a real finding - turn a uniform family into
        // "Mixed".
        if (!reads[i].ours) {
            ++info.notOurs;
            continue;
        }
        if (!reads[i].ok) {
            ++info.failed;
            continue;
        }

        CpuSetStage s;
        std::wstring name;
        if (reads[i].ids.empty()) {
            s = CpuSetStage::AllCores;
        } else {
            name = MaskNameForIds(masks, reads[i].ids);
            // Not "the closest mask" and not "the one we asked for": an assignment we cannot
            // name is Custom, because the only honest thing we know about it is that it is
            // not one of ours.
            s = name.empty() ? CpuSetStage::Custom : CpuSetStage::Named;
        }

        if (!haveOne) {
            haveOne = true;
            agreed = s;
            agreedName = name;
        } else if (s != agreed || name != agreedName) {
            disagree = true;
        }
    }

    if (!haveOne) {
        // Nothing of ours answered, and the two ways that happens are DIFFERENT facts:
        //   at least one of OUR processes refused -> we could not ask       -> No access
        //   every pid we had has been recycled    -> our processes are gone -> not running
        // Saying "No access" for the second would be false twice over - we could ask, and we
        // got an answer; it just was not ours to report.
        info.stage = (info.failed > 0) ? CpuSetStage::NoAccess : CpuSetStage::NotRunning;
        return info;
    }
    if (disagree) {
        info.stage = CpuSetStage::Mixed;
        return info;
    }
    info.stage = agreed;
    info.name = agreedName;
    return info;
}

namespace {

// One process, ONE handle, both questions - identity first, then the mask. Every exit closes
// the handle, including the two that give up before reading anything.
CpuSetReadback ReadCpuSetOnce(const ObservedProc& p) {
    CpuSetReadback r;   // ok == false, ours == true: "ours as far as we know, and no answer"
    if (p.pid == 0) return r;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (h == NULL) return r;   // protected, or it exited between the snapshot and now

    ULONGLONG live = 0;
    if (!CreationTimeOnHandle(h, live)) {
        CloseHandle(h);
        return r;
    }

    if (!SameProcessInstance(p.creationTime, live)) {
        // TWO rejections land here and only one of them is a recycled pid:
        //   both times known and different - the pid HAS been recycled. Live, readable, and
        //     none of our business: whatever mask it carries was put there for a process this
        //     app has never seen.
        //   either time zero - the process cannot be IDENTIFIED. In practice that is one the
        //     caller's snapshot could not open either, so it stays what it has always been,
        //     a process we got no answer out of. Never "not running": it is plainly running.
        if (p.creationTime != 0 && live != 0) r.ours = false;
        CloseHandle(h);
        return r;
    }

    r.ok = ReadCpuSetsOnHandle(h, r.ids);
    CloseHandle(h);
    return r;
}

}  // namespace

CpuSetStageInfo ReadCpuSetStage(const std::vector<ObservedProc>& procs,
                                const std::vector<Mask>& masks,
                                size_t maxProbe) {
    std::vector<CpuSetReadback> reads;
    const size_t n = (maxProbe == 0 || procs.size() < maxProbe) ? procs.size() : maxProbe;
    reads.reserve(n);
    for (size_t i = 0; i < n; ++i) reads.push_back(ReadCpuSetOnce(procs[i]));
    return ClassifyCpuSetStage(masks, reads);
}

// ---- inspection ------------------------------------------------------------
//
// Everything below reports STORED INTENT and STATIC HAZARDS. It does not, and cannot,
// report where the process is actually executing - see the contract in applier.h. The two
// hazards it can see are the two documented routes into "accepted then ignored":
// assigned processors that are parked, and a restrictive affinity mask set by something
// else. Neither being present is NOT evidence that the assignment took effect.

InspectionResult InspectProcess(DWORD pid, const std::vector<ULONG>& expectedIds,
                                const Topology& topo) {
    InspectionResult r;
    r.totalInMask = static_cast<int>(expectedIds.size());

    // Parked count is taken from the topology the caller handed in. Parked state moves, so
    // a stale Topology gives a stale count; Settings re-detects before calling.
    for (size_t i = 0; i < expectedIds.size(); ++i) {
        const CpuSetEntry* e = FindById(topo, expectedIds[i]);
        if (e != nullptr && e->Parked) ++r.parkedInMask;
    }

    if (pid == 0) return r;

    // "Could we open it at all" is asked separately so a query failure is not reported as
    // an access failure. Minimum rights, as everywhere else in this file - never widened.
    HANDLE probe = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (probe == NULL) return r;   // opened stays false: access denied, or already gone
    CloseHandle(probe);
    r.opened = true;

    std::vector<ULONG> actual;
    if (ReadCpuSets(pid, actual)) {
        r.actualIds = actual;
        // Order-insensitive: the API is not documented to preserve the order we passed in,
        // so comparing sequences directly would manufacture false mismatches.
        std::vector<ULONG> a = actual;
        std::vector<ULONG> b = expectedIds;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        r.assignmentMatches = (a == b);
    }

    ULONG_PTR pm = 0, sm = 0;
    if (ReadAffinityMask(pid, pm, sm)) {
        r.processAffinity = pm;
        r.systemAffinity = sm;
        // A process mask narrower than the system mask is the restriction that Microsoft
        // documents as respected ABOVE a conflicting CPU Set assignment. pm == 0 is not a
        // real mask - treat an implausible read as "nothing observed" rather than a warning.
        r.hasRestrictiveAffinity = (pm != 0) && (pm != sm);
    }

    return r;
}

// ---- journal ---------------------------------------------------------------

void JournalAdd(DWORD pid, ULONGLONG creationTime, const std::wstring& name) {
    if (pid == 0) return;

    JournalGuard guard;
    std::vector<JournalEntry> entries = JournalReadLocked();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].pid == pid) return;   // idempotent for a pid already present
    }

    JournalEntry e;
    e.pid = pid;
    e.creationTime = creationTime;
    e.name = name;
    entries.push_back(e);

    if (!JournalWriteLocked(entries)) {
        LogLine(L"applier: journal add failed for pid %lu (%s), err=%lu",
                static_cast<unsigned long>(pid), name.c_str(),
                static_cast<unsigned long>(GetLastError()));
    }
}

void JournalRemove(DWORD pid) {
    if (pid == 0) return;

    JournalGuard guard;
    std::vector<JournalEntry> entries = JournalReadLocked();

    std::vector<JournalEntry> kept;
    kept.reserve(entries.size());
    bool found = false;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].pid == pid) { found = true; continue; }
        kept.push_back(entries[i]);
    }
    if (!found) return;   // nothing to rewrite

    if (!JournalWriteLocked(kept)) {
        LogLine(L"applier: journal remove failed for pid %lu, err=%lu",
                static_cast<unsigned long>(pid),
                static_cast<unsigned long>(GetLastError()));
    }
}

void JournalClearAll() {
    JournalGuard guard;
    std::vector<JournalEntry> empty;
    if (!JournalWriteLocked(empty)) {
        LogLine(L"applier: journal truncate failed, err=%lu",
                static_cast<unsigned long>(GetLastError()));
    }
}

std::vector<JournalEntry> JournalRead() {
    JournalGuard guard;
    return JournalReadLocked();
}

int RecoverFromJournal() {
    JournalGuard guard;

    std::vector<JournalEntry> entries = JournalReadLocked();
    if (entries.empty()) {
        LogLine(L"applier: recovery - journal empty, nothing to clear");
        // Still truncate: an all-malformed file should not survive to the next launch.
        JournalWriteLocked(std::vector<JournalEntry>());
        return 0;
    }

    int cleared = 0;
    int skippedDead = 0;
    int skippedRecycled = 0;
    int failed = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        const JournalEntry& e = entries[i];

        ULONGLONG liveCreated = 0;
        if (!GetProcessCreationTime(e.pid, liveCreated)) {
            // Not live (or not queryable) - nothing to undo.
            ++skippedDead;
            continue;
        }
        if (liveCreated != e.creationTime) {
            // The pid has been RECYCLED. Clearing here would strip CPU sets from a
            // process this program never touched, so it is left alone deliberately.
            ++skippedRecycled;
            LogLine(L"applier: recovery - pid %lu recycled (journal %llu, live %llu), skipping %s",
                    static_cast<unsigned long>(e.pid), e.creationTime, liveCreated,
                    e.name.c_str());
            continue;
        }

        ApplyOutcome o = ClearCpuSets(e.pid);
        if (o.result == ApplyResult::Ok) {
            ++cleared;
            LogLine(L"applier: recovery - cleared pid %lu (%s)",
                    static_cast<unsigned long>(e.pid), e.name.c_str());
        } else {
            ++failed;
            LogLine(L"applier: recovery - clear FAILED pid %lu (%s): %s err=%lu",
                    static_cast<unsigned long>(e.pid), e.name.c_str(),
                    ApplyResultName(o.result),
                    static_cast<unsigned long>(o.lastError));
        }
    }

    LogLine(L"applier: recovery - %d entries: %d cleared, %d gone, %d recycled, %d failed",
            static_cast<int>(entries.size()), cleared, skippedDead, skippedRecycled, failed);

    JournalWriteLocked(std::vector<JournalEntry>());
    return cleared;
}

}  // namespace cd
