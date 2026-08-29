// Game Optimizer Gate B harness - does the product actually work against the live OS?
//
// WHY THIS FILE EXISTS, and it determines every line below.
//
// [M] docs\spec\04-measurements.md 2.3/2.4, measured on this machine: assigning a set made
// entirely of PARKED processors produced SetProcessDefaultCpuSets -> TRUE,
// GetProcessDefaultCpuSets reading back all 16 requested ids, and NOT ONE of those 16
// processors running a single sample. Both the setter and the getter reported complete
// success for an assignment the scheduler ignored entirely.
//
//     *** VERIFYING BY READING THE ASSIGNMENT BACK IS WORTHLESS. ***
//
// So every placement claim here is made by sampling GetCurrentProcessorNumberEx INSIDE the
// governed process while it runs busy threads, and asserting the observed processor set is
// a SUBSET of the assigned mask. GetProcessDefaultCpuSets is called in exactly one place -
// GATE_B4 - where the claim genuinely is about what is ASSIGNED rather than about placement.
//
// GATE_B4's claim is "Game Optimizer left no residue", NOT "this machine is pristine". The
// difference is not pedantry: the machine-wide form of that check is measurably false here
// for a reason that has nothing to do with this product. See the governed-pid ledger above
// ChildProc for the measurement and for what the check is now scoped to.
//
// Every check also carries a NEGATIVE CONTROL, because a subset assertion passes vacuously
// on an empty histogram, on a machine with two usable processors, and on a confinement that
// was not caused by us. A check that cannot fail is not evidence.
//
// SetProcessAffinityMask is never called. PROCESS_VM_* / PROCESS_ALL_ACCESS are never
// requested - the applier uses PROCESS_SET_LIMITED_INFORMATION |
// PROCESS_QUERY_LIMITED_INFORMATION and nothing here widens that.
//
// Modes:
//   --worker <hist> <sec> [--settle <ms>] [--spawn-grandchild <hist2> <sec2>]
//                         [--grandchild-exe <path>] [--phase2 <hist2> <sec2>]
//                         [--gate <file>]
//   --selftest   (default)
//   --version
//
// The optional worker flags are measurement windows, not relaxations:
//   --settle          delays the START of sampling, because architecture 8 Gate B item 2
//                     grants the watcher ONE poll period to notice a new process. The
//                     negative controls use the SAME settle, which is what proves the
//                     settle cannot make a check pass by itself.
//   --grandchild-exe  gives the grandchild a different executable NAME from the child, so
//                     the engine's game-root rule cannot select the grandchild. See
//                     MakeExeCopy - this made GATE_B2 stricter, not looser.
//   --gate            blocks between phase 1 and phase 2 so GATE_B3's post-recovery sample
//                     is guaranteed to be after recovery rather than probably after.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "applier.h"
#include "config.h"
#include "engine.h"
#include "procwatch.h"
#include "topology.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Shared: histogram of processors actually observed
// ---------------------------------------------------------------------------
//
// Key is Group * 64 + Number, exactly as PROCESSOR_NUMBER reports it. 1024 slots covers
// 16 processor groups; this machine has one.

static const size_t kHistSlots = 1024;

static std::atomic<bool> g_run(false);
static std::vector<std::atomic<long> > g_hist(kHistSlots);

typedef std::map<unsigned, long long> HistMap;

static std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

static std::string N(const std::wstring& s) { return cd::Narrow(s); }

// ---------------------------------------------------------------------------
// Worker mode - the SUBJECT of every measurement. It is deliberately passive:
// it sets no CPU sets and no affinity of its own, so anything observed about its
// placement was done to it from outside.
// ---------------------------------------------------------------------------

static void BurnThread() {
    while (!g_run.load(std::memory_order_acquire)) Sleep(0);
    volatile double x = 1.0;
    while (g_run.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 20000; ++i) x = x * 1.0000001 + 0.0000001;
        PROCESSOR_NUMBER pn;
        ZeroMemory(&pn, sizeof(pn));
        GetCurrentProcessorNumberEx(&pn);
        unsigned idx = static_cast<unsigned>(pn.Group) * 64u + static_cast<unsigned>(pn.Number);
        if (idx < kHistSlots) g_hist[idx].fetch_add(1, std::memory_order_relaxed);
    }
    (void)x;
}

// 8 busy threads for `ms`, sampling into the (freshly zeroed) global histogram.
static void RunBusyPhase(DWORD ms) {
    for (size_t i = 0; i < kHistSlots; ++i) g_hist[i].store(0, std::memory_order_relaxed);
    g_run.store(false, std::memory_order_relaxed);

    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i) ts.push_back(std::thread(BurnThread));
    Sleep(30);
    g_run.store(true, std::memory_order_release);
    Sleep(ms);
    g_run.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < ts.size(); ++i) ts[i].join();
}

// Written through util's tmp + MoveFileEx replace, so the file only APPEARS once it is
// complete. The selftest sequences phases on that appearance, so a torn read would be a
// silent sequencing bug rather than a visible one.
static bool WriteHistogram(const std::wstring& path) {
    std::wstring text;
    for (size_t i = 0; i < kHistSlots; ++i) {
        long v = g_hist[i].load(std::memory_order_relaxed);
        if (v <= 0) continue;
        text += std::to_wstring(static_cast<unsigned long long>(i));
        text += L" ";
        text += std::to_wstring(static_cast<long long>(v));
        text += L"\n";
    }
    return cd::WriteFileUtf8Atomic(path, text);
}

static bool ReadHistogram(const std::wstring& path, HistMap& out) {
    out.clear();
    std::wstring text;
    if (!cd::ReadFileUtf8(path, text)) return false;
    std::vector<std::wstring> lines = cd::Split(text, L'\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring line = cd::Trim(lines[i]);
        if (line.empty()) continue;
        std::vector<std::wstring> parts = cd::Split(line, L' ');
        if (parts.size() < 2) continue;
        unsigned long key = 0, cnt = 0;
        if (!cd::ParseUlongW(cd::Trim(parts[0]), key)) continue;
        if (!cd::ParseUlongW(cd::Trim(parts[1]), cnt)) continue;
        out[static_cast<unsigned>(key)] += static_cast<long long>(cnt);
    }
    return true;
}

struct WorkerArgs {
    std::wstring out1;
    double sec1 = 0.0;
    int settleMs = 0;
    bool spawnGc = false;
    std::wstring gcOut;
    double gcSec = 0.0;
    std::wstring gcExe;      // image the grandchild is spawned from; empty = this exe
    bool phase2 = false;
    std::wstring p2Out;
    double p2Sec = 0.0;
    std::wstring gate;
};

static std::wstring BuildWorkerCmdLine(const WorkerArgs& a) {
    std::wstring s = L"--worker " + Q(a.out1) + L" " + std::to_wstring(a.sec1);
    if (a.settleMs > 0) s += L" --settle " + std::to_wstring(a.settleMs);
    if (a.spawnGc)
        s += L" --spawn-grandchild " + Q(a.gcOut) + L" " + std::to_wstring(a.gcSec);
    if (!a.gcExe.empty()) s += L" --grandchild-exe " + Q(a.gcExe);
    if (a.phase2) s += L" --phase2 " + Q(a.p2Out) + L" " + std::to_wstring(a.p2Sec);
    if (!a.gate.empty()) s += L" --gate " + Q(a.gate);
    return s;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

static bool WaitForFile(const std::wstring& p, DWORD timeoutMs) {
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (FileExists(p)) return true;
        if (GetTickCount64() - t0 > timeoutMs) return false;
        Sleep(40);
    }
}

