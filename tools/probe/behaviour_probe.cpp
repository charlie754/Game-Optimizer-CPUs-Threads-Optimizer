// Game Optimizer ground-truth probe #2: how do CPU Sets actually BEHAVE on this machine?
//
// Three questions the documentation does not settle, each of which changes the product:
//   Q1  Are CPU Sets a soft preference (threads spill to other processors under
//       oversubscription) or a hard restriction? MS docs say a thread "will typically
//       execute on one of the processors in its list"; a Microsoft Q&A report claims they
//       now behave as hard affinity on AMD multi-CCD parts. Both cannot be right here.
//   Q2  What happens when the assigned set is made ENTIRELY of PARKED processors? On this
//       machine a global policy had all 16 LPs of CCD1 parked. If a mask of parked cores
//       starves a process, that is a shipping hazard; if the cores un-park, it is not.
//   Q3  What does the API actually return for an invalid CPU Set Id? The error mapping in
//       applier.cpp depends on the answer.
//
// Measures, never asserts: prints a histogram of the processors actually observed.
// Sets NO affinity mask anywhere - that would invalidate Q1 by construction, because a
// restrictive affinity mask is documented to override a conflicting CPU Set assignment.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <atomic>
#include <thread>
#include <vector>

static std::atomic<bool> g_run{false};
static std::vector<std::atomic<long>> g_hist(64);

static void burn() {
    while (!g_run.load(std::memory_order_acquire)) Sleep(0);
    volatile double x = 1.0;
    while (g_run.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 20000; ++i) x = x * 1.0000001 + 0.0000001;
        PROCESSOR_NUMBER pn{};
        GetCurrentProcessorNumberEx(&pn);
        unsigned idx = (unsigned)pn.Group * 64u + pn.Number;
        if (idx < g_hist.size()) g_hist[idx].fetch_add(1, std::memory_order_relaxed);
    }
    (void)x;
}

struct SetInfo { ULONG id; ULONG lp; ULONG llc; bool parked; };

static std::vector<SetInfo> readSets() {
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, nullptr, 0);
    std::vector<BYTE> buf(size);
    std::vector<SetInfo> out;
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), size, &size, nullptr, 0))
        return out;
    BYTE* p = buf.data(); BYTE* end = p + size;
    while (p < end) {
        auto* i = (PSYSTEM_CPU_SET_INFORMATION)p;
        if (i->Type == CpuSetInformation)
            out.push_back({ i->CpuSet.Id, i->CpuSet.LogicalProcessorIndex,
                            i->CpuSet.LastLevelCacheIndex, i->CpuSet.Parked != 0 });
        p += i->Size;
    }
    return out;
}

static void runTrial(const char* label, const std::vector<ULONG>& ids, int threads, int ms) {
    for (auto& h : g_hist) h.store(0);

    BOOL ok = SetProcessDefaultCpuSets(GetCurrentProcess(),
                                       ids.empty() ? nullptr : ids.data(),
                                       (ULONG)ids.size());
    DWORD err = ok ? 0 : GetLastError();
    printf("\n--- %s ---\n", label);
    printf("SetProcessDefaultCpuSets(%zu ids) -> %s", ids.size(), ok ? "TRUE" : "FALSE");
    if (!ok) printf("  GetLastError=%lu", err);
    printf("\n");
    if (!ok) return;

    // Read back what the OS says it stored.
    ULONG need = 0;
    GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &need);
    std::vector<ULONG> back(need);
    if (need) GetProcessDefaultCpuSets(GetCurrentProcess(), back.data(), need, &need);
    printf("GetProcessDefaultCpuSets read back %lu ids\n", need);

    g_run.store(false);
    std::vector<std::thread> ts;
    for (int i = 0; i < threads; ++i) ts.emplace_back(burn);
    Sleep(50);
    g_run.store(true, std::memory_order_release);
    Sleep(ms);
    g_run.store(false, std::memory_order_relaxed);
    for (auto& t : ts) t.join();

    long total = 0;
    for (auto& h : g_hist) total += h.load();
    printf("%d busy threads for %d ms, %ld samples. Processors actually observed:\n",
           threads, ms, total);
    int distinct = 0;
    for (size_t i = 0; i < g_hist.size(); ++i) {
        long v = g_hist[i].load();
        if (!v) continue;
        distinct++;
        printf("   LP %2zu : %6ld  (%5.1f%%)\n", i, v, total ? 100.0 * v / total : 0.0);
    }
    printf("   => %d distinct logical processors used\n", distinct);
}

int main() {
    auto sets = readSets();
    printf("cpu sets: %zu\n", sets.size());
    if (sets.empty()) { printf("no cpu sets, aborting\n"); return 1; }

    std::vector<ULONG> unparked, parked;
    for (auto& s : sets) (s.parked ? parked : unparked).push_back(s.id);
    printf("unparked=%zu parked=%zu\n", unparked.size(), parked.size());
    printf("parked LPs:");
    for (auto& s : sets) if (s.parked) printf(" %lu", s.lp);
    printf("\n");

    // Q3 first - it is cheap and does not disturb anything.
    {
        std::vector<ULONG> bogus{ 99999 };
        BOOL ok = SetProcessDefaultCpuSets(GetCurrentProcess(), bogus.data(), 1);
        printf("\n--- Q3 invalid id 99999 ---\nreturn=%s GetLastError=%lu\n",
               ok ? "TRUE" : "FALSE", ok ? 0UL : GetLastError());
        ULONG need = 0;
        GetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0, &need);
        printf("assignment after the failed call: %lu ids (expect 0 - was it left intact?)\n", need);
    }

    // Q1: one single unparked LP, heavily oversubscribed.
    if (!unparked.empty()) {
        std::vector<ULONG> one{ unparked[1 % unparked.size()] };
        runTrial("Q1 single unparked LP, 8 busy threads", one, 8, 2000);
    }

    // Q2: the entire parked domain.
    if (!parked.empty()) {
        runTrial("Q2 ENTIRELY PARKED set, 8 busy threads", parked, 8, 2000);
    }

    // Baseline for comparison: no assignment at all.
    runTrial("Baseline: no assignment (cleared), 8 busy threads", {}, 8, 1500);

    SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
    printf("\ncleared. done.\n");
    return 0;
}