// Spawn `exePath` (or this exe when empty) with `args`. Never inherits handles.
static bool SpawnExe(const std::wstring& exePath, const std::wstring& args, bool suspended,
                     PROCESS_INFORMATION& pi, DWORD* err) {
    ZeroMemory(&pi, sizeof(pi));
    std::wstring exe = exePath.empty() ? cd::GetExePath() : exePath;
    if (exe.empty()) { if (err) *err = ERROR_FILE_NOT_FOUND; return false; }
    std::wstring cmd = Q(exe) + L" " + args;

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    BOOL ok = CreateProcessW(exe.c_str(), &buf[0], nullptr, nullptr, FALSE,
                             suspended ? CREATE_SUSPENDED : 0,
                             nullptr, nullptr, &si, &pi);
    if (!ok) { if (err) *err = GetLastError(); return false; }
    return true;
}

static int RunWorker(int argc, wchar_t** argv) {
    if (argc < 4) {
        printf("worker: usage --worker <hist> <seconds> [...]\n");
        return 2;
    }
    WorkerArgs a;
    a.out1 = argv[2];
    a.sec1 = wcstod(argv[3], nullptr);

    for (int i = 4; i < argc;) {
        if (wcscmp(argv[i], L"--settle") == 0 && i + 1 < argc) {
            a.settleMs = static_cast<int>(wcstol(argv[i + 1], nullptr, 10));
            i += 2;
        } else if (wcscmp(argv[i], L"--spawn-grandchild") == 0 && i + 2 < argc) {
            a.spawnGc = true;
            a.gcOut = argv[i + 1];
            a.gcSec = wcstod(argv[i + 2], nullptr);
            i += 3;
        } else if (wcscmp(argv[i], L"--phase2") == 0 && i + 2 < argc) {
            a.phase2 = true;
            a.p2Out = argv[i + 1];
            a.p2Sec = wcstod(argv[i + 2], nullptr);
            i += 3;
        } else if (wcscmp(argv[i], L"--grandchild-exe") == 0 && i + 1 < argc) {
            a.gcExe = argv[i + 1];
            i += 2;
        } else if (wcscmp(argv[i], L"--gate") == 0 && i + 1 < argc) {
            a.gate = argv[i + 1];
            i += 2;
        } else {
            ++i;
        }
    }

    PROCESS_INFORMATION gc;
    ZeroMemory(&gc, sizeof(gc));
    bool haveGc = false;

    if (a.spawnGc) {
        // The 800 ms is load-bearing: the grandchild must be CREATED AFTER the parent has
        // already been masked. That is the exact scenario a parent-only implementation
        // gets wrong, and a grandchild spawned before the mask lands would not test it.
        Sleep(800);
        WorkerArgs g;
        g.out1 = a.gcOut;
        g.sec1 = a.gcSec;
        g.settleMs = a.settleMs;   // inherit, so both are sampled after the same settle
        DWORD e = 0;
        if (SpawnExe(a.gcExe, BuildWorkerCmdLine(g), false, gc, &e)) {
            haveGc = true;
        } else {
            printf("worker: failed to spawn grandchild, err=%lu\n", static_cast<unsigned long>(e));
        }
    }

    // SETTLE. Not a weakening of anything: architecture 8 Gate B item 2 grants the watcher
    // ONE poll period to notice a new process, so sampling that starts inside that window
    // would fail for a reason the product explicitly does not promise. The window is
    // reported in the selftest output, and the negative control - same settle, no engine -
    // is what proves the settle cannot make the check pass on its own.
    if (a.settleMs > 0) Sleep(static_cast<DWORD>(a.settleMs));

    RunBusyPhase(static_cast<DWORD>(a.sec1 * 1000.0));
    if (!WriteHistogram(a.out1)) printf("worker: failed to write %s\n", N(a.out1).c_str());

    if (a.phase2) {
        if (!a.gate.empty()) {
            // Hand the sequencing to the selftest: it clears the mask (via recovery) and
            // only then creates the gate file, so phase 2 is guaranteed to be sampled
            // entirely AFTER recovery rather than "probably after".
            if (!WaitForFile(a.gate, 60000))
                printf("worker: gate file never appeared, running phase2 anyway\n");
        }
        RunBusyPhase(static_cast<DWORD>(a.p2Sec * 1000.0));
        if (!WriteHistogram(a.p2Out)) printf("worker: failed to write %s\n", N(a.p2Out).c_str());
    }

    // Never leave a grandchild behind: the selftest cannot see it to clean it up.
    if (haveGc) {
        if (WaitForSingleObject(gc.hProcess, 30000) != WAIT_OBJECT_0)
            TerminateProcess(gc.hProcess, 3);
        CloseHandle(gc.hThread);
        CloseHandle(gc.hProcess);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Selftest scaffolding
// ---------------------------------------------------------------------------

static std::vector<std::wstring> g_tempFiles;

static std::wstring TempPath(const wchar_t* tag) {
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    std::wstring d = (n > 0 && n <= MAX_PATH) ? std::wstring(dir, n) : std::wstring(L".\\");
    if (!d.empty() && d[d.size() - 1] != L'\\') d += L'\\';
    std::wstring p = d + L"cd_gateb_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L".txt";
    g_tempFiles.push_back(p);
    DeleteFileW(p.c_str());
    return p;
}

// GATE_B2 needs the child and the grandchild to have DIFFERENT executable names.
//
// Measured on this machine, 2026-08-28, and it cost a whole run: with the harness, the
// child and the grandchild all named gateb_probe.exe, ComputeDesired rule 1 picks the
// LOWEST matching pid as the game root - and Windows pid allocation is not monotonic, so
// the GRANDCHILD can be allocated a lower pid than the child. When that happened,
// gameSet = {grandchild} + Descendants(grandchild) = {grandchild}, the child was correctly
// NOT governed, and the check FAILED for a reason that had nothing to do with the product.
//
// Two runtime copies under %TEMP% fix it deterministically and make the check STRONGER:
// the profile's `game` now matches the child and nothing else, so the grandchild can only
// become governed by being a DESCENDANT - which is exactly the property under test.
static std::wstring MakeExeCopy(const wchar_t* tag) {
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    std::wstring d = (n > 0 && n <= MAX_PATH) ? std::wstring(dir, n) : std::wstring(L".\\");
    if (!d.empty() && d[d.size() - 1] != L'\\') d += L'\\';
    std::wstring p = d + L"cd_gateb_" + tag + L"_" + std::to_wstring(GetCurrentProcessId()) + L".exe";
    DeleteFileW(p.c_str());
    if (!CopyFileW(cd::GetExePath().c_str(), p.c_str(), FALSE)) return std::wstring();
    g_tempFiles.push_back(p);
    return p;
}

static void DeleteTempFiles() {
    for (size_t i = 0; i < g_tempFiles.size(); ++i) {
        DeleteFileW(g_tempFiles[i].c_str());
        DeleteFileW((g_tempFiles[i] + L".tmp").c_str());
    }
    g_tempFiles.clear();
}

// ---------------------------------------------------------------------------
// Governed-pid ledger - the exact scope of GATE_B4's claim
// ---------------------------------------------------------------------------
//
// GATE_B4 used to assert that NO process anywhere on the machine carried a default CPU set
// assignment once the harness was done. That is a claim about the MACHINE, not about
// Game Optimizer, and on this hardware it is false for a reason that is not a product defect:
//
//   [M] measured on this machine while diagnosing a GATE_B4 failure. A scan found 50
//   processes carrying default CPU set assignments this harness never made. They were
//   cleared and the count went immediately to 0; 75 seconds later, with NO Game Optimizer
//   process and NO gateb_probe process running, 3 of them had ACQUIRED an assignment
//   again. A freshly started notepad, the scanning shell itself and explorer had none, so
//   it is not applied blanket to everything. Something on this machine other than
//   Game Optimizer writes default CPU Sets. This harness deliberately does NOT name it:
//   the author of those assignments is NOT proven, and guessing in a gate's output is how
//   an unproven attribution becomes a quoted fact.
//
// So the claim is scoped to what Game Optimizer actually governed. Recorded as it happens:
// every pid this harness spawned, every pid it applied a mask to, every pid cd::Engine
// reported as governed, and every pid the restore journal lists. Everything else on the
// machine is reported as INFORMATION and can never fail the gate.
struct GovernedRec {
    ULONGLONG creationTime = 0;   // 0 when it could not be read; identity then unprovable
    std::wstring how;             // how this pid entered the ledger
};

static std::map<DWORD, GovernedRec> g_governed;

// `ctHint` is a creation time already recorded elsewhere (the journal carries one); 0 means
// "read it now". The creation time is what makes a RECYCLED pid distinguishable from the
// process actually governed - without it this ledger would eventually blame Game Optimizer for
// a stranger that inherited the number.
static void NoteGoverned(DWORD pid, const wchar_t* how, ULONGLONG ctHint = 0) {
    if (pid == 0 || pid == 4) return;
    GovernedRec& rec = g_governed[pid];
    if (ctHint != 0) rec.creationTime = ctHint;
    else if (rec.creationTime == 0) cd::GetProcessCreationTime(pid, rec.creationTime);
    rec.how = how;
}

// A spawned child that is TERMINATED if it is still alive when this goes out of scope.
// "Clean up: kill or wait for every child you spawn" is a constraint, not a nicety - a
// stranded busy worker would poison GATE_B4 and leave the machine altered.
struct ChildProc {
    PROCESS_INFORMATION pi;
    bool live = false;

    ChildProc() { ZeroMemory(&pi, sizeof(pi)); }
    ~ChildProc() { Kill(); }

    DWORD pid() const { return pi.dwProcessId; }

    bool Spawn(const std::wstring& args, bool suspended, DWORD* err) {
        return SpawnFrom(std::wstring(), args, suspended, err);
    }
    bool SpawnFrom(const std::wstring& exePath, const std::wstring& args, bool suspended,
                   DWORD* err) {
        DWORD e = 0;
        if (!SpawnExe(exePath, args, suspended, pi, &e)) { if (err) *err = e; return false; }
        live = true;
        // Recorded at SPAWN, not at apply time: a mask may be applied to this pid later by
        // cd::Engine rather than by this file, and the handle is open right now, so the
        // creation time captured here cannot already belong to a recycled pid.
        NoteGoverned(pi.dwProcessId, L"spawned by the harness");
        return true;
    }
    void Resume() { if (live) ResumeThread(pi.hThread); }
    bool Wait(DWORD ms) {
        if (!live) return true;
        return WaitForSingleObject(pi.hProcess, ms) == WAIT_OBJECT_0;
    }
    bool Exited() const {
        if (!live) return true;
        return WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0;
    }
    void Kill() {
        if (!live) return;
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 4);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        live = false;
    }

private:
    ChildProc(const ChildProc&);
    ChildProc& operator=(const ChildProc&);
};

static std::set<unsigned> ExpectedKeys(const cd::Topology& t, const std::vector<ULONG>& ids) {
    // Group * 64 + LogicalProcessorIndex. On a single-group machine (this one) that is just
    // the LP index; see the hazard note in the report about multi-group machines.
    std::set<unsigned> s;
    for (size_t i = 0; i < ids.size(); ++i) {
        const cd::CpuSetEntry* e = cd::FindById(t, ids[i]);
        if (e == nullptr) continue;
        s.insert(static_cast<unsigned>(e->Group) * 64u + static_cast<unsigned>(e->LogicalProcessorIndex));
    }
    return s;
}

static std::string DescribeHist(const HistMap& h) {
    if (h.empty()) return std::string("(EMPTY - the process recorded no samples at all)");
    long long total = 0;
    for (HistMap::const_iterator it = h.begin(); it != h.end(); ++it) total += it->second;
    std::string s;
    char buf[128];
    for (HistMap::const_iterator it = h.begin(); it != h.end(); ++it) {
        double pct = total ? 100.0 * static_cast<double>(it->second) / static_cast<double>(total) : 0.0;
        sprintf_s(buf, sizeof(buf), "LP%u=%lld(%.1f%%) ", it->first, it->second, pct);
        s += buf;
    }
    sprintf_s(buf, sizeof(buf), " [%d distinct, %lld samples]",
              static_cast<int>(h.size()), total);
    s += buf;
    return s;
}

static std::vector<unsigned> OutsideMask(const HistMap& h, const std::set<unsigned>& expected) {
    std::vector<unsigned> out;
    for (HistMap::const_iterator it = h.begin(); it != h.end(); ++it)
        if (expected.find(it->first) == expected.end()) out.push_back(it->first);
    return out;
}

static std::string JoinU(const std::vector<unsigned>& v) {
    std::string s;
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        sprintf_s(buf, sizeof(buf), "%s%u", i ? "," : "", v[i]);
        s += buf;
    }
    return s.empty() ? std::string("(none)") : s;
}

struct Results {
    int passed = 0;
    int failed = 0;
    int skipped = 0;
};

static void Pass(Results& r, int n) {
    printf("GATE_B%d PASS\n", n);
    ++r.passed;
}
static void Fail(Results& r, int n, const char* why) {
    printf("GATE_B%d FAIL %s\n", n, why);
    ++r.failed;
}
static void Skip(Results& r, int n, const char* why) {
    printf("GATE_B%d SKIP %s\n", n, why);
    ++r.skipped;
}

// ---------------------------------------------------------------------------
// GATE_B1 - PLACEMENT
// ---------------------------------------------------------------------------

static void GateB1(Results& r, const cd::Topology& t, const std::vector<ULONG>& ids2,
                   const std::set<unsigned>& expected) {
    printf("\n--- GATE_B1 PLACEMENT ---------------------------------------------------\n");
    printf("  method: spawn a --worker child CREATE_SUSPENDED, apply the mask with\n");
    printf("          cd::ApplyCpuSets while it is suspended, then resume - so the mask is\n");
    printf("          in force from the child's first instruction and there is no unmasked\n");
    printf("          window to explain away.\n");

    std::wstring hMasked = TempPath(L"b1_masked");
    std::wstring hFree = TempPath(L"b1_free");

    WorkerArgs wa;
    wa.out1 = hMasked;
    wa.sec1 = 2.0;

    ChildProc c;
    DWORD e = 0;
    if (!c.Spawn(BuildWorkerCmdLine(wa), true, &e)) {
        char b[128];
        sprintf_s(b, sizeof(b), "could not spawn masked worker, CreateProcess err=%lu",
                  static_cast<unsigned long>(e));
        Fail(r, 1, b);
        return;
    }
    printf("  masked worker pid=%lu\n", static_cast<unsigned long>(c.pid()));
    NoteGoverned(c.pid(), L"cd::ApplyCpuSets in GATE_B1");

    cd::ApplyOutcome oc = cd::ApplyCpuSets(c.pid(), ids2);
    printf("  cd::ApplyCpuSets -> %s (err %lu)\n", N(cd::ApplyResultName(oc.result)).c_str(),
           static_cast<unsigned long>(oc.lastError));
    if (oc.result != cd::ApplyResult::Ok) {
        c.Kill();
        Fail(r, 1, "ApplyCpuSets did not return Ok");
        return;
    }
    c.Resume();
    if (!c.Wait(60000)) {
        c.Kill();
        Fail(r, 1, "masked worker did not exit within 60 s");
        return;
    }
    c.Kill();

    HistMap masked;
    if (!ReadHistogram(hMasked, masked)) {
        Fail(r, 1, "masked worker wrote no histogram");
        return;
    }
    printf("  masked  observed: %s\n", DescribeHist(masked).c_str());

    std::vector<unsigned> out = OutsideMask(masked, expected);

    // Negative control. Without it this check passes vacuously on a machine that only ever
    // had 2 processors available to begin with - the subset assertion would be true for a
    // completely ineffective assignment.
    WorkerArgs wf;
    wf.out1 = hFree;
    wf.sec1 = 2.0;
    ChildProc f;
    if (!f.Spawn(BuildWorkerCmdLine(wf), false, &e)) {
        Fail(r, 1, "could not spawn the unmasked negative-control worker");
        return;
    }
    printf("  control (no mask applied) worker pid=%lu\n", static_cast<unsigned long>(f.pid()));
    if (!f.Wait(60000)) {
        f.Kill();
        Fail(r, 1, "unmasked control worker did not exit within 60 s");
        return;
    }
    f.Kill();

    HistMap free_;
    if (!ReadHistogram(hFree, free_)) {
        Fail(r, 1, "unmasked control worker wrote no histogram");
        return;
    }
    printf("  control observed: %s\n", DescribeHist(free_).c_str());
    printf("  processors observed OUTSIDE the assigned mask (masked run): %s\n",
           JoinU(out).c_str());

    if (masked.empty()) {
        Fail(r, 1, "masked worker observed NO processors - nothing was measured");
        return;
    }
    if (!out.empty()) {
        char b[256];
        sprintf_s(b, sizeof(b), "masked worker ran on processors outside the mask: %s",
                  JoinU(out).c_str());
        Fail(r, 1, b);
        return;
    }
    if (free_.size() <= masked.size()) {
        char b[256];
        sprintf_s(b, sizeof(b),
                  "negative control saw %d distinct processors, masked run saw %d - the "
                  "confinement is not attributable to the mask",
                  static_cast<int>(free_.size()), static_cast<int>(masked.size()));
        Fail(r, 1, b);
        return;
    }
    Pass(r, 1);
}

// ---------------------------------------------------------------------------
// GATE_B2 - CHILD INHERITANCE
// ---------------------------------------------------------------------------

// Drives the engine deterministically and returns once the child has exited (the child
// waits for its own grandchild before exiting, so that also means the grandchild is done).
// Also reports the game root the engine actually SELECTED and the peak number of processes
// it governed. Without that, a run where the engine picked a different game root looks
// identical to a run where the product failed to govern a descendant.
static void DriveEngine(cd::Engine& eng, ChildProc& c, DWORD budgetMs,
                        DWORD* gamePidOut, int* peakGovernedOut) {
    ULONGLONG t0 = GetTickCount64();
    int ticks = 0;
    DWORD gamePid = 0;
    int peak = 0;
    for (;;) {
        eng.TickOnce();
        ++ticks;
        cd::EngineStatus st = eng.GetStatus();
        if (st.gamePid != 0) gamePid = st.gamePid;
        // GATE_B4's residue scope. The GRANDCHILD is spawned by the child process, so this
        // harness never learns its pid any other way - and it is precisely the descendant
        // the engine is supposed to govern and then release.
        for (size_t i = 0; i < st.governed.size(); ++i)
            NoteGoverned(st.governed[i].pid, L"governed by cd::Engine");
        int governed = static_cast<int>(st.governed.size());
        if (governed > peak) peak = governed;
        if (c.Exited()) break;
        if (GetTickCount64() - t0 > budgetMs) break;
        Sleep(250);
    }
    printf("  drove Engine::TickOnce() %d times over %lu ms; engine selected gamePid=%lu, "
           "peak governed=%d\n",
           ticks, static_cast<unsigned long>(GetTickCount64() - t0),
           static_cast<unsigned long>(gamePid), peak);
    if (gamePidOut) *gamePidOut = gamePid;
    if (peakGovernedOut) *peakGovernedOut = peak;
}

static void GateB2(Results& r, const cd::Topology& t, const std::vector<ULONG>& ids2,
                   const std::set<unsigned>& expected) {
    printf("\n--- GATE_B2 CHILD INHERITANCE -------------------------------------------\n");

    // Two runtime copies of this exe, so the child and the grandchild have DIFFERENT names.
    // See MakeExeCopy: with one shared name the engine's lowest-pid game-root rule can pick
    // the grandchild, and the check then fails for a reason unrelated to the product.
    std::wstring gameExe = MakeExeCopy(L"game");
    std::wstring kidExe = MakeExeCopy(L"kid");
    if (gameExe.empty() || kidExe.empty()) {
        Fail(r, 2, "could not create the two temp exe copies the check needs");
        return;
    }
    std::wstring gameName = cd::BaseName(gameExe);
    std::wstring kidName = cd::BaseName(kidExe);

    printf("  method: an in-memory Config (the user's config.ini is never read or written)\n");
    printf("          with one enabled profile game=%s mask=GateB. The engine is driven\n",
           N(gameName).c_str());
    printf("          with TickOnce() every 250 ms - Start() is never called, so the test\n");
    printf("          is deterministic. The child spawns a GRANDCHILD (%s) 800 ms in, i.e.\n",
           N(kidName).c_str());
    printf("          AFTER the parent was already masked. The grandchild's name is NOT in\n");
    printf("          the profile, so it can only be governed as a DESCENDANT.\n");

    // The in-memory config. Nothing here touches %LOCALAPPDATA%\GameOptimizer\config.ini.
    cd::Config cfg;
    cfg.version = 1;
    cfg.pollMs = 250;
    cfg.paused = false;
    cd::Mask m;
    m.name = L"GateB";
    m.ids = ids2;
    m.derived = false;
    cfg.masks.push_back(m);
    cd::Profile p;
    p.name = L"GateB";
    p.enabled = true;
    p.game = gameName;
    p.gameMask = L"GateB";
    p.heavyMask = L"";
    p.autoPin = false;
    cfg.profiles.push_back(p);

    std::wstring hC = TempPath(L"b2_child");
    std::wstring hG = TempPath(L"b2_grandchild");

    WorkerArgs wa;
    wa.out1 = hC;
    wa.sec1 = 3.0;
    wa.settleMs = 1500;
    wa.spawnGc = true;
    wa.gcOut = hG;
    wa.gcSec = 2.0;
    wa.gcExe = kidExe;

    HistMap child, grand;
    DWORD selectedGamePid = 0;
    DWORD childPid = 0;
    int peakGoverned = 0;
    {
        cd::Engine eng;
        eng.SetTopology(t);
        eng.SetConfig(cfg);

        ChildProc c;
        DWORD e = 0;
        if (!c.SpawnFrom(gameExe, BuildWorkerCmdLine(wa), false, &e)) {
            Fail(r, 2, "could not spawn the child worker");
            return;
        }
        childPid = c.pid();
        printf("  child pid=%lu (spawns its grandchild 800 ms later)\n",
               static_cast<unsigned long>(childPid));
        DriveEngine(eng, c, 20000, &selectedGamePid, &peakGoverned);
        c.Wait(20000);
        c.Kill();
        eng.Stop();   // clears every applied mask and truncates the journal
    }

    bool okC = ReadHistogram(hC, child);
    bool okG = WaitForFile(hG, 5000) && ReadHistogram(hG, grand);
    printf("  child       observed: %s\n", okC ? DescribeHist(child).c_str() : "(no histogram file)");
    printf("  grandchild  observed: %s\n", okG ? DescribeHist(grand).c_str() : "(no histogram file)");

    std::vector<unsigned> outC = OutsideMask(child, expected);
    std::vector<unsigned> outG = OutsideMask(grand, expected);
    printf("  child      outside mask: %s\n", JoinU(outC).c_str());
    printf("  grandchild outside mask: %s\n", JoinU(outG).c_str());

    // --- the negative control, run BEFORE deciding, so its evidence is always printed ---
    printf("  negative control: same scenario, Engine given an EMPTY config\n");
    std::wstring nC = TempPath(L"b2n_child");
    std::wstring nG = TempPath(L"b2n_grandchild");
    WorkerArgs wn = wa;
    wn.out1 = nC;
    wn.gcOut = nG;

    HistMap nchild, ngrand;
    {
        cd::Config empty;
        cd::Engine eng2;
        eng2.SetTopology(t);
        eng2.SetConfig(empty);

        ChildProc c2;
        DWORD e = 0;
        if (!c2.SpawnFrom(gameExe, BuildWorkerCmdLine(wn), false, &e)) {
            Fail(r, 2, "could not spawn the negative-control child worker");
            return;
        }
        printf("  control child pid=%lu\n", static_cast<unsigned long>(c2.pid()));
        DWORD ngp = 0;
        int npeak = 0;
        DriveEngine(eng2, c2, 20000, &ngp, &npeak);
        c2.Wait(20000);
        c2.Kill();
        eng2.Stop();
    }
    bool okNC = ReadHistogram(nC, nchild);
    bool okNG = WaitForFile(nG, 5000) && ReadHistogram(nG, ngrand);
    printf("  control child      observed: %s\n", okNC ? DescribeHist(nchild).c_str() : "(no file)");
    printf("  control grandchild observed: %s\n", okNG ? DescribeHist(ngrand).c_str() : "(no file)");
    std::vector<unsigned> nOutG = OutsideMask(ngrand, expected);
    printf("  control grandchild outside mask: %s\n", JoinU(nOutG).c_str());

    if (!okNG || ngrand.empty()) {
        printf("  *** THE NEGATIVE CONTROL DID NOT RUN. GATE_B2 PROVES NOTHING THIS RUN. ***\n");
        Fail(r, 2, "negative control produced no grandchild histogram - the check is unproven");
        return;
    }
    if (nOutG.empty()) {
        printf("  *** LOUD WARNING: with NO engine running at all, the grandchild was STILL\n");
        printf("      confined to the mask. Something other than Game Optimizer is confining\n");
        printf("      these processes, so a PASS above would prove nothing. ***\n");
        Fail(r, 2, "grandchild confined to the mask even with an EMPTY config - check is vacuous");
        return;
    }

    // --- SECOND control: is the grandchild's confinement Game Optimizer's doing, or the OS's?
    // The empty-config control above leaves the whole chain unmasked, so it cannot separate
    // "the engine governed the grandchild" from "the grandchild INHERITED its parent's
    // assignment". If inheritance existed, GATE_B2 would pass for a parent-only
    // implementation - which is the single defect this check exists to catch.
    // measurements 4 says children do not inherit; this re-measures it here rather than
    // trusting the note, by masking ONLY the child, with no engine running at all.
    printf("  inheritance control: mask ONLY the child, NO engine - does the grandchild\n");
    printf("                       inherit the parent's assignment?\n");
    std::wstring iC = TempPath(L"b2i_child");
    std::wstring iG = TempPath(L"b2i_grandchild");
    WorkerArgs wi = wa;
    wi.out1 = iC;
    wi.gcOut = iG;

    HistMap ichild, igrand;
    {
        ChildProc c3;
        DWORD e = 0;
        if (!c3.SpawnFrom(gameExe, BuildWorkerCmdLine(wi), true, &e)) {
            Fail(r, 2, "could not spawn the inheritance-control child worker");
            return;
        }
        cd::ApplyOutcome ioc = cd::ApplyCpuSets(c3.pid(), ids2);
        NoteGoverned(c3.pid(), L"cd::ApplyCpuSets in GATE_B2 inheritance control");
        printf("  inheritance-control child pid=%lu, ApplyCpuSets -> %s\n",
               static_cast<unsigned long>(c3.pid()), N(cd::ApplyResultName(ioc.result)).c_str());
        if (ioc.result != cd::ApplyResult::Ok) {
            c3.Kill();
            Fail(r, 2, "could not mask the inheritance-control child");
            return;
        }
        c3.Resume();
        c3.Wait(30000);
        c3.Kill();
    }
    bool okIC = ReadHistogram(iC, ichild);
    bool okIG = WaitForFile(iG, 5000) && ReadHistogram(iG, igrand);
    printf("  inheritance-control child      observed: %s\n",
           okIC ? DescribeHist(ichild).c_str() : "(no file)");
    printf("  inheritance-control grandchild observed: %s\n",
           okIG ? DescribeHist(igrand).c_str() : "(no file)");
    std::vector<unsigned> iOutC = OutsideMask(ichild, expected);
    std::vector<unsigned> iOutG = OutsideMask(igrand, expected);
    printf("  inheritance-control child      outside mask: %s\n", JoinU(iOutC).c_str());
    printf("  inheritance-control grandchild outside mask: %s\n", JoinU(iOutG).c_str());

    if (!okIG || igrand.empty()) {
        Fail(r, 2, "inheritance control produced no grandchild histogram - the check is unproven");
        return;
    }
    if (!okIC || ichild.empty() || !iOutC.empty()) {
        Fail(r, 2, "inheritance control's child was not itself confined, so its grandchild "
                   "result says nothing about inheritance");
        return;
    }
    if (iOutG.empty()) {
        printf("  *** LOUD WARNING: the grandchild of a masked parent was confined to the\n");
        printf("      parent's mask with NO engine running. CPU Sets ARE inherited on this\n");
        printf("      build, so GATE_B2 above would pass for a PARENT-ONLY implementation\n");
        printf("      and proves nothing about the watcher. ***\n");
        Fail(r, 2, "grandchild inherited the parent's mask - GATE_B2 cannot distinguish a "
                   "watcher from a parent-only implementation");
        return;
    }

    // Precondition, not an outcome: the engine must have selected THE CHILD as the game
    // root, or the scenario under test (a grandchild of the game) was never set up and any
    // verdict below would be about the wrong thing. With two distinct exe names this is
    // deterministic; it is asserted rather than assumed because it silently was not, once.
    if (selectedGamePid != childPid) {
        char b[256];
        sprintf_s(b, sizeof(b),
                  "engine selected gamePid=%lu but the child is pid=%lu - the scenario under "
                  "test was never set up, so this run is inconclusive rather than a product "
                  "failure", static_cast<unsigned long>(selectedGamePid),
                  static_cast<unsigned long>(childPid));
        Fail(r, 2, b);
        return;
    }
    if (peakGoverned < 2) {
        char b[192];
        sprintf_s(b, sizeof(b),
                  "engine governed at most %d process(es) - it never governed the child AND "
                  "the grandchild together", peakGoverned);
        Fail(r, 2, b);
        return;
    }
    if (!okC || child.empty()) {
        Fail(r, 2, "child produced no samples");
        return;
    }
    if (!okG || grand.empty()) {
        Fail(r, 2, "grandchild produced no samples");
        return;
    }
    if (!outC.empty()) {
        char b[256];
        sprintf_s(b, sizeof(b), "child ran outside the mask on: %s", JoinU(outC).c_str());
        Fail(r, 2, b);
        return;
    }
    if (!outG.empty()) {
        char b[256];
        sprintf_s(b, sizeof(b),
                  "GRANDCHILD ran outside the mask on: %s - a process spawned after the "
                  "parent was masked is NOT being governed", JoinU(outG).c_str());
        Fail(r, 2, b);
        return;
    }
    Pass(r, 2);
}

// ---------------------------------------------------------------------------
// GATE_B3 - CRASH RECOVERY
// ---------------------------------------------------------------------------

static void GateB3(Results& r, const std::vector<ULONG>& ids2,
                   const std::set<unsigned>& expected) {
    printf("\n--- GATE_B3 CRASH RECOVERY ----------------------------------------------\n");
    printf("  method: ONE worker sampled in TWO phases - not a fresh process, because a\n");
    printf("          fresh process was never masked and so could not show a release.\n");
    printf("          phase 1 runs masked + journalled; the worker then blocks on a gate\n");
    printf("          file; the harness calls cd::RecoverFromJournal() (simulating the next\n");
    printf("          launch after a crash) and only then creates the gate file, so phase 2\n");
    printf("          is sampled entirely AFTER recovery.\n");

    std::wstring h1 = TempPath(L"b3_phase1_masked");
    std::wstring h2 = TempPath(L"b3_phase2_after_recovery");
    std::wstring gate = TempPath(L"b3_gate");

    WorkerArgs wa;
    wa.out1 = h1;
    wa.sec1 = 2.0;
    wa.phase2 = true;
    wa.p2Out = h2;
    wa.p2Sec = 2.0;
    wa.gate = gate;

    ChildProc c;
    DWORD e = 0;
    if (!c.Spawn(BuildWorkerCmdLine(wa), true, &e)) {
        Fail(r, 3, "could not spawn the recovery-subject worker");
        return;
    }
    printf("  subject pid=%lu\n", static_cast<unsigned long>(c.pid()));

    cd::ApplyOutcome oc = cd::ApplyCpuSets(c.pid(), ids2);
    NoteGoverned(c.pid(), L"cd::ApplyCpuSets in GATE_B3 (simulated crash)");
    printf("  cd::ApplyCpuSets -> %s (err %lu)\n", N(cd::ApplyResultName(oc.result)).c_str(),
           static_cast<unsigned long>(oc.lastError));
    if (oc.result != cd::ApplyResult::Ok) {
        c.Kill();
        Fail(r, 3, "ApplyCpuSets did not return Ok");
        return;
    }
    ULONGLONG ct = 0;
    cd::GetProcessCreationTime(c.pid(), ct);
    cd::JournalAdd(c.pid(), ct, L"gateb_probe.exe");
    printf("  cd::JournalAdd(pid=%lu, creationTime=%llu) - exactly what the engine does\n",
           static_cast<unsigned long>(c.pid()), ct);
    c.Resume();

    if (!WaitForFile(h1, 60000)) {
        c.Kill();
        Fail(r, 3, "phase 1 histogram never appeared");
        return;
    }

    // No ClearCpuSets, no Engine::Stop - this is the crash. The journal is all that is left.
    int recovered = cd::RecoverFromJournal();
    printf("  cd::RecoverFromJournal() -> %d process(es) cleared\n", recovered);

    cd::WriteFileUtf8Atomic(gate, L"go\n");

    if (!c.Wait(60000)) {
        c.Kill();
        Fail(r, 3, "subject did not exit within 60 s");
        return;
    }
    c.Kill();

    HistMap p1, p2;
    bool ok1 = ReadHistogram(h1, p1);
    bool ok2 = ReadHistogram(h2, p2);
    printf("  phase 1 (masked, before recovery) observed: %s\n",
           ok1 ? DescribeHist(p1).c_str() : "(no file)");
    printf("  phase 2 (after recovery)          observed: %s\n",
           ok2 ? DescribeHist(p2).c_str() : "(no file)");

    std::vector<unsigned> out1 = OutsideMask(p1, expected);
    std::vector<unsigned> out2 = OutsideMask(p2, expected);
    printf("  phase 1 outside mask: %s\n", JoinU(out1).c_str());
    printf("  phase 2 outside mask: %s\n", JoinU(out2).c_str());

    std::vector<cd::JournalEntry> after = cd::JournalRead();
    printf("  journal entries after recovery: %d\n", static_cast<int>(after.size()));

    if (recovered < 1) {
        Fail(r, 3, "RecoverFromJournal() cleared 0 processes");
        return;
    }
    if (!ok2 || p2.empty()) {
        Fail(r, 3, "no post-recovery samples - the release was never observed");
        return;
    }
    if (out2.empty()) {
        Fail(r, 3, "after recovery the process was STILL confined to the mask - it was not "
                   "released back to the whole machine");
        return;
    }
    if (!after.empty()) {
        Fail(r, 3, "journal was not empty after RecoverFromJournal()");
        return;
    }
    if (!ok1 || p1.empty() || !out1.empty()) {
        // Phase 1 is the "it really was confined beforehand" half. Without it, phase 2's
        // freedom is not evidence of a release.
        Fail(r, 3, "phase 1 did not show the subject confined to the mask, so phase 2's "
                   "freedom is not evidence of a release");
        return;
    }
    Pass(r, 3);
}

// ---------------------------------------------------------------------------
// GATE_B4 - NO RESIDUE
// ---------------------------------------------------------------------------

static std::string IdsToStr(const std::vector<ULONG>& ids, size_t maxShow) {
    std::string s;
    char buf[32];
    for (size_t i = 0; i < ids.size() && i < maxShow; ++i) {
        sprintf_s(buf, sizeof(buf), "%s%lu", i ? " " : "", static_cast<unsigned long>(ids[i]));
        s += buf;
    }
    if (ids.size() > maxShow) s += " ...";
    return s.empty() ? std::string("(none)") : s;
}

static void GateB4(Results& r, const std::vector<ULONG>& ids2) {
    printf("\n--- GATE_B4 NO RESIDUE --------------------------------------------------\n");
    printf("  claim under test: CORE DIRECTOR left no residue.\n");
    printf("  NOT the claim: \"no process on this machine carries a default CPU set\". That is\n");
    printf("      a claim about the MACHINE, it is not Game Optimizer's to make, and it is\n");
    printf("      measurably false on this hardware - see the INFORMATION section below.\n");
    printf("  method: cd::ReadCpuSets on every pid this harness spawned, masked, saw\n");
    printf("      cd::Engine govern, or found in the restore journal. Reading back IS the\n");
    printf("      right call here - the claim is about what is ASSIGNED, not about placement.\n");
    printf("      A pid that has EXITED counts as clean; a dead process carries nothing. Each\n");
    printf("      live pid is re-checked against the creation time recorded when it was\n");
    printf("      governed, so a RECYCLED pid is never blamed on Game Optimizer.\n");

    // --- the restore journal --------------------------------------------------------------
    // Read BEFORE anything is cleared. The journal is the product's own record of cleanups it
    // still owes, so an entry surviving a clean shutdown IS residue, and the pid it names is
    // one this product governed.
    std::vector<cd::JournalEntry> jrnl = cd::JournalRead();
    for (size_t i = 0; i < jrnl.size(); ++i)
        NoteGoverned(jrnl[i].pid, L"listed in the restore journal", jrnl[i].creationTime);
    printf("  restore journal: %d entr%s%s\n", static_cast<int>(jrnl.size()),
           jrnl.size() == 1 ? "y" : "ies", jrnl.empty() ? "  (empty, as required)" : "");
    for (size_t i = 0; i < jrnl.size(); ++i)
        printf("    JOURNAL RESIDUE pid %lu %s\n",
               static_cast<unsigned long>(jrnl[i].pid), N(jrnl[i].name).c_str());

    // --- the pids Game Optimizer actually governed -------------------------------------------
    int gDead = 0, gRecycled = 0, gClean = 0, gUnreadable = 0;
    std::vector<std::wstring> residue;
    std::vector<std::wstring> unreadable;
    for (std::map<DWORD, GovernedRec>::const_iterator it = g_governed.begin();
         it != g_governed.end(); ++it) {
        const DWORD pid = it->first;
        ULONGLONG live = 0;
        if (!cd::GetProcessCreationTime(pid, live)) { ++gDead; continue; }
        if (it->second.creationTime != 0 && live != it->second.creationTime) {
            ++gRecycled;   // a stranger now owns this number; clearing or blaming it would
            continue;      // be exactly the bug the journal's creation-time guard prevents
        }
        std::vector<ULONG> ids;
        if (!cd::ReadCpuSets(pid, ids)) {
            // Racing an exit is the likely cause, so re-check liveness before calling this a
            // hole - otherwise a process that died between the two calls is a flaky FAIL.
            ULONGLONG again = 0;
            if (!cd::GetProcessCreationTime(pid, again)) { ++gDead; continue; }
            ++gUnreadable;
            unreadable.push_back(L"pid " + std::to_wstring(pid) + L" [" + it->second.how + L"]");
            continue;
        }
        if (ids.empty()) { ++gClean; continue; }
        std::wstring s = L"pid " + std::to_wstring(pid) + L" [" + it->second.how + L"] "
                       + std::to_wstring(ids.size()) + L" ids:";
        for (size_t i = 0; i < ids.size() && i < 8; ++i) s += L" " + std::to_wstring(ids[i]);
        if (ids.size() > 8) s += L" ...";
        residue.push_back(s);
    }
    printf("  pids Game Optimizer governed this run: %d  (exited: %d, pid recycled: %d,\n"
           "      live and clear: %d, unreadable: %d, RESIDUE: %d)\n",
           static_cast<int>(g_governed.size()), gDead, gRecycled, gClean, gUnreadable,
           static_cast<int>(residue.size()));
    printf("  HONEST LIMIT: the harness kills every worker it spawns, so most tracked pids are\n");
    printf("      already gone when this runs and a dead process cannot carry residue. The\n");
    printf("      load-bearing halves of this check are the EMPTY JOURNAL above and GATE_B3,\n");
    printf("      which watches a LIVE process be released back to the whole machine.\n");
    for (size_t i = 0; i < residue.size(); ++i) printf("    RESIDUE %s\n", N(residue[i]).c_str());
    for (size_t i = 0; i < unreadable.size(); ++i)
        printf("    UNREADABLE - cannot prove clean: %s\n", N(unreadable[i]).c_str());

    // --- INFORMATION: everything this harness never governed --------------------------------
    // Diagnostic only, and deliberately incapable of failing the gate. It exists because the
    // machine-wide form of this check was misread as a product defect once already.
    printf("\n  --- INFORMATION: other processes (never a GATE_B4 failure) ---------------\n");
    cd::ProcessSnapshot snap;
    int totalLp = cd::GetTotalLogicalProcessors();
    if (totalLp <= 0) totalLp = 1;
    if (!snap.Take(nullptr, 250, totalLp, 8)) {
        printf("  ProcessSnapshot::Take failed, so the machine-wide survey is UNAVAILABLE this\n");
        printf("  run. It is diagnostic only and the verdict above does not depend on it.\n");
    } else {
        int others = 0, denied = 0, withSets = 0, sameAsOurMask = 0;
        std::map<std::vector<ULONG>, std::pair<int, std::wstring> > groups;
        const std::map<DWORD, cd::ProcInfo>& all = snap.All();
        for (std::map<DWORD, cd::ProcInfo>::const_iterator it = all.begin(); it != all.end(); ++it) {
            const DWORD pid = it->first;
            if (pid == 0 || pid == 4) continue;
            if (g_governed.find(pid) != g_governed.end()) continue;   // judged above
            ++others;
            std::vector<ULONG> ids;
            if (!cd::ReadCpuSets(pid, ids)) { ++denied; continue; }
            if (ids.empty()) continue;
            ++withSets;
            if (!ids2.empty() && ids == ids2) ++sameAsOurMask;
            std::pair<int, std::wstring>& g = groups[ids];
            ++g.first;
            if (g.second.empty())
                g.second = it->second.name + L" (pid " + std::to_wstring(pid) + L")";
        }
        printf("  processes this harness never governed: %d   readable: %d   access-denied: %d\n",
               others, others - denied, denied);
        printf("  of the readable ones, %d carry a non-empty default CPU set assignment,\n",
               withSets);
        printf("  in %d distinct id-set(s):\n", static_cast<int>(groups.size()));
        if (groups.empty()) printf("    (none)\n");
        for (std::map<std::vector<ULONG>, std::pair<int, std::wstring> >::const_iterator
                 g = groups.begin(); g != groups.end(); ++g) {
            const bool same = (!ids2.empty() && g->first == ids2);
            printf("    %4d process(es)  %2d id(s): %s   e.g. %s%s\n",
                   g->second.first, static_cast<int>(g->first.size()),
                   IdsToStr(g->first, 20).c_str(), N(g->second.second).c_str(),
                   same ? "   <-- the same ids this harness applies" : "");
        }
        printf("  Software other than Game Optimizer also calls SetProcessDefaultCpuSets on this\n");
        printf("  machine: assignments were observed RE-APPEARING on processes while no\n");
        printf("  Game Optimizer and no gateb_probe were running at all. So the counts above are\n");
        printf("  NOT evidence of Game Optimizer residue and cannot fail this gate. This harness\n");
        printf("  does not attribute them to any particular program - the author is not proven.\n");
        if (sameAsOurMask > 0) {
            printf("  NOTE: %d of them carry EXACTLY the ids this harness applies. Worth a look,\n",
                   sameAsOurMask);
            printf("        but they are not pids this harness or cd::Engine ever governed.\n");
        }
    }

    if (residue.empty() && jrnl.empty() && gUnreadable == 0) {
        Pass(r, 4);
        return;
    }
    char b[320];
    sprintf_s(b, sizeof(b),
              "Game Optimizer residue: %d governed pid(s) still carry a default CPU set, %d "
              "journal entr%s left behind, %d governed pid(s) could not be read",
              static_cast<int>(residue.size()), static_cast<int>(jrnl.size()),
              jrnl.size() == 1 ? "y" : "ies", gUnreadable);
    Fail(r, 4, b);
}

// ---------------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------------

static int RunSelftest() {
    printf("Game Optimizer Gate B harness\n");
    printf("Placement is measured by sampling GetCurrentProcessorNumberEx INSIDE the\n");
    printf("governed process. GetProcessDefaultCpuSets is used in GATE_B4 only, where the\n");
    printf("claim really is about the assignment. See docs\\spec\\04-measurements.md 2.4.\n");

    Results r;

    // SAFETY INTERLOCK, and it is not optional.
    //
    // This harness is DESTRUCTIVE to a live Game Optimizer: GATE_B3 calls RecoverFromJournal(),
    // which clears the CPU sets of every journalled process and truncates the journal, and
    // the selftest truncates the journal again on entry and on exit. Run against a running
    // tray app, it would strip that instance's masks off the user's game and destroy the
    // crash-recovery record that exists to put them back - i.e. it would "leave the machine
    // altered", which the harness is explicitly forbidden to do.
    {
        cd::ProcessSnapshot pre;
        int lp = cd::GetTotalLogicalProcessors();
        if (lp <= 0) lp = 1;
        if (pre.Take(nullptr, 250, lp, 8)) {
            std::vector<DWORD> hits = pre.FindBySpec(L"GameOptimizer.exe");
            if (!hits.empty()) {
                printf("\n*** REFUSING TO RUN: GameOptimizer.exe is running (pid");
                for (size_t i = 0; i < hits.size(); ++i)
                    printf(" %lu", static_cast<unsigned long>(hits[i]));
                printf(").\n");
                printf("    GATE_B3 calls RecoverFromJournal(), which would clear that\n");
                printf("    instance's applied masks and truncate its journal. Close\n");
                printf("    Game Optimizer and re-run. No check has been executed and nothing\n");
                printf("    on this machine has been changed. ***\n");
                printf("\nGATE_B TOTAL 4 PASSED 0 FAILED 0\n");
                printf("GATE_B SKIPPED 4\n");
                return 2;
            }
        }
    }

    cd::Topology t;
    std::wstring err;
    if (!cd::DetectTopology(t, &err)) {
        printf("DetectTopology FAILED: %s\n", N(err).c_str());
        printf("GATE_B TOTAL 4 PASSED 0 FAILED 4\n");
        return 1;
    }
    printf("\ntopology: kind=%s confidence=%s lps=%d groups=%u signature=%s\n",
           N(cd::KindName(t.kind)).c_str(), N(cd::ConfidenceName(t.confidence)).c_str(),
           t.totalLogicalProcessors, static_cast<unsigned>(t.groupCount),
           N(t.signature).c_str());
    printf("defaultGameMask = \"%s\"\n", N(t.defaultGameMask).c_str());

    // Pre-existing journal state. Reported, then cleared, so GATE_B3's count is
    // unambiguous. A non-empty journal here means some earlier run did not shut down.
    //
    // Truncating it here destroys the only record of those pids, so each one is entered in
    // the governed ledger FIRST, with the creation time the journal recorded. They are
    // Game Optimizer's residue even though this run did not create them, and GATE_B4 is
    // entitled to say so - unlike an assignment on a process Game Optimizer never governed.
    std::vector<cd::JournalEntry> pre = cd::JournalRead();
    printf("journal at start: %d entr%s", static_cast<int>(pre.size()),
           pre.size() == 1 ? "y" : "ies");
    for (size_t i = 0; i < pre.size(); ++i)
        printf(" [pid %lu %s]", static_cast<unsigned long>(pre[i].pid), N(pre[i].name).c_str());
    printf("%s\n", pre.empty() ? "" : "  (from an EARLIER run; tracked for GATE_B4, then cleared)");
    for (size_t i = 0; i < pre.size(); ++i)
        NoteGoverned(pre[i].pid, L"journal entry left by an EARLIER run", pre[i].creationTime);
    if (!pre.empty()) cd::JournalClearAll();

    // --- pick the mask -------------------------------------------------------------------
    std::vector<cd::Mask> masks = cd::DeriveMasks(t);
    const cd::Mask* gm = nullptr;
    for (size_t i = 0; i < masks.size(); ++i)
        if (masks[i].name == t.defaultGameMask) { gm = &masks[i]; break; }

    std::vector<ULONG> ids2;
    std::set<unsigned> expected;
    bool haveMask = false;

    if (gm == nullptr) {
        printf("\nthe topology's defaultGameMask is not in DeriveMasks() output.\n");
    } else {
        printf("mask \"%s\" has %d ids\n", N(gm->name).c_str(), static_cast<int>(gm->ids.size()));
        // Parked entries are measurably ignored by the scheduler (measurements 2.3), so a
        // parked id would make the placement check fail for the wrong reason.
        int parkedSeen = 0;
        for (size_t i = 0; i < gm->ids.size() && ids2.size() < 2; ++i) {
            const cd::CpuSetEntry* ent = cd::FindById(t, gm->ids[i]);
            if (ent == nullptr) continue;
            if (ent->Parked) { ++parkedSeen; continue; }
            ids2.push_back(gm->ids[i]);
        }
        printf("skipped %d parked id(s) while choosing\n", parkedSeen);
        haveMask = (ids2.size() == 2);
    }

    if (!haveMask) {
        printf("\nCould not find 2 UNPARKED CPU Set Ids in the default game mask.\n");
        printf("Skipping the three placement checks rather than emitting a misleading FAIL.\n");
        Skip(r, 1, "fewer than 2 unparked ids in the default game mask");
        Skip(r, 2, "fewer than 2 unparked ids in the default game mask");
        Skip(r, 3, "fewer than 2 unparked ids in the default game mask");
        GateB4(r, ids2);
    } else {
        expected = ExpectedKeys(t, ids2);
        std::vector<ULONG> lps = cd::LpsForIds(t, ids2);
        printf("\nassigned mask under test: ids");
        for (size_t i = 0; i < ids2.size(); ++i) printf(" %lu", static_cast<unsigned long>(ids2[i]));
        printf("  ->  LPs");
        for (size_t i = 0; i < lps.size(); ++i) printf(" %lu", static_cast<unsigned long>(lps[i]));
        printf("   (expected histogram keys:");
        for (std::set<unsigned>::const_iterator it = expected.begin(); it != expected.end(); ++it)
            printf(" %u", *it);
        printf(")\n");

        GateB1(r, t, ids2, expected);
        GateB2(r, t, ids2, expected);
        GateB3(r, ids2, expected);

        // The harness's OWN process is cleared here: it is not in the governed ledger (no
        // mask is ever applied to it), so this is housekeeping, not the check preparing its
        // own answer.
        //
        // cd::JournalClearAll() USED to be called here too. It was removed deliberately: it
        // emptied the journal moments before GATE_B4 asserted the journal was empty, which
        // is a check writing its own evidence. The journal is now left exactly as the
        // product's own shutdown paths left it, and the final cleanup below still restores
        // the machine after the verdict is recorded.
        cd::ClearCpuSets(GetCurrentProcessId());
        Sleep(300);
        GateB4(r, ids2);
    }

    // Leave the machine exactly as found.
    cd::ClearCpuSets(GetCurrentProcessId());
    cd::JournalClearAll();
    DeleteTempFiles();

    printf("\nGATE_B TOTAL 4 PASSED %d FAILED %d\n", r.passed, r.failed);
    if (r.skipped) printf("GATE_B SKIPPED %d\n", r.skipped);
    return (r.failed > 0 || r.skipped > 0) ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--worker") == 0) return RunWorker(argc, argv);
    if (argc >= 2 && wcscmp(argv[1], L"--version") == 0) {
        printf("gateb_probe " __DATE__ " " __TIME__ " (MSVC %d, C++%ld)\n",
               static_cast<int>(_MSC_VER), static_cast<long>(__cplusplus));
        return 0;
    }
    return RunSelftest();
}
