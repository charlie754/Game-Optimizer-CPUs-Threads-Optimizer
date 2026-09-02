// Game Optimizer - unit-test harness.
//
// Every expectation in this file is written from the header comments in src\topology.h,
// src\config.h, src\engine.h and src\procwatch.h. No implementation .cpp was read while
// writing it: a suite derived from the implementation only proves the implementation
// agrees with itself, and this one has to be able to FAIL.
//
// Topology coverage is driven from SYNTHETIC topologies built by hand, never from
// DetectTopology(), so Intel hybrid / symmetric dual-CCD / single-domain / no-SMT paths
// are all exercised on a machine that has none of them.
//
// NOTE on section C.  ProcessSnapshot::procs_ is private and there is no public mutator,
// so synthetic snapshots are built through the standard-conforming explicit-instantiation
// access idiom (C++17 [temp.explicit]/12: "The usual access checking rules do not apply to
// names used to specify explicit instantiations"). procwatch.h is NOT modified, no macro
// redefines `private`, and the class layout the implementation was compiled with is the one
// used here. See namespace `access` below.
//
// Output contract: prints every failure with file, line and both values, then exactly
//   TOTAL <n> PASSED <n> FAILED <n>
// and returns non-zero when failed > 0.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "agent_transition.h"
#include "applier.h"
#include "config.h"
#include "engine.h"
#include "mask_merge.h"
#include "mask_edit.h"
#include "procwatch.h"
#include "settings_environment.h"
#include "settings_heavy_order.h"
#include "settings_merge.h"
#include "settings_warning.h"
#include "envwarning_text.h"
#include "firstrun_text.h"
#include "startup_warning.h"
#include "topology.h"
#include "util.h"

// ===========================================================================
// Private-member access for ProcessSnapshot (header untouched).
// ===========================================================================
namespace access {

template <typename Tag, typename Tag::type M>
struct Rob {
    friend typename Tag::type get(Tag) { return M; }
};

struct ProcsTag {
    typedef std::map<DWORD, cd::ProcInfo> cd::ProcessSnapshot::*type;
    friend type get(ProcsTag);
};
template struct Rob<ProcsTag, &cd::ProcessSnapshot::procs_>;

inline std::map<DWORD, cd::ProcInfo>& Procs(cd::ProcessSnapshot& s) {
    return s.*get(ProcsTag());
}

}  // namespace access

// ===========================================================================
// Tiny assertion framework.
// ===========================================================================
namespace {

int g_total = 0;
int g_failed = 0;
const char* g_case = "(no case)";

void Case(const char* name) {
    g_case = name;
    std::printf("---- %s\n", name);
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr,
                                  nullptr);
    if (n <= 0) return std::string("<unconvertible>");
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string Show(bool v) { return v ? "true" : "false"; }

std::string Show(int v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%d", v);
    return std::string(b);
}

std::string Show(unsigned int v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%u", v);
    return std::string(b);
}

std::string Show(unsigned long v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lu", v);
    return std::string(b);
}

std::string Show(unsigned long long v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%llu", v);
    return std::string(b);
}

std::string Show(double v) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.4f", v);
    return std::string(b);
}

std::string Show(const std::wstring& v) { return "\"" + Utf8(v) + "\""; }
std::string Show(const wchar_t* v) { return v ? ("\"" + Utf8(v) + "\"") : "<null>"; }
std::string Show(const char* v) { return v ? std::string(v) : "<null>"; }

std::string Show(const std::vector<ULONG>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += Show(v[i]);
    }
    s += "] (n=" + Show((int)v.size()) + ")";
    return s;
}

std::string Show(const std::vector<std::wstring>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += Show(v[i]);
    }
    s += "]";
    return s;
}

std::string Show(cd::TopologyKind k) {
    switch (k) {
        case cd::TopologyKind::Unknown: return "TopologyKind::Unknown";
        case cd::TopologyKind::IntelHybrid: return "TopologyKind::IntelHybrid";
        case cd::TopologyKind::AmdAsymmetricCache: return "TopologyKind::AmdAsymmetricCache";
        case cd::TopologyKind::MultiCcdSymmetric: return "TopologyKind::MultiCcdSymmetric";
        case cd::TopologyKind::SingleDomain: return "TopologyKind::SingleDomain";
    }
    return "TopologyKind::<bad>";
}

std::string Show(cd::Confidence c) {
    switch (c) {
        case cd::Confidence::None: return "Confidence::None";
        case cd::Confidence::Medium: return "Confidence::Medium";
        case cd::Confidence::High: return "Confidence::High";
    }
    return "Confidence::<bad>";
}

std::string Show(cd::MaskNameProblem p) {
    switch (p) {
        case cd::MaskNameProblem::None: return "MaskNameProblem::None";
        case cd::MaskNameProblem::Empty: return "MaskNameProblem::Empty";
        case cd::MaskNameProblem::Duplicate: return "MaskNameProblem::Duplicate";
        case cd::MaskNameProblem::ReservedDerivedName:
            return "MaskNameProblem::ReservedDerivedName";
    }
    return "MaskNameProblem::<bad>";
}

void Fail(const char* file, int line, const char* expr, const std::string& got,
          const std::string& want) {
    ++g_failed;
    std::printf("FAIL [%s] %s:%d\n", g_case, file, line);
    std::printf("       expr     : %s\n", expr);
    std::printf("       actual   : %s\n", got.c_str());
    std::printf("       expected : %s\n", want.c_str());
}

bool CheckTrue(bool cond, const char* expr, const char* file, int line) {
    ++g_total;
    if (!cond) {
        Fail(file, line, expr, "false", "true");
        return false;
    }
    return true;
}

template <class A, class B>
bool CheckEq(const A& a, const B& b, const char* expr, const char* file, int line) {
    ++g_total;
    if (!(a == b)) {
        Fail(file, line, expr, Show(a), Show(b));
        return false;
    }
    return true;
}

template <class A, class B>
bool CheckNe(const A& a, const B& b, const char* expr, const char* file, int line) {
    ++g_total;
    if (a == b) {
        Fail(file, line, expr, Show(a), std::string("anything but ") + Show(b));
        return false;
    }
    return true;
}

#define CHECK(cond) CheckTrue((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) CheckEq((a), (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NE(a, b) CheckNe((a), (b), #a " != " #b, __FILE__, __LINE__)

// ===========================================================================
// Shared helpers.
// ===========================================================================

std::vector<ULONG> IdRange(ULONG first, int count, ULONG step) {
    std::vector<ULONG> v;
    for (int i = 0; i < count; ++i) v.push_back(first + (ULONG)i * step);
    return v;
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const cd::Mask* MaskNamed(const std::vector<cd::Mask>& v, const std::wstring& name) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name) return &v[i];
    return nullptr;
}

std::vector<std::wstring> MaskNames(const std::vector<cd::Mask>& v) {
    std::vector<std::wstring> n;
    for (size_t i = 0; i < v.size(); ++i) n.push_back(v[i].name);
    return n;
}

// Assert a named mask exists and holds exactly `ids`. Returns false (and reports) otherwise.
bool ExpectMask(const std::vector<cd::Mask>& masks, const wchar_t* name,
                const std::vector<ULONG>& ids, const char* file, int line) {
    const cd::Mask* m = MaskNamed(masks, name);
    ++g_total;
    if (!m) {
        Fail(file, line, "mask exists", Show(MaskNames(masks)),
             "a mask named " + Show(name));
        return false;
    }
    ++g_total;
    if (m->ids != ids) {
        Fail(file, line, "mask ids", Show(name) + " -> " + Show(m->ids), Show(ids));
        return false;
    }
    return true;
}

#define EXPECT_MASK(masks, name, ids) ExpectMask((masks), (name), (ids), __FILE__, __LINE__)

// ---- Synthetic topology construction --------------------------------------
// Id is 256 + LogicalProcessorIndex throughout, matching the measured reference machine
// (topology.h: "Id starts at 256, NOT 0. Id 256 <-> LogicalProcessorIndex 0.").

cd::CpuSetEntry Entry(ULONG lp, ULONG coreIndex, ULONG llcIndex, BYTE efficiencyClass,
                      ULONGLONG l3KiB) {
    cd::CpuSetEntry e;
    e.Id = 256u + lp;
    e.LogicalProcessorIndex = lp;
    e.CoreIndex = coreIndex;
    e.LastLevelCacheIndex = llcIndex;
    e.NumaNodeIndex = 0;
    e.Group = 0;
    e.EfficiencyClass = efficiencyClass;
    e.Parked = false;
    e.Allocated = true;
    e.RealTime = false;
    e.LastLevelCacheBytes = l3KiB * 1024ull;
    return e;
}

// Derive `domains` mechanically from `entries` (grouped by LastLevelCacheIndex, ascending),
// fill the scalar fields, then hand the whole thing to the classifier under test.
void FinishTopology(cd::Topology& t) {
    std::vector<ULONG> indices;
    for (size_t i = 0; i < t.entries.size(); ++i) {
        ULONG ix = t.entries[i].LastLevelCacheIndex;
        if (std::find(indices.begin(), indices.end(), ix) == indices.end()) indices.push_back(ix);
    }
    std::sort(indices.begin(), indices.end());

    t.domains.clear();
    for (size_t d = 0; d < indices.size(); ++d) {
        cd::LlcDomain dom;
        dom.index = indices[d];
        dom.l3Bytes = 0;
        for (size_t i = 0; i < t.entries.size(); ++i) {
            if (t.entries[i].LastLevelCacheIndex != dom.index) continue;
            dom.lps.push_back(t.entries[i].LogicalProcessorIndex);
            if (dom.l3Bytes == 0) dom.l3Bytes = t.entries[i].LastLevelCacheBytes;
        }
        std::sort(dom.lps.begin(), dom.lps.end());
        t.domains.push_back(dom);
    }

    t.groupCount = 1;
    t.totalLogicalProcessors = (int)t.entries.size();
    cd::ClassifyTopology(t);
}

// The measured reference machine: Ryzen 9 9950X3D, 16C/32T, two CCDs.
// LPs 0-15 in LastLevelCacheIndex 0, LPs 16-31 in LastLevelCacheIndex 16.
// `swapped` puts the SMALL cache on domain 0 and the LARGE one on domain 16.
cd::Topology MakeReference(bool swapped) {
    cd::Topology t;
    for (ULONG lp = 0; lp < 32; ++lp) {
        ULONG llc = (lp < 16) ? 0u : 16u;
        ULONGLONG kib;
        if (!swapped)
            kib = (llc == 0u) ? 98304ull : 32768ull;
        else
            kib = (llc == 0u) ? 32768ull : 98304ull;
        t.entries.push_back(Entry(lp, (lp / 2u) * 2u, llc, (BYTE)0, kib));
    }
    FinishTopology(t);
    return t;
}

// Intel hybrid: 8 P-cores with SMT (LP 0-15, EfficiencyClass 1) + 8 E-cores without SMT
// (LP 16-23, EfficiencyClass 0), all in one LLC domain.
cd::Topology MakeIntelHybrid() {
    cd::Topology t;
    for (ULONG lp = 0; lp < 16; ++lp)
        t.entries.push_back(Entry(lp, (lp / 2u) * 2u, 0u, (BYTE)1, 36864ull));
    for (ULONG lp = 16; lp < 24; ++lp)
        t.entries.push_back(Entry(lp, lp, 0u, (BYTE)0, 36864ull));
    FinishTopology(t);
    return t;
}

// Two LLC domains, identical L3 size. 16C/32T.
cd::Topology MakeSymmetricDualCcd() {
    cd::Topology t;
    for (ULONG lp = 0; lp < 32; ++lp) {
        ULONG llc = (lp < 16) ? 0u : 16u;
        t.entries.push_back(Entry(lp, (lp / 2u) * 2u, llc, (BYTE)0, 32768ull));
    }
    FinishTopology(t);
    return t;
}

// One LLC domain, 8 cores / 16 threads.
cd::Topology MakeSingleDomain() {
    cd::Topology t;
    for (ULONG lp = 0; lp < 16; ++lp)
        t.entries.push_back(Entry(lp, (lp / 2u) * 2u, 0u, (BYTE)0, 32768ull));
    FinishTopology(t);
    return t;
}

// No SMT anywhere: a distinct CoreIndex for every LP. Two asymmetric domains so that the
// classifier still produces named per-domain masks whose "no SMT" variants would be
// duplicates.
cd::Topology MakeNoSmtMachine() {
    cd::Topology t;
    for (ULONG lp = 0; lp < 16; ++lp) {
        ULONG llc = (lp < 8) ? 0u : 8u;
        ULONGLONG kib = (llc == 0u) ? 98304ull : 32768ull;
        t.entries.push_back(Entry(lp, lp, llc, (BYTE)0, kib));
    }
    FinishTopology(t);
    return t;
}

// ===========================================================================
// A. Topology classification and mask derivation.
// ===========================================================================

void Test_A1_ReferenceMachine() {
    Case("A1 reference machine (Ryzen 9 9950X3D, asymmetric L3)");
    cd::Topology t = MakeReference(false);

    CHECK_EQ((int)t.entries.size(), 32);
    CHECK_EQ((int)t.domains.size(), 2);
    CHECK_EQ(t.totalLogicalProcessors, 32);
    CHECK_EQ(t.kind, cd::TopologyKind::AmdAsymmetricCache);
    CHECK_EQ(t.confidence, cd::Confidence::High);
    CHECK_EQ(t.defaultGameMask, L"Cache");
    CHECK_EQ(t.defaultHeavyMask, L"Freq");

    // The Id <-> LP mapping the whole product depends on.
    CHECK_EQ(t.entries[0].Id, 256ul);
    CHECK_EQ(t.entries[31].Id, 287ul);

    std::vector<cd::Mask> m = cd::DeriveMasks(t);
    CHECK_EQ((int)m.size(), 6);

    EXPECT_MASK(m, L"Cache", IdRange(256u, 16, 1u));         // 256..271
    EXPECT_MASK(m, L"Cache no SMT", IdRange(256u, 8, 2u));   // 256,258,...,270
    EXPECT_MASK(m, L"Freq", IdRange(272u, 16, 1u));          // 272..287
    EXPECT_MASK(m, L"Freq no SMT", IdRange(272u, 8, 2u));    // 272,274,...,286
    EXPECT_MASK(m, L"All", IdRange(256u, 32, 1u));           // 256..287
    EXPECT_MASK(m, L"All no SMT", IdRange(256u, 16, 2u));    // 256,258,...,286

    // topology.h: "'All' and 'All no SMT' are always last."
    if (m.size() >= 2) {
        CHECK_EQ(m[m.size() - 2].name, L"All");
        CHECK_EQ(m[m.size() - 1].name, L"All no SMT");
    }

    CHECK(!t.signature.empty());
}

void Test_A2_InvertedDomainOrder() {
    Case("A2 INVERTED: large L3 on the SECOND domain (classifier must key on SIZE)");
    cd::Topology t = MakeReference(true);

    CHECK_EQ(t.kind, cd::TopologyKind::AmdAsymmetricCache);
    CHECK_EQ(t.confidence, cd::Confidence::High);
    CHECK_EQ(t.defaultGameMask, L"Cache");
    CHECK_EQ(t.defaultHeavyMask, L"Freq");

    // Sanity on the fixture itself: domain 0 really is the small one now.
    CHECK_EQ((int)t.domains.size(), 2);
    if (t.domains.size() == 2) {
        CHECK_EQ(t.domains[0].l3Bytes, 32768ull * 1024ull);
        CHECK_EQ(t.domains[1].l3Bytes, 98304ull * 1024ull);
    }

    std::vector<cd::Mask> m = cd::DeriveMasks(t);

    // THE POINT OF THIS TEST: "Cache" is the ids of the SECOND domain now. A detector that
    // assumed "the first CCD is the cache CCD" passes A1 and fails here, and would silently
    // pin games to the wrong CCD on real hardware.
    EXPECT_MASK(m, L"Cache", IdRange(272u, 16, 1u));
    EXPECT_MASK(m, L"Cache no SMT", IdRange(272u, 8, 2u));
    EXPECT_MASK(m, L"Freq", IdRange(256u, 16, 1u));
    EXPECT_MASK(m, L"Freq no SMT", IdRange(256u, 8, 2u));
}

void Test_A3_IntelHybrid() {
    Case("A3 Intel hybrid (8P+SMT / 8E, one LLC domain)");
    cd::Topology t = MakeIntelHybrid();

    CHECK_EQ((int)t.entries.size(), 24);
    CHECK_EQ((int)t.domains.size(), 1);
    CHECK_EQ(t.kind, cd::TopologyKind::IntelHybrid);
    CHECK_EQ(t.confidence, cd::Confidence::High);
    CHECK_EQ(t.defaultGameMask, L"P-cores");
    CHECK_EQ(t.defaultHeavyMask, L"E-cores");

    std::vector<cd::Mask> m = cd::DeriveMasks(t);
    EXPECT_MASK(m, L"P-cores", IdRange(256u, 16, 1u));        // LP 0..15
    EXPECT_MASK(m, L"P-cores no SMT", IdRange(256u, 8, 2u));  // LP 0,2,...,14
    EXPECT_MASK(m, L"E-cores", IdRange(272u, 8, 1u));         // LP 16..23

    // topology.h: "A 'no SMT' variant is omitted when it would equal its parent."
    // The E-cores have no SMT siblings, so "E-cores no SMT" must be absent -- or, if
    // present, identical to "E-cores".
    const cd::Mask* eNo = MaskNamed(m, L"E-cores no SMT");
    const cd::Mask* e = MaskNamed(m, L"E-cores");
    if (eNo && e) {
        CHECK_EQ(eNo->ids, e->ids);
    } else {
        CHECK(eNo == nullptr);
    }

    EXPECT_MASK(m, L"All", IdRange(256u, 24, 1u));
}

void Test_A4_SymmetricDualCcd() {
    Case("A4 symmetric dual-CCD (two domains, identical L3)");
    cd::Topology t = MakeSymmetricDualCcd();

    CHECK_EQ((int)t.domains.size(), 2);
    CHECK_EQ(t.kind, cd::TopologyKind::MultiCcdSymmetric);
    CHECK_EQ(t.confidence, cd::Confidence::Medium);
    CHECK_EQ(t.defaultGameMask, L"CCD0");

    std::vector<cd::Mask> m = cd::DeriveMasks(t);
    EXPECT_MASK(m, L"CCD0", IdRange(256u, 16, 1u));
    EXPECT_MASK(m, L"CCD1", IdRange(272u, 16, 1u));
    EXPECT_MASK(m, L"CCD0 no SMT", IdRange(256u, 8, 2u));

    // The X3D vocabulary must NOT appear on a symmetric machine.
    CHECK(MaskNamed(m, L"Cache") == nullptr);
    CHECK(MaskNamed(m, L"Freq") == nullptr);
}

void Test_A5_SingleDomain() {
    Case("A5 single domain (8C/16T)");
    cd::Topology t = MakeSingleDomain();

    CHECK_EQ((int)t.domains.size(), 1);
    CHECK_EQ(t.kind, cd::TopologyKind::SingleDomain);
    CHECK_EQ(t.confidence, cd::Confidence::None);
    // THE EXCEPTION, and it is deliberate: topology.h keeps "All no SMT" here because "All"
    // is every processor, i.e. the same thing as no assignment at all, and the shipped
    // profile would do nothing on a machine that has no split to exploit.
    CHECK_EQ(t.defaultGameMask, L"All no SMT");
    CHECK_EQ(t.defaultHeavyMask, L"All");

    std::vector<cd::Mask> m = cd::DeriveMasks(t);
    CHECK_EQ((int)m.size(), 2);
    EXPECT_MASK(m, L"All", IdRange(256u, 16, 1u));
    EXPECT_MASK(m, L"All no SMT", IdRange(256u, 8, 2u));
}

void Test_A6_NoSmtMachine() {
    Case("A6 no SMT anywhere: no 'no SMT' mask may duplicate its parent");
    cd::Topology t = MakeNoSmtMachine();
    std::vector<cd::Mask> m = cd::DeriveMasks(t);

    int scanned = 0;
    for (size_t i = 0; i < m.size(); ++i) {
        if (!EndsWith(m[i].name, L" no SMT")) continue;
        std::wstring parentName = m[i].name.substr(0, m[i].name.size() - 7);
        const cd::Mask* parent = MaskNamed(m, parentName);
        if (!parent) continue;
        ++scanned;
        CHECK_NE(m[i].ids, parent->ids);
    }
    // Informational: on a no-SMT machine the expected count is zero.
    std::printf("       (no-SMT variants present with a parent: %d)\n", scanned);

    // POSITIVE CONTROL for the scanner above: on the SMT reference machine the very same
    // loop must find several parented "no SMT" masks. Without this, A6 could pass simply
    // because EndsWith() never matched anything.
    cd::Topology ref = MakeReference(false);
    std::vector<cd::Mask> rm = cd::DeriveMasks(ref);
    int refScanned = 0;
    for (size_t i = 0; i < rm.size(); ++i) {
        if (!EndsWith(rm[i].name, L" no SMT")) continue;
        std::wstring parentName = rm[i].name.substr(0, rm[i].name.size() - 7);
        if (MaskNamed(rm, parentName)) ++refScanned;
    }
    CHECK_EQ(refScanned, 3);  // Cache no SMT, Freq no SMT, All no SMT
}

void Test_A7_ReduceToNoSmt() {
    Case("A7 ReduceToNoSmt over all 32 ids of the reference machine");
    cd::Topology t = MakeReference(false);
    std::vector<ULONG> all = IdRange(256u, 32, 1u);
    std::vector<ULONG> got = cd::ReduceToNoSmt(t, all);
    CHECK_EQ(got, IdRange(256u, 16, 2u));

    // A subset must reduce within itself only.
    std::vector<ULONG> second = IdRange(272u, 16, 1u);
    CHECK_EQ(cd::ReduceToNoSmt(t, second), IdRange(272u, 8, 2u));

    // An already-reduced list is a fixed point.
    CHECK_EQ(cd::ReduceToNoSmt(t, IdRange(256u, 16, 2u)), IdRange(256u, 16, 2u));
}

// topology.h: "The chosen name is checked against DeriveMasks before it is stored, because a
// default naming a mask that DeriveMasks did not emit would leave the engine with nothing to
// apply." That is an invariant over EVERY topology, so it is asserted over every one this
// file can build rather than case by case.
void Test_A8_DefaultGameMaskAlwaysExists() {
    Case("A8 defaultGameMask names a mask DeriveMasks actually emits, on every topology");

    // `split` is whether this topology HAS a group to isolate. Only those get the plain-group
    // rule; SingleDomain deliberately keeps its "no SMT" default - see A5 and topology.h.
    struct Named { const char* what; bool split; cd::Topology t; };
    std::vector<Named> all;
    { Named n; n.what="reference";         n.split=true;  n.t=MakeReference(false);   all.push_back(n); }
    { Named n; n.what="reference/swapped"; n.split=true;  n.t=MakeReference(true);    all.push_back(n); }
    { Named n; n.what="intel hybrid";      n.split=true;  n.t=MakeIntelHybrid();      all.push_back(n); }
    { Named n; n.what="symmetric dual";    n.split=true;  n.t=MakeSymmetricDualCcd(); all.push_back(n); }
    { Named n; n.what="single domain";     n.split=false; n.t=MakeSingleDomain();     all.push_back(n); }
    { Named n; n.what="no SMT anywhere";   n.split=true;  n.t=MakeNoSmtMachine();     all.push_back(n); }

    for (size_t i = 0; i < all.size(); ++i) {
        const std::vector<cd::Mask> m = cd::DeriveMasks(all[i].t);
        std::printf("       (%s -> game default \"%s\")\n", all[i].what,
                    Utf8(all[i].t.defaultGameMask).c_str());
        ++g_total;
        if (MaskNamed(m, all[i].t.defaultGameMask) == nullptr) {
            Fail(__FILE__, __LINE__, all[i].what, Show(all[i].t.defaultGameMask),
                 "one of " + Show(MaskNames(m)));
        }
        // And where there IS a group to isolate it must be the PLAIN one, never the reduced
        // one - the operator decision this whole case exists to protect. A machine with no
        // split is the documented exception and is asserted the other way, in A5.
        if (all[i].split) {
            CHECK(!EndsWith(all[i].t.defaultGameMask, L" no SMT"));
        } else {
            CHECK(EndsWith(all[i].t.defaultGameMask, L" no SMT"));
        }
    }
}

// The fallback itself, which needs a topology no real machine produces: the LARGEST-L3
// domain names logical processors that have no CpuSetEntry, so BaseGroups still labels it
// "Cache" and DeriveMasks then emits nothing for it. Before the default was validated this
// wrote "Cache no SMT" into every shipped profile on such a machine and the engine had
// nothing to look up. Built by hand, deliberately NOT through FinishTopology, because
// FinishTopology derives domains from entries and so cannot express this.
void Test_A9_DefaultFallsBackWhenTheGroupHasNoMask() {
    Case("A9 a first group with no derivable mask falls back to one that exists");
    cd::Topology t;
    for (ULONG lp = 0; lp < 8; ++lp)
        t.entries.push_back(Entry(lp, (lp / 2u) * 2u, 8u, (BYTE)0, 32768ull));

    cd::LlcDomain phantom;          // the big-cache domain, with no entries behind it
    phantom.index = 0;
    phantom.l3Bytes = 98304ull * 1024ull;
    phantom.lps.push_back(100u);
    phantom.lps.push_back(101u);

    cd::LlcDomain real;
    real.index = 8;
    real.l3Bytes = 32768ull * 1024ull;
    for (ULONG lp = 0; lp < 8; ++lp) real.lps.push_back(lp);

    t.domains.push_back(phantom);
    t.domains.push_back(real);
    t.groupCount = 1;
    t.totalLogicalProcessors = (int)t.entries.size();
    cd::ClassifyTopology(t);

    // The fixture really is the awkward shape: asymmetric, so "Cache" IS the label of the
    // first group, and DeriveMasks really does refuse to emit it.
    CHECK_EQ(t.kind, cd::TopologyKind::AmdAsymmetricCache);
    const std::vector<cd::Mask> m = cd::DeriveMasks(t);
    CHECK(MaskNamed(m, L"Cache") == nullptr);
    CHECK(MaskNamed(m, L"Cache no SMT") == nullptr);

    // So the default must not be either of them, and must be something that exists.
    CHECK_NE(t.defaultGameMask, std::wstring(L"Cache"));
    CHECK_EQ(t.defaultGameMask, L"All");
    CHECK(MaskNamed(m, t.defaultGameMask) != nullptr);
}

// ===========================================================================
// B. Config.
// ===========================================================================

std::wstring ToLf(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n') continue;  // drop CR of a CRLF
            o.push_back(L'\n');                                   // lone CR
        } else {
            o.push_back(s[i]);
        }
    }
    return o;
}

std::wstring ToCrlf(const std::wstring& lf) {
    std::wstring o;
    o.reserve(lf.size() * 2);
    for (size_t i = 0; i < lf.size(); ++i) {
        if (lf[i] == L'\n') o.push_back(L'\r');
        o.push_back(lf[i]);
    }
    return o;
}

std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> v;
    std::wstring cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\n') {
            v.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    v.push_back(cur);
    return v;
}

std::wstring JoinLines(const std::vector<std::wstring>& v) {
    std::wstring o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o.push_back(L'\n');
        o += v[i];
    }
    return o;
}

int LineContaining(const std::vector<std::wstring>& lines, const std::wstring& needle) {
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].find(needle) != std::wstring::npos) return (int)i;
    return -1;
}

bool AnyContains(const std::vector<std::wstring>& v, const std::wstring& needle) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].find(needle) != std::wstring::npos) return true;
    return false;
}

bool UnknownHasLine(const cd::Config& c, const std::wstring& needle) {
    std::map<std::wstring, std::vector<std::wstring> >::const_iterator it;
    for (it = c.unknown.begin(); it != c.unknown.end(); ++it)
        if (AnyContains(it->second, needle)) return true;
    return false;
}

const wchar_t* kSigMarker = L"amd:2:16:16:98304:32768";
const wchar_t* kOddPath = L"C:\\Games\\odd=path\\cs2.exe";
// A plausible FILETIME: far outside 32 bits, so a 64-bit path is genuinely required.
const ULONGLONG kStampMarker = 133700000000000000ull;

cd::Config MakeRoundTripConfig() {
    cd::Config c;
    c.version = 1;
    c.startWithWindows = true;
    c.pollMs = 300;
    c.notifications = true;
    c.paused = true;
    c.firstRunDone = true;
    c.topologySignature = kSigMarker;

    cd::Mask m1;
    m1.name = L"Cache no SMT";  // B3: a mask name containing spaces
    m1.ids = IdRange(256u, 8, 2u);
    m1.derived = true;
    cd::Mask m2;
    m2.name = L"Freq";
    m2.ids = IdRange(272u, 16, 1u);
    m2.derived = false;  // hand-edited
    c.masks.push_back(m1);
    c.masks.push_back(m2);

    cd::Profile p1;
    p1.name = L"Overwatch";
    p1.enabled = true;
    p1.game = L"Overwatch.exe";
    p1.gameMask = L"Cache no SMT";
    p1.heavy.push_back(L"OBS.exe");  // B1: a heavy list of three entries
    p1.heavy.push_back(L"chrome.exe");
    p1.heavy.push_back(L"Discord.exe");
    p1.heavyMask = L"Freq";
    p1.autoPin = true;
    p1.autoPinPercent = 12;
    p1.autoPinSeconds = 7;   // deliberately NOT the default: see ExpectConfigEqual
    p1.isAllGames = false;
    p1.lastUsed = kStampMarker;

    cd::Profile p2;
    p2.name = L"Counter Strike 2";  // B1: a profile name with spaces
    p2.enabled = false;
    p2.game = kOddPath;  // B2: a value containing '='
    p2.gameMask = L"Freq";
    p2.heavyMask = L"Cache no SMT";
    p2.autoPin = false;
    p2.autoPinPercent = 3;
    p2.autoPinSeconds = 42;  // ditto
    p2.isAllGames = true;    // both values of all_games are exercised by the pair
    p2.lastUsed = 0;         // and both a set and an unset stamp

    c.profiles.push_back(p1);
    c.profiles.push_back(p2);

    c.exclusions.push_back(L"EasyAntiCheat.exe");
    c.exclusions.push_back(L"audiodg.exe");

    std::vector<std::wstring> futureLines;
    futureLines.push_back(L"futurekey=futurevalue");
    futureLines.push_back(L"anotherfuture=1|2|3");
    c.unknown[L"futuresection"] = futureLines;

    return c;
}

void ExpectConfigEqual(const cd::Config& got, const cd::Config& want, const char* what) {
    std::printf("       comparing: %s\n", what);
    CHECK_EQ(got.version, want.version);
    CHECK_EQ(got.startWithWindows, want.startWithWindows);
    CHECK_EQ(got.pollMs, want.pollMs);
    CHECK_EQ(got.notifications, want.notifications);
    CHECK_EQ(got.paused, want.paused);
    CHECK_EQ(got.vcacheOriginalStart, want.vcacheOriginalStart);
    CHECK_EQ(got.firstRunDone, want.firstRunDone);
    CHECK_EQ(got.topologySignature, want.topologySignature);

    CHECK_EQ((int)got.masks.size(), (int)want.masks.size());
    for (size_t i = 0; i < want.masks.size() && i < got.masks.size(); ++i) {
        CHECK_EQ(got.masks[i].name, want.masks[i].name);
        CHECK_EQ(got.masks[i].ids, want.masks[i].ids);
        CHECK_EQ(got.masks[i].derived, want.masks[i].derived);
    }

    CHECK_EQ((int)got.profiles.size(), (int)want.profiles.size());
    for (size_t i = 0; i < want.profiles.size() && i < got.profiles.size(); ++i) {
        const cd::Profile& a = got.profiles[i];
        const cd::Profile& b = want.profiles[i];
        CHECK_EQ(a.name, b.name);
        CHECK_EQ(a.enabled, b.enabled);
        CHECK_EQ(a.game, b.game);
        CHECK_EQ(a.gameMask, b.gameMask);
        CHECK_EQ(a.heavy, b.heavy);
        CHECK_EQ(a.heavyMask, b.heavyMask);
        CHECK_EQ(a.autoPin, b.autoPin);
        CHECK_EQ(a.autoPinPercent, b.autoPinPercent);
        CHECK_EQ(a.isAllGames, b.isAllGames);
        CHECK_EQ(a.lastUsed, b.lastUsed);
        // autoPinSeconds is NOT compared against the original on purpose. config.h now
        // declares it not user-editable and Config no longer writes it, so a parsed profile
        // must come back holding the struct default whatever the original said. Asserting
        // THAT is the stronger check: a build that started persisting the key again fails
        // here, and so does one that persists it as some other value.
        CHECK_EQ(a.autoPinSeconds, cd::Profile().autoPinSeconds);
    }

    CHECK_EQ(got.exclusions, want.exclusions);

    CHECK_EQ((int)got.unknown.size(), (int)want.unknown.size());
    std::map<std::wstring, std::vector<std::wstring> >::const_iterator it;
    for (it = want.unknown.begin(); it != want.unknown.end(); ++it) {
        std::map<std::wstring, std::vector<std::wstring> >::const_iterator f =
            got.unknown.find(it->first);
        ++g_total;
        if (f == got.unknown.end()) {
            Fail(__FILE__, __LINE__, "unknown section present", "<missing>",
                 Show(it->first));
            continue;
        }
        CHECK_EQ(f->second, it->second);
    }
}

void Test_B1_B2_B3_RoundTrip() {
    Case("B1/B2/B3 Config round-trip through SerializeConfig -> ParseConfig");
    cd::Config c = MakeRoundTripConfig();
    std::wstring text = cd::SerializeConfig(c);
    CHECK(!text.empty());

    cd::Config out;
    std::wstring err;
    bool ok = cd::ParseConfig(text, out, &err);
    CHECK(ok);
    if (!ok) std::printf("       parse error: %s\n", Utf8(err).c_str());

    ExpectConfigEqual(out, c, "round-tripped config vs original");

    // B2 explicitly: split on the FIRST '=' only, so a value containing '=' survives.
    if (out.profiles.size() >= 2) CHECK_EQ(out.profiles[1].game, kOddPath);
    // B3 explicitly: a mask name containing spaces round-trips.
    CHECK(MaskNamed(out.masks, L"Cache no SMT") != nullptr);
    // and a profile name containing spaces.
    CHECK(out.FindProfile(L"Counter Strike 2") != nullptr);

    // Fixture control for the autoPinSeconds assertion in ExpectConfigEqual: the values put
    // into the fixture really are different from the struct default, so "the parsed value is
    // the default" is a claim that could have failed.
    CHECK_NE(c.profiles[0].autoPinSeconds, cd::Profile().autoPinSeconds);
    CHECK_NE(c.profiles[1].autoPinSeconds, cd::Profile().autoPinSeconds);
    // and the key really is absent from the text, not merely ignored on the way back in.
    CHECK(text.find(L"auto_pin_seconds") == std::wstring::npos);
}

void Test_B10_AllGamesAndLastUsedRoundTrip() {
    Case("B10 all_games and last_used round-trip through SerializeConfig -> ParseConfig");
    cd::Config c = MakeRoundTripConfig();
    std::wstring text = cd::SerializeConfig(c);

    // The keys are actually emitted (a parser that defaulted both would otherwise pass).
    CHECK(text.find(L"all_games=") != std::wstring::npos);
    CHECK(text.find(L"last_used=") != std::wstring::npos);
    CHECK(text.find(L"133700000000000000") != std::wstring::npos);

    cd::Config out;
    std::wstring err;
    bool ok = cd::ParseConfig(text, out, &err);
    CHECK(ok);
    if (!ok) std::printf("       parse error: %s\n", Utf8(err).c_str());

    CHECK_EQ((int)out.profiles.size(), 2);
    if (out.profiles.size() == 2) {
        CHECK_EQ(out.profiles[0].isAllGames, false);
        CHECK_EQ(out.profiles[0].lastUsed, kStampMarker);
        CHECK_EQ(out.profiles[1].isAllGames, true);
        CHECK_EQ(out.profiles[1].lastUsed, 0ull);
        // AllGamesProfile() finds it, and finds the FIRST one.
        const cd::Profile* ag = out.AllGamesProfile();
        CHECK(ag != nullptr);
        if (ag) CHECK_EQ(ag->name, L"Counter Strike 2");
    }

    // An OLDER config - no all_games, no last_used anywhere - still loads, with both fields
    // at their documented defaults rather than as preserved unknown keys.
    std::vector<std::wstring> lines = SplitLines(ToLf(text));
    std::vector<std::wstring> stripped;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(L"all_games=") != std::wstring::npos) continue;
        if (lines[i].find(L"last_used=") != std::wstring::npos) continue;
        stripped.push_back(lines[i]);
    }
    CHECK((int)stripped.size() < (int)lines.size());

    cd::Config old;
    CHECK(cd::ParseConfig(JoinLines(stripped), old, &err));
    CHECK_EQ((int)old.profiles.size(), 2);
    if (old.profiles.size() == 2) {
        CHECK_EQ(old.profiles[0].isAllGames, false);
        CHECK_EQ(old.profiles[0].lastUsed, 0ull);
        CHECK_EQ(old.profiles[1].isAllGames, false);
        CHECK_EQ(old.profiles[1].lastUsed, 0ull);
    }
    CHECK(old.AllGamesProfile() == nullptr);
}

void Test_B4_LineEndingsAndBom() {
    Case("B4 CRLF, LF and a UTF-8 BOM all parse to the same Config");
    cd::Config c = MakeRoundTripConfig();
    std::wstring lf = ToLf(cd::SerializeConfig(c));
    std::wstring crlf = ToCrlf(lf);
    std::wstring bom = std::wstring(L"\xFEFF") + crlf;

    // Fixture sanity: the three inputs really are different byte sequences.
    CHECK_NE(lf, crlf);
    CHECK_NE(crlf, bom);

    cd::Config a, b, d;
    std::wstring e1, e2, e3;
    bool ok1 = cd::ParseConfig(lf, a, &e1);
    bool ok2 = cd::ParseConfig(crlf, b, &e2);
    bool ok3 = cd::ParseConfig(bom, d, &e3);
    CHECK(ok1);
    CHECK(ok2);
    CHECK(ok3);
    if (!ok1) std::printf("       LF error  : %s\n", Utf8(e1).c_str());
    if (!ok2) std::printf("       CRLF error: %s\n", Utf8(e2).c_str());
    if (!ok3) std::printf("       BOM error : %s\n", Utf8(e3).c_str());

    ExpectConfigEqual(a, c, "LF input vs original");
    ExpectConfigEqual(b, c, "CRLF input vs original");
    ExpectConfigEqual(d, c, "BOM+CRLF input vs original");
}

void Test_B5_MalformedLineInTheMiddle() {
    Case("B5 a malformed line in the middle does not abort the parse");
    cd::Config c = MakeRoundTripConfig();
    std::vector<std::wstring> lines = SplitLines(ToLf(cd::SerializeConfig(c)));

    int a = LineContaining(lines, kSigMarker);
    int b = LineContaining(lines, kOddPath);
    CHECK(a >= 0);
    CHECK(b >= 0);
    if (a < 0 || b < 0) return;

    int lo = (a < b) ? a : b;
    int hi = (a < b) ? b : a;
    CHECK(hi > lo);
    if (hi <= lo) return;

    // Insert garbage strictly between the two marker lines: no section header, no comment,
    // no '='. Nothing is removed, so both markers remain on either side of the damage.
    lines.insert(lines.begin() + (lo + 1), L"this line is not valid ini at all");

    cd::Config out;
    std::wstring err;
    bool ok = cd::ParseConfig(JoinLines(lines), out, &err);
    CHECK(ok);
    if (!ok) std::printf("       parse error: %s\n", Utf8(err).c_str());

    // The key BEFORE the damage.
    CHECK_EQ(out.topologySignature, kSigMarker);
    // The key AFTER the damage.
    const cd::Profile* p = out.FindProfile(L"Counter Strike 2");
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->game, kOddPath);
    // and nothing else was lost on the way.
    CHECK_EQ((int)out.profiles.size(), 2);
    CHECK_EQ((int)out.masks.size(), 2);
}

void Test_B6_UnknownPreserved() {
    Case("B6 unknown sections and unknown keys survive parse/serialize");
    cd::Config c = MakeRoundTripConfig();
    std::vector<std::wstring> lines = SplitLines(ToLf(cd::SerializeConfig(c)));

    // An unknown KEY inside a section this version does recognise.
    int firstSection = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].empty() && lines[i][0] == L'[') {
            firstSection = (int)i;
            break;
        }
    }
    CHECK(firstSection >= 0);
    if (firstSection < 0) return;
    lines.insert(lines.begin() + (firstSection + 1), L"zz_unknown_key=zz_unknown_value");

    // An unknown SECTION entirely.
    lines.push_back(L"[section_from_the_future]");
    lines.push_back(L"tomorrow_key=tomorrow_value");
    lines.push_back(L"");

    cd::Config parsed;
    std::wstring err;
    bool ok = cd::ParseConfig(JoinLines(lines), parsed, &err);
    CHECK(ok);
    if (!ok) std::printf("       parse error: %s\n", Utf8(err).c_str());

    CHECK(UnknownHasLine(parsed, L"zz_unknown_key=zz_unknown_value"));
    CHECK(UnknownHasLine(parsed, L"tomorrow_key=tomorrow_value"));

    // config.h: "Re-emitted on save" -- an older binary cannot destroy a newer config.
    std::wstring again = cd::SerializeConfig(parsed);
    CHECK(again.find(L"zz_unknown_key=zz_unknown_value") != std::wstring::npos);
    CHECK(again.find(L"tomorrow_key=tomorrow_value") != std::wstring::npos);
    CHECK(again.find(L"section_from_the_future") != std::wstring::npos);

    // And a second cycle is stable.
    cd::Config parsed2;
    CHECK(cd::ParseConfig(again, parsed2, &err));
    CHECK(UnknownHasLine(parsed2, L"zz_unknown_key=zz_unknown_value"));
    CHECK(UnknownHasLine(parsed2, L"tomorrow_key=tomorrow_value"));
}

cd::Config MakeRepairConfig(const cd::Topology& t, int pollMs, int autoPinPercent) {
    cd::Config c;
    c.version = 1;
    c.pollMs = pollMs;
    c.masks = cd::DeriveMasks(t);

    cd::Profile good;
    good.name = L"GoodProfile";
    good.enabled = true;
    good.game = L"Good.exe";
    good.gameMask = L"Cache";
    good.heavyMask = L"Freq";
    good.autoPin = true;
    good.autoPinPercent = autoPinPercent;
    good.autoPinSeconds = 5;

    cd::Profile bad;
    bad.name = L"BadProfile";
    bad.enabled = true;
    bad.game = L"Bad.exe";
    bad.gameMask = L"NoSuchMaskAnywhere";
    bad.heavyMask = L"Freq";
    bad.autoPin = false;
    bad.autoPinPercent = 8;
    bad.autoPinSeconds = 5;

    c.profiles.push_back(good);
    c.profiles.push_back(bad);
    c.exclusions = cd::DefaultExclusions();
    return c;
}

void Test_B7_ValidateAndRepair() {
    Case("B7 ValidateAndRepair clamps and drops");
    cd::Topology t = MakeReference(false);

    // Low end: pollMs 50 -> 100, autoPinPercent 0 -> 1, and BadProfile dropped.
    {
        cd::Config c = MakeRepairConfig(t, 50, 0);
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);
        CHECK(!rep.empty());
        CHECK_EQ(c.pollMs, 100);
        CHECK_EQ((int)c.profiles.size(), 1);
        if (c.profiles.size() == 1) {
            CHECK_EQ(c.profiles[0].name, L"GoodProfile");
            CHECK_EQ(c.profiles[0].autoPinPercent, 1);
        }
        CHECK(AnyContains(rep, L"BadProfile"));
        if (!AnyContains(rep, L"BadProfile")) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
        }
    }

    // High end: pollMs 9999 -> 2000, autoPinPercent 500 -> 100.
    {
        cd::Config c = MakeRepairConfig(t, 9999, 500);
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);
        CHECK(!rep.empty());
        CHECK_EQ(c.pollMs, 2000);
        CHECK_EQ((int)c.profiles.size(), 1);
        if (c.profiles.size() == 1) {
            CHECK_EQ(c.profiles[0].name, L"GoodProfile");
            CHECK_EQ(c.profiles[0].autoPinPercent, 100);
        }
    }

    // A config that is already valid produces an EMPTY repair list. Without this control,
    // "rep is non-empty" above could be satisfied by an implementation that always reports.
    {
        cd::Config c = cd::DefaultConfig(t);
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);
        CHECK(rep.empty());
        if (!rep.empty()) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       unexpected repair[%d]: %s\n", (int)i,
                            Utf8(rep[i]).c_str());
        }
    }
}

void Test_B8_DefaultConfig() {
    // REWRITTEN. This case previously asserted ONE profile, "Overwatch", enabled == false.
    // Both halves encoded behaviour the operator has since changed: profiles now ship
    // ENABLED with auto-pin ON, and a second "All Games" profile ships alongside the worked
    // example. config.h states the All Games profile is always considered LAST, so its
    // position in the vector is part of the contract and is asserted here.
    Case("B8 DefaultConfig(referenceTopology)");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);

    CHECK_EQ((int)c.profiles.size(), 2);
    if (c.profiles.size() == 2) {
        CHECK_EQ(c.profiles[0].name, L"Overwatch");
        CHECK_EQ(c.profiles[0].enabled, true);
        CHECK_EQ(c.profiles[0].autoPin, true);
        CHECK_EQ(c.profiles[0].isAllGames, false);
        CHECK_EQ(c.profiles[0].game, L"Overwatch.exe");
        CHECK_EQ(c.profiles[0].lastUsed, 0ull);

        // LAST in the vector, by contract.
        CHECK_EQ(c.profiles[1].name, L"All Games");
        CHECK_EQ(c.profiles[1].isAllGames, true);
        CHECK_EQ(c.profiles[1].enabled, true);
        CHECK_EQ(c.profiles[1].autoPin, true);
        CHECK_EQ(c.profiles[1].game, L"");
        CHECK(!c.profiles[1].heavy.empty());
        CHECK_EQ(c.profiles[1].heavy, c.profiles[0].heavy);
        CHECK_EQ(c.profiles[1].lastUsed, 0ull);
    }

    // Exactly one All Games profile, and AllGamesProfile() finds it.
    {
        int allGamesCount = 0;
        for (size_t i = 0; i < c.profiles.size(); ++i)
            if (c.profiles[i].isAllGames) ++allGamesCount;
        CHECK_EQ(allGamesCount, 1);
        const cd::Profile* ag = c.AllGamesProfile();
        CHECK(ag != nullptr);
        if (ag) CHECK_EQ(ag->name, L"All Games");
    }

    CHECK_EQ(c.firstRunDone, false);
    CHECK(!c.masks.empty());
    CHECK(!c.exclusions.empty());
    // The masks must be this machine's, and EVERY shipped profile must name ones that exist -
    // otherwise ValidateAndRepair would drop the profile on the first load.
    for (size_t i = 0; i < c.profiles.size(); ++i) {
        CHECK(c.FindMask(c.profiles[i].gameMask) != nullptr);
        CHECK(c.FindMask(c.profiles[i].heavyMask) != nullptr);
    }

    // Nothing shipped is stamped as used, so the display list is all one group.
    {
        int sep = 99;
        std::vector<size_t> order = c.ProfilesForDisplay(&sep);
        CHECK_EQ(sep, -1);
        CHECK_EQ((int)order.size(), (int)c.profiles.size());
    }
}

void Test_B11_ProfilesForDisplay() {
    Case("B11 ProfilesForDisplay: used profiles newest-first, then the rest in vector order");
    cd::Config c;

    const wchar_t* names[4] = {L"P0", L"P1", L"P2", L"P3"};
    const ULONGLONG stamps[4] = {0ull, 100ull, 300ull, 0ull};
    for (int i = 0; i < 4; ++i) {
        cd::Profile p;
        p.name = names[i];
        p.lastUsed = stamps[i];
        c.profiles.push_back(p);
    }

    int sep = -99;
    std::vector<size_t> order = c.ProfilesForDisplay(&sep);

    CHECK_EQ((int)order.size(), 4);
    CHECK_EQ(sep, 2);
    if (order.size() == 4) {
        // 300 first, then 100 - newest at the top, not vector order.
        CHECK_EQ((int)order[0], 2);
        CHECK_EQ((int)order[1], 1);
        // then the two unstamped, in VECTOR order.
        CHECK_EQ((int)order[2], 0);
        CHECK_EQ((int)order[3], 3);
    }

    // Every index appears exactly once: an ordering that loses or duplicates a profile would
    // otherwise satisfy the positional checks above on a shorter list.
    {
        std::vector<size_t> seen = order;
        std::sort(seen.begin(), seen.end());
        for (int i = 0; i < 4 && i < (int)seen.size(); ++i) CHECK_EQ((int)seen[i], i);
    }

    Case("B11b every stamp 0 -> separatorAfter is -1 and the order is untouched");
    cd::Config d;
    for (int i = 0; i < 4; ++i) {
        cd::Profile p;
        p.name = names[i];
        p.lastUsed = 0;
        d.profiles.push_back(p);
    }
    int sep2 = 99;
    std::vector<size_t> order2 = d.ProfilesForDisplay(&sep2);
    CHECK_EQ(sep2, -1);
    CHECK_EQ((int)order2.size(), 4);
    for (int i = 0; i < 4 && i < (int)order2.size(); ++i) CHECK_EQ((int)order2[i], i);

    Case("B11c equal stamps break ties by vector order (deterministic)");
    cd::Config e;
    for (int i = 0; i < 4; ++i) {
        cd::Profile p;
        p.name = names[i];
        p.lastUsed = 500ull;   // all identical
        e.profiles.push_back(p);
    }
    int sep3 = -99;
    std::vector<size_t> order3 = e.ProfilesForDisplay(&sep3);
    CHECK_EQ(sep3, 4);
    CHECK_EQ((int)order3.size(), 4);
    for (int i = 0; i < 4 && i < (int)order3.size(); ++i) CHECK_EQ((int)order3[i], i);

    Case("B11d an empty profile list is not a crash and reports no separator");
    cd::Config f;
    int sep4 = 99;
    std::vector<size_t> order4 = f.ProfilesForDisplay(&sep4);
    CHECK_EQ(sep4, -1);
    CHECK_EQ((int)order4.size(), 0);

    Case("B11e a null separatorAfter is accepted");
    std::vector<size_t> order5 = c.ProfilesForDisplay(nullptr);
    CHECK_EQ((int)order5.size(), 4);
}

void Test_B12_ValidateAndRepairNewRules() {
    Case("B12 ValidateAndRepair forces autoPinSeconds and does NOT report it");
    cd::Topology t = MakeReference(false);
    {
        cd::Config c = cd::DefaultConfig(t);
        // Values on both sides of the fixed debounce, and one the old clamp would have
        // accepted silently, so "forced" cannot be confused with "clamped to a range".
        c.profiles[0].autoPinSeconds = 900;
        c.profiles[1].autoPinSeconds = 5;
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);

        CHECK_EQ((int)c.profiles.size(), 2);
        for (size_t i = 0; i < c.profiles.size(); ++i)
            CHECK_EQ(c.profiles[i].autoPinSeconds, cd::kAutoPinDebounceTicks);

        // It is not the user's setting any more, so it is not reported as a repair. The
        // whole list must still be empty here: DefaultConfig is otherwise valid.
        CHECK(rep.empty());
        CHECK(!AnyContains(rep, L"auto-pin hold"));
        CHECK(!AnyContains(rep, L"seconds"));
        if (!rep.empty()) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       unexpected repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
        }
    }

    Case("B12b a second All Games profile is demoted, and that IS reported");
    {
        cd::Config c = cd::DefaultConfig(t);
        cd::Profile extra = c.profiles[1];       // a copy of the real All Games profile
        extra.name = L"Second All Games";
        extra.isAllGames = true;
        c.profiles.push_back(extra);
        CHECK_EQ((int)c.profiles.size(), 3);

        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);

        CHECK_EQ((int)c.profiles.size(), 3);     // demoted, not dropped
        if (c.profiles.size() == 3) {
            CHECK_EQ(c.profiles[1].name, L"All Games");
            CHECK_EQ(c.profiles[1].isAllGames, true);   // the FIRST one is kept
            CHECK_EQ(c.profiles[2].name, L"Second All Games");
            CHECK_EQ(c.profiles[2].isAllGames, false);  // the rest are cleared
        }
        CHECK(!rep.empty());
        CHECK(AnyContains(rep, L"Second All Games"));
        if (!AnyContains(rep, L"Second All Games")) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
        }
        // and AllGamesProfile() now finds exactly the surviving one.
        const cd::Profile* ag = c.AllGamesProfile();
        CHECK(ag != nullptr);
        if (ag) CHECK_EQ(ag->name, L"All Games");
    }

    Case("B12c an All Games profile carrying a game name has it cleared, and reported");
    {
        cd::Config c = cd::DefaultConfig(t);
        c.profiles[1].game = L"Overwatch.exe";
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);

        CHECK_EQ((int)c.profiles.size(), 2);
        if (c.profiles.size() == 2) {
            CHECK_EQ(c.profiles[1].isAllGames, true);
            CHECK_EQ(c.profiles[1].game, L"");
            // The ordinary profile's game is untouched.
            CHECK_EQ(c.profiles[0].game, L"Overwatch.exe");
        }
        CHECK(!rep.empty());
        CHECK(AnyContains(rep, L"All Games"));
    }
}

void Test_B13_MarkProfileUsed() {
    Case("B13 MarkProfileUsed stamps the named profile and is a no-op for an unknown name");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    CHECK_EQ((int)c.profiles.size(), 2);

    CHECK_EQ(c.profiles[0].lastUsed, 0ull);
    c.MarkProfileUsed(L"Overwatch", kStampMarker);
    CHECK_EQ(c.profiles[0].lastUsed, kStampMarker);
    CHECK_EQ(c.profiles[1].lastUsed, 0ull);   // only the named one moved

    // A later stamp replaces the earlier one.
    c.MarkProfileUsed(L"Overwatch", kStampMarker + 1ull);
    CHECK_EQ(c.profiles[0].lastUsed, kStampMarker + 1ull);

    // Unknown name: no crash, no profile added, no stamp changed.
    c.MarkProfileUsed(L"zz_no_such_profile_qqq", 12345ull);
    CHECK_EQ((int)c.profiles.size(), 2);
    CHECK_EQ(c.profiles[0].lastUsed, kStampMarker + 1ull);
    CHECK_EQ(c.profiles[1].lastUsed, 0ull);

    // The stamp drives the display order, so the used profile is now first.
    int sep = -99;
    std::vector<size_t> order = c.ProfilesForDisplay(&sep);
    CHECK_EQ(sep, 1);
    CHECK_EQ((int)order.size(), 2);
    if (order.size() == 2) {
        CHECK_EQ((int)order[0], 0);
        CHECK_EQ((int)order[1], 1);
    }

    // and it survives a save/load cycle.
    cd::Config back;
    std::wstring err;
    CHECK(cd::ParseConfig(cd::SerializeConfig(c), back, &err));
    const cd::Profile* p = back.FindProfile(L"Overwatch");
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->lastUsed, kStampMarker + 1ull);
}

void Test_B9_IsExcludedCaseInsensitive() {
    Case("B9 IsExcluded is case-insensitive and supports exclusion-only trailing wildcards");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);

    CHECK(c.IsExcluded(L"AUDIODG.EXE"));
    CHECK(c.IsExcluded(L"audiodg.exe"));
    CHECK(c.IsExcluded(L"AudioDg.Exe"));
    CHECK(!c.IsExcluded(L"zz_not_a_real_process_qqq.exe"));

    // A trailing '*' is a case-insensitive prefix wildcard for exclusions only.
    cd::Config wildcard;
    wildcard.exclusions.push_back(L"NVIDIA Broadcast*");
    CHECK(wildcard.IsExcluded(L"NVIDIA Broadcast 1.exe"));
    CHECK(wildcard.IsExcluded(L"nvidia broadcast.exe"));
    CHECK(!wildcard.IsExcluded(L"NVIDIA Broad.exe"));
    CHECK(!wildcard.IsExcluded(L"OBS.exe"));

    // A '*' anywhere else remains an ordinary literal character, and a bare '*' is ignored
    // so a malformed hand-edited entry cannot disable auto-pin for the whole machine.
    cd::Config literal;
    literal.exclusions.push_back(L"NVIDIA*Broadcast.exe");
    CHECK(literal.IsExcluded(L"NVIDIA*Broadcast.exe"));
    CHECK(!literal.IsExcluded(L"NVIDIA Camera Broadcast.exe"));
    literal.exclusions.clear();
    literal.exclusions.push_back(L"*");
    CHECK(!literal.IsExcluded(L"anything.exe"));

    // Ordinary entries retain their exact-match behavior; they are not prefixes.
    cd::Config exact;
    exact.exclusions.push_back(L"audiodg.exe");
    CHECK(exact.IsExcluded(L"AUDIODG.EXE"));
    CHECK(!exact.IsExcluded(L"audiodg.exe.backup"));

    cd::Config d;
    d.exclusions = cd::DefaultExclusions();
    CHECK(!d.exclusions.empty());
    CHECK(d.IsExcluded(L"AUDIODG.EXE"));
    CHECK(d.IsExcluded(L"NVIDIA Broadcast 1.exe"));
    CHECK(!d.IsExcluded(L"zz_not_a_real_process_qqq.exe"));
}

void Test_B14_DefaultExclusionsProtectAtieclxx() {
    Case("B14 default exclusions protect AMD display-driver client atieclxx only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"atieclxx.exe"));
    CHECK(!c.IsExcluded(L"atieclxx-helper.exe"));
}

void Test_B15_DefaultExclusionsProtectAtiesrxx() {
    Case("B15 default exclusions protect AMD display-driver service atiesrxx only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"atiesrxx.exe"));
    CHECK(!c.IsExcluded(L"atiesrxx-helper.exe"));
}

void Test_B16_DefaultExclusionsProtectAmdow() {
    Case("B16 default exclusions protect AMD display-driver helper amdow only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"amdow.exe"));
    CHECK(!c.IsExcluded(L"amdow-helper.exe"));
}

void Test_B17_DefaultExclusionsProtectAmd3dvcachePrefix() {
    Case("B17 default exclusions protect AMD V-Cache optimizer prefix only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"amd3dvcacheSvc.exe"));
    CHECK(!c.IsExcluded(L"amd3dvideo.exe"));
}

void Test_B18_DefaultExclusionsProtectAmdfendrPrefix() {
    Case("B18 default exclusions protect AMD Crash Defender prefix only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"amdfendrsr.exe"));
    CHECK(!c.IsExcluded(L"amddefender.exe"));
}

void Test_B19_DefaultExclusionsProtectAmdAppCompatPrefix() {
    Case("B19 default exclusions protect AMD app-compatibility service prefix only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"AmdAppCompatSvc.exe"));
    CHECK(!c.IsExcluded(L"AmdApplication.exe"));
}

void Test_B20_DefaultExclusionsProtectAmdPpkgPrefix() {
    Case("B20 default exclusions protect AMD provisioning-package service prefix only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"AmdPpkgSvc.exe"));
    CHECK(!c.IsExcluded(L"AmdPackageManager.exe"));
}

void Test_B21_DefaultExclusionsProtectAmdRsSourceExtension() {
    Case("B21 default exclusions protect AMD Radeon source extension only");
    const cd::Config c = cd::DefaultConfig(MakeReference(false));
    CHECK(c.IsExcluded(L"AMDRSSrcExt.exe"));
    CHECK(!c.IsExcluded(L"AMDRSSrcExt-helper.exe"));
}

// ===========================================================================
// C. ComputeDesired.
// ===========================================================================

void AddProc(cd::ProcessSnapshot& s, DWORD pid, DWORD ppid, const wchar_t* name,
             ULONGLONG creationTime, int aboveTicks, double cpuPct) {
    cd::ProcInfo p;
    p.pid = pid;
    p.ppid = ppid;
    p.name = name;
    p.fullPath = std::wstring(L"C:\\Apps\\") + name;
    p.creationTime = creationTime;
    p.cpuTime = 0;
    p.cpuPercent = cpuPct;
    p.aboveThresholdTicks = aboveTicks;
    p.accessDenied = false;
    access::Procs(s)[pid] = p;
}

bool Has(const std::map<DWORD, std::wstring>& m, DWORD pid) { return m.count(pid) != 0; }

std::wstring MaskOf(const std::map<DWORD, std::wstring>& m, DWORD pid) {
    std::map<DWORD, std::wstring>::const_iterator it = m.find(pid);
    if (it == m.end()) return std::wstring(L"<absent from result>");
    return it->second;
}

// pid 400  explorer's parent stand-in (not governed)
// pid 500  explorer.exe
// pid 1000 Overwatch.exe                 <- the game
// pid 1001 OverwatchChild.exe            <- descendant
// pid 1002 OverwatchGrandchild.exe       <- descendant of 1001
// pid 1003 EasyAntiCheat.exe             <- descendant, but EXCLUDED
// pid 1004 OBS.exe                       <- descendant AND on the heavy list (precedence)
// pid 3000 OBS.exe                       <- heavy, not a descendant
// pid 3001 audiodg.exe                   <- heavy AND excluded (heavy wins)
// pid 4    System                        <- fake descendant, must never be governed
// pid 0    Idle                          <- fake descendant, must never be governed
cd::ProcessSnapshot MakeGameSnapshot() {
    cd::ProcessSnapshot s;
    AddProc(s, 400, 1, L"parent.exe", 50, 0, 0.0);
    AddProc(s, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s, 1000, 500, L"Overwatch.exe", 200, 0, 0.0);
    AddProc(s, 1001, 1000, L"OverwatchChild.exe", 300, 0, 0.0);
    AddProc(s, 1002, 1001, L"OverwatchGrandchild.exe", 400, 0, 0.0);
    AddProc(s, 1003, 1000, L"EasyAntiCheat.exe", 350, 0, 0.0);
    AddProc(s, 1004, 1000, L"OBS.exe", 360, 0, 0.0);
    AddProc(s, 2000, 500, L"notepad.exe", 120, 0, 0.0);
    AddProc(s, 3000, 500, L"OBS.exe", 150, 0, 0.0);
    AddProc(s, 3001, 400, L"audiodg.exe", 130, 0, 0.0);
    AddProc(s, 4, 1000, L"System", 250, 0, 0.0);
    AddProc(s, 0, 1000, L"Idle", 250, 0, 0.0);
    return s;
}

cd::Config MakeEngineConfig(const cd::Topology& t, bool autoPin) {
    cd::Config c;
    c.version = 1;
    c.pollMs = 250;
    c.masks = cd::DeriveMasks(t);
    c.exclusions.push_back(L"EasyAntiCheat.exe");
    c.exclusions.push_back(L"audiodg.exe");
    c.exclusions.push_back(L"encoder.exe");

    cd::Profile p;
    p.name = L"Overwatch";
    p.enabled = true;
    p.game = L"Overwatch.exe";
    p.gameMask = L"Cache no SMT";
    p.heavy.push_back(L"OBS.exe");
    p.heavy.push_back(L"audiodg.exe");
    p.heavyMask = L"Freq";
    p.autoPin = autoPin;
    p.autoPinPercent = 8;
    p.autoPinSeconds = 5;
    c.profiles.push_back(p);
    return c;
}

void Test_C1_NoMatch() {
    Case("C1 no profile matches -> empty result, matchedProfile null");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, false);

    cd::ProcessSnapshot s;
    AddProc(s, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s, 3000, 500, L"OBS.exe", 150, 0, 0.0);  // heavy is live, the game is not

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = reinterpret_cast<const cd::Profile*>(0x1);
    std::map<DWORD, std::wstring> res = cd::ComputeDesired(s, c, 500, sticky, &matched);

    CHECK_EQ((int)res.size(), 0);
    CHECK(matched == nullptr);

    // C1b: rule 1 says the FIRST ENABLED profile. A disabled profile must not match even
    // when its game is running.
    Case("C1b a DISABLED profile does not match even when its game is running");
    cd::Config c2 = MakeEngineConfig(t, false);
    c2.profiles[0].enabled = false;
    cd::ProcessSnapshot s2 = MakeGameSnapshot();
    std::vector<std::wstring> sticky2;
    const cd::Profile* matched2 = reinterpret_cast<const cd::Profile*>(0x1);
    std::map<DWORD, std::wstring> res2 = cd::ComputeDesired(s2, c2, 1000, sticky2, &matched2);
    CHECK_EQ((int)res2.size(), 0);
    CHECK(matched2 == nullptr);
}

void Test_C2_C3_C4_C5_C8() {
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, false);
    cd::ProcessSnapshot s = MakeGameSnapshot();

    // Fixture sanity, so the assertions below cannot pass for the wrong reason.
    std::vector<DWORD> desc = s.Descendants(1000);
    CHECK(std::find(desc.begin(), desc.end(), 1003u) != desc.end());  // EAC IS a descendant
    CHECK(std::find(desc.begin(), desc.end(), 1004u) != desc.end());  // OBS child IS one too
    CHECK_EQ((int)s.FindBySpec(L"OBS.exe").size(), 2);

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::map<DWORD, std::wstring> res = cd::ComputeDesired(s, c, 1000, sticky, &matched);

    Case("C2 game pid and all descendants get the game mask");
    CHECK(matched != nullptr);
    if (matched) CHECK_EQ(matched->name, L"Overwatch");
    CHECK_EQ(MaskOf(res, 1000), L"Cache no SMT");
    CHECK_EQ(MaskOf(res, 1001), L"Cache no SMT");
    CHECK_EQ(MaskOf(res, 1002), L"Cache no SMT");
    CHECK(!Has(res, 500));   // explorer is not a descendant
    CHECK(!Has(res, 2000));  // notepad is neither game nor heavy
    CHECK(!Has(res, 400));

    Case("C3 an excluded descendant (EasyAntiCheat.exe) is NOT governed");
    CHECK(!Has(res, 1003));

    Case("C4 heavy apps get the heavy mask; an excluded name on the heavy list still counts");
    CHECK_EQ(MaskOf(res, 3000), L"Freq");
    CHECK_EQ(MaskOf(res, 3001), L"Freq");  // audiodg.exe is excluded AND explicitly heavy

    Case("C5 precedence: a pid in both the game set and the heavy set gets the GAME mask");
    CHECK_EQ(MaskOf(res, 1004), L"Cache no SMT");

    Case("C8 pids 0 and 4 never appear (reached here as game descendants)");
    CHECK(!Has(res, 0));
    CHECK(!Has(res, 4));
}

void Test_C8b_ZeroAndFourViaHeavyList() {
    Case("C8b pids 0 and 4 never appear even when named on the heavy list");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, false);
    c.profiles[0].heavy.push_back(L"System");
    c.profiles[0].heavy.push_back(L"Idle");

    cd::ProcessSnapshot s = MakeGameSnapshot();
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::map<DWORD, std::wstring> res = cd::ComputeDesired(s, c, 1000, sticky, &matched);

    CHECK(matched != nullptr);
    CHECK(!Has(res, 0));
    CHECK(!Has(res, 4));
}

void Test_C6_AutoPinInertWhenForegroundNotInGameSet() {
    Case("C6 autoPin is inert when the foreground pid is not in the game set");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);
    c.pollMs = 250;  // the debounce is a fixed sample COUNT now, not a duration

    cd::ProcessSnapshot s = MakeGameSnapshot();
    AddProc(s, 4000, 500, L"transcoder.exe", 140, 40, 25.0);  // well past the threshold

    // Foreground is explorer, which is NOT in the game set.
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::map<DWORD, std::wstring> res = cd::ComputeDesired(s, c, 500, sticky, &matched);
    CHECK(matched != nullptr);
    CHECK(!Has(res, 4000));
    CHECK_EQ((int)sticky.size(), 0);

    // POSITIVE CONTROL: identical snapshot, foreground moved to the game, and the very same
    // process IS auto-pinned. Without this, C6 would pass on an engine where auto-pin is
    // simply broken.
    std::vector<std::wstring> sticky2;
    const cd::Profile* matched2 = nullptr;
    std::map<DWORD, std::wstring> res2 = cd::ComputeDesired(s, c, 1000, sticky2, &matched2);
    CHECK(matched2 != nullptr);
    CHECK_EQ(MaskOf(res2, 4000), L"Freq");
    CHECK(std::find(sticky2.begin(), sticky2.end(), L"transcoder.exe") != sticky2.end());

    // REWRITTEN for the fixed debounce. This block used to read "3 ticks * 250 ms = 750 ms
    // < the profile's 5 s hold". autoPinSeconds is no longer a user setting (config.h), so
    // the hold is now kAutoPinDebounceTicks CONSECUTIVE SAMPLES and does not scale with
    // pollMs at all. A candidate one sample short of it is still not pinned.
    CHECK_EQ(cd::kAutoPinDebounceTicks, 2);   // fixture assumption, stated out loud
    cd::ProcessSnapshot s3 = MakeGameSnapshot();
    AddProc(s3, 4001, 500, L"briefly_busy.exe", 140, cd::kAutoPinDebounceTicks - 1, 25.0);
    std::vector<std::wstring> sticky3;
    const cd::Profile* matched3 = nullptr;
    std::map<DWORD, std::wstring> res3 = cd::ComputeDesired(s3, c, 1000, sticky3, &matched3);
    CHECK(matched3 != nullptr);
    CHECK(!Has(res3, 4001));

    // BOUNDARY, the other side: exactly kAutoPinDebounceTicks IS enough. Without this the
    // assertion above would also pass on an engine that never auto-pins anything.
    cd::ProcessSnapshot s3b = MakeGameSnapshot();
    AddProc(s3b, 4001, 500, L"briefly_busy.exe", 140, cd::kAutoPinDebounceTicks, 25.0);
    std::vector<std::wstring> sticky3b;
    const cd::Profile* matched3b = nullptr;
    std::map<DWORD, std::wstring> res3b = cd::ComputeDesired(s3b, c, 1000, sticky3b, &matched3b);
    CHECK(matched3b != nullptr);
    CHECK_EQ(MaskOf(res3b, 4001), L"Freq");

    // And the debounce does NOT scale with pollMs any more: the same sample count at a very
    // different poll interval gives the same answer. A seconds-based hold would not.
    cd::Config cSlow = MakeEngineConfig(t, true);
    cSlow.pollMs = 2000;
    std::vector<std::wstring> sticky3c;
    const cd::Profile* matched3c = nullptr;
    std::map<DWORD, std::wstring> res3c = cd::ComputeDesired(s3b, cSlow, 1000, sticky3c, &matched3c);
    CHECK(matched3c != nullptr);
    CHECK_EQ(MaskOf(res3c, 4001), L"Freq");

    // An EXCLUDED process is never auto-pinned (rule 4: "not excluded").
    cd::ProcessSnapshot s4 = MakeGameSnapshot();
    AddProc(s4, 4002, 500, L"encoder.exe", 140, 40, 25.0);  // "encoder.exe" is excluded
    std::vector<std::wstring> sticky4;
    const cd::Profile* matched4 = nullptr;
    std::map<DWORD, std::wstring> res4 = cd::ComputeDesired(s4, c, 1000, sticky4, &matched4);
    CHECK(matched4 != nullptr);
    CHECK(!Has(res4, 4002));

    Case("C6b NVIDIA Broadcast wildcard blocks auto-pin, while an exact manual Heavy entry wins");
    cd::Config broadcastConfig = MakeEngineConfig(t, true);
    broadcastConfig.exclusions = cd::DefaultExclusions();
    cd::ProcessSnapshot broadcastSnapshot = MakeGameSnapshot();
    AddProc(broadcastSnapshot, 4003, 500, L"NVIDIA Broadcast 1.exe", 145, 40, 25.0);

    std::vector<std::wstring> broadcastSticky;
    const cd::Profile* broadcastMatched = nullptr;
    std::map<DWORD, std::wstring> broadcastResult =
        cd::ComputeDesired(broadcastSnapshot, broadcastConfig, 1000, broadcastSticky,
                           &broadcastMatched);
    CHECK(broadcastMatched != nullptr);
    CHECK(!Has(broadcastResult, 4003));
    CHECK(std::find(broadcastSticky.begin(), broadcastSticky.end(),
                    L"NVIDIA Broadcast 1.exe") == broadcastSticky.end());

    // Rule 3 is deliberate: the user's exact Heavy entry outranks an exclusion.
    broadcastConfig.profiles[0].heavy.push_back(L"NVIDIA Broadcast 1.exe");
    broadcastSticky.clear();
    broadcastResult = cd::ComputeDesired(broadcastSnapshot, broadcastConfig, 1000,
                                         broadcastSticky, &broadcastMatched);
    CHECK_EQ(MaskOf(broadcastResult, 4003), L"Freq");
}

void Test_C7_Stickiness() {
    Case("C7 once auto-pinned, an executable stays pinned after its CPU drops, until the "
         "game exits");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);
    c.pollMs = 250;

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;

    // Tick 1: busy, foreground is the game -> auto-pinned.
    cd::ProcessSnapshot s1 = MakeGameSnapshot();
    AddProc(s1, 4000, 500, L"transcoder.exe", 140, 40, 25.0);
    std::map<DWORD, std::wstring> r1 = cd::ComputeDesired(s1, c, 1000, sticky, &matched);
    CHECK(matched != nullptr);
    CHECK_EQ(MaskOf(r1, 4000), L"Freq");
    CHECK(std::find(sticky.begin(), sticky.end(), L"transcoder.exe") != sticky.end());

    // Tick 2: same process, CPU has collapsed. It must STAY pinned.
    cd::ProcessSnapshot s2 = MakeGameSnapshot();
    AddProc(s2, 4000, 500, L"transcoder.exe", 140, 0, 0.0);
    std::map<DWORD, std::wstring> r2 = cd::ComputeDesired(s2, c, 1000, sticky, &matched);
    CHECK(matched != nullptr);
    CHECK_EQ(MaskOf(r2, 4000), L"Freq");

    // Tick 3: still pinned even though the foreground has moved off the game.
    cd::ProcessSnapshot s3 = MakeGameSnapshot();
    AddProc(s3, 4000, 500, L"transcoder.exe", 140, 0, 0.0);
    std::map<DWORD, std::wstring> r3 = cd::ComputeDesired(s3, c, 500, sticky, &matched);
    CHECK(matched != nullptr);
    CHECK_EQ(MaskOf(r3, 4000), L"Freq");

    // Tick 4: the game exits. Nothing is governed any more.
    cd::ProcessSnapshot s4;
    AddProc(s4, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s4, 4000, 500, L"transcoder.exe", 140, 0, 0.0);
    const cd::Profile* matched4 = reinterpret_cast<const cd::Profile*>(0x1);
    std::map<DWORD, std::wstring> r4 = cd::ComputeDesired(s4, c, 500, sticky, &matched4);
    CHECK(matched4 == nullptr);
    CHECK_EQ((int)r4.size(), 0);
}

// pid 9000 GameOptimizer.exe       <- US
// pid 9001 msedgewebview2.exe      <- our sponsor panel, our child
// pid 9002 msedgewebview2.exe      <- a child of THAT
// pid 8000 SearchHost.exe          <- Windows' own, nothing to do with us
// pid 8001 msedgewebview2.exe      <- ITS child: the SAME executable name, NOT ours
// Every one of them is well past the threshold and well past the debounce, so the only
// thing that can separate them is descent.
cd::ProcessSnapshot MakeSelfSubtreeSnapshot() {
    cd::ProcessSnapshot s = MakeGameSnapshot();
    AddProc(s, 9000, 500, L"GameOptimizer.exe", 600, 40, 25.0);
    AddProc(s, 9001, 9000, L"msedgewebview2.exe", 610, 40, 25.0);
    AddProc(s, 9002, 9001, L"msedgewebview2.exe", 620, 40, 25.0);
    AddProc(s, 8000, 500, L"SearchHost.exe", 605, 40, 25.0);
    AddProc(s, 8001, 8000, L"msedgewebview2.exe", 615, 40, 25.0);
    return s;
}

void Test_C9_SelfSubtreeIsNeverAutoPinned() {
    Case("C9 rule 4 never auto-pins our own process or its descendants, and separates them "
         "by PARENTAGE rather than by executable name");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);

    cd::ProcessSnapshot s = MakeSelfSubtreeSnapshot();
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::map<DWORD, std::wstring> res =
        cd::ComputeDesired(s, c, 1000, sticky, &matched, 9000);
    CHECK(matched != nullptr);
    CHECK(!Has(res, 9000));
    CHECK(!Has(res, 9001));
    CHECK(!Has(res, 9002));

    // The self-only executable is not admitted. msedgewebview2.exe is admitted by the valid
    // SearchHost child below, so the output assertions above prove its self-owned members are
    // re-vetoed after name-group expansion rather than admitted by association.
    CHECK(std::find(sticky.begin(), sticky.end(), L"GameOptimizer.exe") == sticky.end());
    CHECK(std::find(sticky.begin(), sticky.end(), L"msedgewebview2.exe") != sticky.end());

    // THE NAME-COLLISION CONTROL, and the whole reason this rule is parentage and not another
    // line on the exclusion list: the same executable under SearchHost.exe is a genuine
    // background load the user may well want moved, and it still is.
    CHECK_EQ(MaskOf(res, 8001), L"Freq");
    CHECK_EQ(MaskOf(res, 8000), L"Freq");

    // POSITIVE CONTROL: the identical snapshot with no self pid pins all three. Without it,
    // C9 would pass just as happily on an engine whose auto-pin never fires at all.
    std::vector<std::wstring> sticky2;
    const cd::Profile* matched2 = nullptr;
    std::map<DWORD, std::wstring> res2 = cd::ComputeDesired(s, c, 1000, sticky2, &matched2);
    CHECK_EQ(MaskOf(res2, 9000), L"Freq");
    CHECK_EQ(MaskOf(res2, 9001), L"Freq");
    CHECK_EQ(MaskOf(res2, 9002), L"Freq");

    // A selfPid naming no live process excludes nothing. It must not read as "exclude
    // everything", and it must not quietly exclude the real subtree either.
    std::vector<std::wstring> sticky3;
    const cd::Profile* matched3 = nullptr;
    std::map<DWORD, std::wstring> res3 =
        cd::ComputeDesired(s, c, 1000, sticky3, &matched3, 7777);
    CHECK_EQ(MaskOf(res3, 9001), L"Freq");
}

void Test_C10_SelfSubtreeVetoesAStickyName() {
    Case("C10 the self-subtree veto applies to every member of a sticky executable group, "
         "including a recycled pid");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);

    // Tick 1: an ordinary busy process is admitted. It is nothing to do with us.
    cd::ProcessSnapshot s1 = MakeGameSnapshot();
    AddProc(s1, 9000, 500, L"GameOptimizer.exe", 600, 0, 0.0);
    AddProc(s1, 4000, 500, L"transcoder.exe", 140, 40, 25.0);
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::map<DWORD, std::wstring> r1 =
        cd::ComputeDesired(s1, c, 1000, sticky, &matched, 9000);
    CHECK_EQ(MaskOf(r1, 4000), L"Freq");
    CHECK(std::find(sticky.begin(), sticky.end(), L"transcoder.exe") != sticky.end());

    // Tick 2: pid 4000 has been recycled into a new instance of the admitted executable,
    // now inside OUR subtree. Name stickiness intentionally finds the new instance; the
    // member-level self veto must still keep it out.
    cd::ProcessSnapshot s2 = MakeGameSnapshot();
    AddProc(s2, 9000, 500, L"GameOptimizer.exe", 600, 0, 0.0);
    AddProc(s2, 4000, 9000, L"transcoder.exe", 700, 0, 0.0);
    std::vector<std::wstring> sticky2 = sticky;
    const cd::Profile* matched2 = nullptr;
    std::map<DWORD, std::wstring> r2 =
        cd::ComputeDesired(s2, c, 1000, sticky2, &matched2, 9000);
    CHECK(!Has(r2, 4000));

    // POSITIVE CONTROL: the same-name recycled pid with no self pid is pinned, so the
    // assertion above is specifically about the subtree veto.
    std::vector<std::wstring> sticky3 = sticky;
    const cd::Profile* matched3 = nullptr;
    std::map<DWORD, std::wstring> r3 = cd::ComputeDesired(s2, c, 1000, sticky3, &matched3);
    CHECK_EQ(MaskOf(r3, 4000), L"Freq");

    // PID-REUSE CONTROL: session state is not attached to the numeric pid. Reusing 4000 for
    // a different executable does not inherit transcoder.exe's admission.
    cd::ProcessSnapshot s3 = MakeGameSnapshot();
    AddProc(s3, 4000, 500, L"unrelated.exe", 800, 0, 0.0);
    std::vector<std::wstring> sticky4 = sticky;
    const cd::Profile* matched4 = nullptr;
    std::map<DWORD, std::wstring> r4 = cd::ComputeDesired(s3, c, 1000, sticky4, &matched4);
    CHECK(!Has(r4, 4000));
}

void Test_C11_AutoPinnedOutIsExactlyRuleFour() {
    Case("C11 autoPinnedOut carries the pids rule 4 decided, and only those");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);

    cd::ProcessSnapshot s = MakeGameSnapshot();
    AddProc(s, 4000, 500, L"transcoder.exe", 140, 40, 25.0);   // rule 4's own
    // OBS.exe is on the heavy LIST and is busy enough to qualify as well. Rule 5 gives it the
    // heavy mask because the USER named it, so it must not be reported as one the app chose.
    access::Procs(s)[3000].aboveThresholdTicks = 40;
    access::Procs(s)[3000].cpuPercent = 25.0;
    // A busy DESCENDANT of the game keeps the game mask and is likewise not rule 4's doing.
    access::Procs(s)[1001].aboveThresholdTicks = 40;
    access::Procs(s)[1001].cpuPercent = 25.0;

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::set<DWORD> autoPinned;
    std::map<DWORD, std::wstring> res =
        cd::ComputeDesired(s, c, 1000, sticky, &matched, 0, &autoPinned);
    CHECK(matched != nullptr);
    CHECK_EQ((int)autoPinned.size(), 1);
    CHECK(autoPinned.find(4000) != autoPinned.end());
    CHECK(autoPinned.find(3000) == autoPinned.end());   // the user named this one
    CHECK(autoPinned.find(1001) == autoPinned.end());   // the game owns this one
    // Both are still governed - they are just not rule 4's to claim.
    CHECK_EQ(MaskOf(res, 3000), L"Freq");
    CHECK_EQ(MaskOf(res, 1001), L"Cache no SMT");

    // CLEARED, never appended to, so a caller that reuses one set across ticks cannot
    // accumulate pids the rule has since dropped.
    autoPinned.insert(123456);
    cd::ProcessSnapshot gone;
    AddProc(gone, 500, 400, L"explorer.exe", 100, 0, 0.0);
    std::vector<std::wstring> sticky2;
    const cd::Profile* matched2 = nullptr;
    cd::ComputeDesired(gone, c, 500, sticky2, &matched2, 0, &autoPinned);
    CHECK_EQ((int)autoPinned.size(), 0);
}

void Test_C12_AutoPinExpandsExeGroupWithPerPidVetoes() {
    Case("C12 one qualifying auto-pin process admits its executable group, including later "
         "processes, while every per-pid veto remains in force");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeEngineConfig(t, true);

    // Tick 1: only pid 4000 has crossed the CPU threshold. Its idle sibling must join the
    // admitted executable group, but members owned by the game or our subtree must not.
    cd::ProcessSnapshot s1 = MakeGameSnapshot();
    AddProc(s1, 9000, 500, L"GameOptimizer.exe", 600, 0, 0.0);
    AddProc(s1, 4000, 500, L"NVIDIA Broadcast 1.exe", 140,
            cd::kAutoPinDebounceTicks, 25.0);
    AddProc(s1, 4001, 500, L"NVIDIA Broadcast 1.exe", 141, 0, 0.0);
    AddProc(s1, 4002, 9000, L"NVIDIA Broadcast 1.exe", 610, 0, 0.0);
    AddProc(s1, 4003, 1000, L"NVIDIA Broadcast 1.exe", 370, 0, 0.0);
    AddProc(s1, 5000, 500, L"encoder.exe", 142, cd::kAutoPinDebounceTicks, 25.0);
    AddProc(s1, 5001, 500, L"encoder.exe", 143, 0, 0.0);
    access::Procs(s1)[4].name = L"NVIDIA Broadcast 1.exe";
    access::Procs(s1)[4].fullPath = L"C:\\Apps\\NVIDIA Broadcast 1.exe";

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::set<DWORD> autoPinned;
    std::map<DWORD, std::wstring> r1 =
        cd::ComputeDesired(s1, c, 1000, sticky, &matched, 9000, &autoPinned);
    CHECK(matched != nullptr);
    CHECK_EQ(MaskOf(r1, 4000), L"Freq");
    CHECK_EQ(MaskOf(r1, 4001), L"Freq");
    CHECK(!Has(r1, 4002));
    CHECK_EQ(MaskOf(r1, 4003), L"Cache no SMT");
    CHECK(!Has(r1, 4));
    CHECK(!Has(r1, 5000));
    CHECK(!Has(r1, 5001));
    CHECK_EQ((int)autoPinned.size(), 2);
    CHECK(autoPinned.find(4000) != autoPinned.end());
    CHECK(autoPinned.find(4001) != autoPinned.end());

    // Tick 2: the qualifying pid and every ordinary sibling have exited. A newly launched
    // process of the admitted executable still joins while focus is outside the game. The
    // same-name members behind each per-pid veto remain governed only by the winning rule.
    cd::ProcessSnapshot s2 = MakeGameSnapshot();
    AddProc(s2, 9000, 500, L"GameOptimizer.exe", 600, 0, 0.0);
    AddProc(s2, 4010, 500, L"NVIDIA Broadcast 1.exe", 700, 0, 0.0);
    AddProc(s2, 4011, 9000, L"NVIDIA Broadcast 1.exe", 710, 0, 0.0);
    AddProc(s2, 4012, 1000, L"NVIDIA Broadcast 1.exe", 380, 0, 0.0);
    access::Procs(s2)[4].name = L"NVIDIA Broadcast 1.exe";
    access::Procs(s2)[4].fullPath = L"C:\\Apps\\NVIDIA Broadcast 1.exe";

    std::set<DWORD> autoPinned2;
    std::map<DWORD, std::wstring> r2 =
        cd::ComputeDesired(s2, c, 500, sticky, &matched, 9000, &autoPinned2);
    CHECK_EQ(MaskOf(r2, 4010), L"Freq");
    CHECK(!Has(r2, 4011));
    CHECK_EQ(MaskOf(r2, 4012), L"Cache no SMT");
    CHECK(!Has(r2, 4));
    CHECK_EQ((int)autoPinned2.size(), 1);
    CHECK(autoPinned2.find(4010) != autoPinned2.end());
}

// ===========================================================================
// D. BuildTooltip.
// ===========================================================================

cd::EngineStatus MakeActiveStatus(int gameProcCount, int heavyCount, int blockedCount,
                                  const std::wstring& profileName) {
    cd::EngineStatus st;
    st.active = true;
    st.paused = false;
    st.profileName = profileName;
    st.gameMaskName = L"Cache no SMT";
    st.heavyMaskName = L"Freq";
    st.gamePid = 1000;
    st.gameProcCount = gameProcCount;
    st.heavyCount = heavyCount;
    st.blockedCount = blockedCount;
    for (int i = 0; i < gameProcCount + heavyCount; ++i) {
        cd::GovernedProcess g;
        g.pid = (DWORD)(1000 + i);
        g.name = L"proc.exe";
        g.maskName = (i < gameProcCount) ? L"Cache no SMT" : L"Freq";
        g.blocked = (i < blockedCount);
        st.governed.push_back(g);
    }
    return st;
}

void Test_D_BuildTooltip() {
    Case("D idle tooltip");
    cd::EngineStatus idle;
    CHECK_EQ(cd::BuildTooltip(idle), L"Game Optimizer - idle");

    Case("D active tooltip");
    cd::EngineStatus act = MakeActiveStatus(1, 4, 0, L"Overwatch");
    CHECK_EQ(cd::BuildTooltip(act), L"Overwatch: Cache no SMT - 4 apps on Freq");

    Case("D degraded tooltip");
    cd::EngineStatus deg = MakeActiveStatus(4, 2, 2, L"Overwatch");
    CHECK_EQ((int)deg.governed.size(), 6);
    CHECK_EQ(cd::BuildTooltip(deg), L"Overwatch: Cache no SMT - 2 of 6 apps blocked");

    Case("D a very long profile name is truncated to 127 chars (NOTIFYICONDATA szTip)");
    cd::EngineStatus big = MakeActiveStatus(1, 4, 0, std::wstring(500, L'A'));
    std::wstring tip = cd::BuildTooltip(big);
    CHECK((int)tip.size() <= 127);
    CHECK(!tip.empty());

    cd::EngineStatus bigDeg = MakeActiveStatus(4, 2, 2, std::wstring(500, L'B'));
    std::wstring tip2 = cd::BuildTooltip(bigDeg);
    CHECK((int)tip2.size() <= 127);
    CHECK(!tip2.empty());
}

// ===========================================================================
// E. The readback: which mask is a process ACTUALLY on.
// ===========================================================================
//
// Written from the contract in applier.h and topology.h. The point of the whole feature is
// that it must be able to say "that is not one of ours" and "they disagree" - so most of
// what is asserted here is the REFUSALS, not the happy path. A classifier that answered
// "Cache" for anything cache-shaped would pass a suite made only of exact matches.

cd::CpuSetReadback Ok(const std::vector<ULONG>& ids) {
    cd::CpuSetReadback r;
    r.ok = true;
    r.ids = ids;
    return r;
}

cd::CpuSetReadback Denied() {
    cd::CpuSetReadback r;
    r.ok = false;                 // could not ask - NOT the same as "no assignment"
    return r;
}

// A RECYCLED pid: live, and it would have answered - but the process now holding that number
// is not the one the caller saw, so there is nothing here we are entitled to report.
cd::CpuSetReadback Recycled() {
    cd::CpuSetReadback r;
    r.ours = false;
    return r;
}

// The same, except the read went ahead and came back with ids. The product never builds this
// - the readback refuses a pid before it asks it - and that is exactly why it is worth
// asserting: the classifier must reject a stranger on `ours` ALONE, not because the stranger
// happened to answer with nothing.
cd::CpuSetReadback RecycledAnswering(const std::vector<ULONG>& ids) {
    cd::CpuSetReadback r;
    r.ok = true;
    r.ours = false;
    r.ids = ids;
    return r;
}

void Test_E1_MaskNameForIds() {
    Case("E1 MaskNameForIds: exact set match, and nothing else");
    cd::Topology t = MakeReference(false);
    const std::vector<cd::Mask> m = cd::DeriveMasks(t);

    CHECK_EQ(cd::MaskNameForIds(m, IdRange(256u, 16, 1u)), L"Cache");
    CHECK_EQ(cd::MaskNameForIds(m, IdRange(272u, 16, 1u)), L"Freq");
    CHECK_EQ(cd::MaskNameForIds(m, IdRange(256u, 8, 2u)), L"Cache no SMT");
    CHECK_EQ(cd::MaskNameForIds(m, IdRange(256u, 32, 1u)), L"All");

    // GetProcessDefaultCpuSets promises no order, so order must not matter. Nor may a
    // repeated id, which a hand-edited mask can also carry.
    {
        std::vector<ULONG> shuffled = IdRange(256u, 16, 1u);
        std::reverse(shuffled.begin(), shuffled.end());
        CHECK_EQ(cd::MaskNameForIds(m, shuffled), L"Cache");
        std::vector<ULONG> dupes = IdRange(256u, 16, 1u);
        dupes.push_back(256u);
        dupes.push_back(271u);
        CHECK_EQ(cd::MaskNameForIds(m, dupes), L"Cache");
    }

    // THE REFUSALS. A partial set is not "mostly Cache", and a superset is not Cache either.
    {
        std::vector<ULONG> missingOne = IdRange(256u, 16, 1u);
        missingOne.pop_back();                       // 15 of the 16
        CHECK_EQ(cd::MaskNameForIds(m, missingOne), L"");

        std::vector<ULONG> plusOne = IdRange(256u, 16, 1u);
        plusOne.push_back(272u);                     // one id from the other CCD
        CHECK_EQ(cd::MaskNameForIds(m, plusOne), L"");

        std::vector<ULONG> alien;
        alien.push_back(9000u);
        CHECK_EQ(cd::MaskNameForIds(m, alien), L"");
    }

    // No assignment is a STATE, not a mask, and this function does not name it.
    CHECK_EQ(cd::MaskNameForIds(m, std::vector<ULONG>()), L"");

    // Nothing can be named against an empty mask list, however ordinary the ids look.
    CHECK_EQ(cd::MaskNameForIds(std::vector<cd::Mask>(), IdRange(256u, 16, 1u)), L"");
}

void Test_E2_ClassifyCpuSetStage() {
    Case("E2 ClassifyCpuSetStage: every state a user can be shown");
    cd::Topology t = MakeReference(false);
    const std::vector<cd::Mask> m = cd::DeriveMasks(t);
    const std::vector<ULONG> cache = IdRange(256u, 16, 1u);
    const std::vector<ULONG> freq = IdRange(272u, 16, 1u);

    // Not running: nothing live matched the name at all.
    {
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, std::vector<cd::CpuSetReadback>());
        CHECK(i.stage == cd::CpuSetStage::NotRunning);
        CHECK_EQ(i.probed, 0);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"-");
    }

    // Running with NO assignment. This is the state the whole product is the exception to,
    // and it must never be reported as a mask name.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(std::vector<ULONG>()));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::AllCores);
        CHECK_EQ(i.name, L"");
        CHECK_EQ(cd::CpuSetStageLabel(i), L"All cores");
    }

    // Assigned, and it is one of ours.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Cache");
        CHECK_EQ(cd::CpuSetStageLabel(i), L"Cache");
        CHECK_EQ(i.probed, 1);
        CHECK_EQ(i.failed, 0);
    }

    // Assigned to something we cannot name - someone else's writer, or a partial set.
    {
        std::vector<ULONG> odd = cache;
        odd.pop_back();
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(odd));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Custom);
        CHECK_EQ(i.name, L"");
        CHECK_EQ(cd::CpuSetStageLabel(i), L"Custom");
    }

    // Several instances that do not agree. Reporting either one would be a coin toss
    // presented as a fact.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(Ok(freq));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Mixed);
        CHECK_EQ(i.name, L"");
        CHECK_EQ(cd::CpuSetStageLabel(i), L"Mixed");
        CHECK_EQ(i.probed, 2);
    }

    // "Assigned" and "unassigned" disagree too - Mixed, not "All cores".
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(Ok(std::vector<ULONG>()));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Mixed);
    }

    // Live but unreadable: every instance refused. Not "All cores" - we never found out.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Denied());
        r.push_back(Denied());
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::NoAccess);
        CHECK_EQ(i.probed, 2);
        CHECK_EQ(i.failed, 2);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"No access");
    }

    // A partial refusal does not become Mixed: the readable ones agree, and `failed` carries
    // the fact that the picture is incomplete. It must be counted in FULL even though the
    // failure sits between two agreeing reads.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(freq));
        r.push_back(Denied());
        r.push_back(Ok(freq));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Freq");
        CHECK_EQ(i.probed, 3);
        CHECK_EQ(i.failed, 1);
    }

    // `failed` stays complete when the verdict IS Mixed - the disagreement is decided on the
    // first two reads and the counting must not stop there.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(Ok(freq));
        r.push_back(Denied());
        r.push_back(Denied());
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Mixed);
        CHECK_EQ(i.probed, 4);
        CHECK_EQ(i.failed, 2);
    }

    // With no named masks at all, an assignment is Custom - never invented, never blank.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(std::vector<cd::Mask>(), r);
        CHECK(i.stage == cd::CpuSetStage::Custom);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"Custom");
    }
}

void Test_E4_PidReuseGuard() {
    Case("E4 a recycled pid is not ours and never reaches the verdict");
    cd::Topology t = MakeReference(false);
    const std::vector<cd::Mask> m = cd::DeriveMasks(t);
    const std::vector<ULONG> cache = IdRange(256u, 16, 1u);
    const std::vector<ULONG> freq = IdRange(272u, 16, 1u);

    // ---- the rule itself ---------------------------------------------------------------
    // The same non-zero creation time is the ONLY shape that may come back true.
    CHECK(cd::SameProcessInstance(132000000000000000ull, 132000000000000000ull));
    CHECK(cd::SameProcessInstance(1ull, 1ull));
    CHECK(cd::SameProcessInstance(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull));

    // Different times: the pid was recycled. One 100ns tick apart is still apart - two
    // processes cannot hold one pid at the same moment, so any difference at all is proof.
    CHECK(!cd::SameProcessInstance(132000000000000000ull, 132000000000000001ull));
    CHECK(!cd::SameProcessInstance(132000000000000001ull, 132000000000000000ull));

    // A zero on EITHER side is not a match: it means nobody could read a creation time, and
    // an absence of evidence is not evidence of sameness - not even when both are absent.
    CHECK(!cd::SameProcessInstance(0ull, 132000000000000000ull));
    CHECK(!cd::SameProcessInstance(132000000000000000ull, 0ull));
    CHECK(!cd::SameProcessInstance(0ull, 0ull));

    // ---- what the classifier does with one --------------------------------------------
    // Excluded from the verdict, and counted APART from `failed`, because nothing failed:
    // we declined to ask.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(Recycled());
        r.push_back(Ok(cache));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Cache");
        CHECK_EQ(i.probed, 3);
        CHECK_EQ(i.failed, 0);
        CHECK_EQ(i.notOurs, 1);
    }

    // THE ONE THAT MATTERS. A stranger carrying a DIFFERENT mask must not vote, and above all
    // must not be able to manufacture "Mixed": instances disagreeing is a real finding, and a
    // process we never touched disagreeing with us is not one.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(RecycledAnswering(freq));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Cache");
        CHECK_EQ(cd::CpuSetStageLabel(i), L"Cache");
        CHECK_EQ(i.failed, 0);
        CHECK_EQ(i.notOurs, 1);
    }

    // Nor may it manufacture "All cores" by answering with no assignment at all.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(RecycledAnswering(std::vector<ULONG>()));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Cache");
    }

    // EVERY instance recycled. A pid being handed to someone else is proof that the process
    // we saw has EXITED, so the honest word is the one that means "no live instance" - and it
    // must not be "No access", which would claim we could not ask when we could ask perfectly
    // well and got an answer that simply was not ours.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Recycled());
        r.push_back(RecycledAnswering(freq));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::NotRunning);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"-");
        CHECK_EQ(i.name, L"");
        CHECK_EQ(i.probed, 2);
        CHECK_EQ(i.failed, 0);
        CHECK_EQ(i.notOurs, 2);
    }

    // Recycled AND refused, with nothing readable left. One of OURS refused, so "we could not
    // ask" is true of a live process of ours and No access is the right word; the recycled one
    // cannot take that away by being silent about a stranger.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Recycled());
        r.push_back(Denied());
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::NoAccess);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"No access");
        CHECK_EQ(i.failed, 1);
        CHECK_EQ(i.notOurs, 1);
    }

    // And one readable instance still decides for the family, with both counters complete.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Recycled());
        r.push_back(Denied());
        r.push_back(Ok(freq));
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.name, L"Freq");
        CHECK_EQ(i.probed, 3);
        CHECK_EQ(i.failed, 1);
        CHECK_EQ(i.notOurs, 1);
    }

    // The guard changes nothing for a readback nobody marked: it is ours by default, so the
    // states that existed before it still come out exactly as they did.
    {
        std::vector<cd::CpuSetReadback> r;
        r.push_back(Ok(cache));
        r.push_back(Denied());
        cd::CpuSetStageInfo i = cd::ClassifyCpuSetStage(m, r);
        CHECK(i.stage == cd::CpuSetStage::Named);
        CHECK_EQ(i.notOurs, 0);
    }
}

// The LIVE half of the guard, and the only case in this file that opens a process.
//
// Everything else here drives pure functions, deliberately. But a guard that is only tested
// through the classifier proves the classifier and nothing about the code that decides what
// to hand it - so this drives ReadCpuSetStage itself, against THIS process: the one process a
// test can know the true creation time of without racing anything, and one that is always
// openable for PROCESS_QUERY_LIMITED_INFORMATION.
//
// Nothing below asserts WHICH mask we are on. A test may not assume the machine running it is
// unmanaged - see applier.h on the 49 processes already carrying assignments nobody here
// made. What is asserted is WHO the answer is about, which is the whole question.
void Test_E5_LiveReadbackChecksIdentity() {
    Case("E5 the live readback refuses a pid whose creation time has moved");
    const std::vector<cd::Mask> m = cd::DeriveMasks(MakeReference(false));

    const DWORD self = GetCurrentProcessId();
    ULONGLONG born = 0;
    CHECK(cd::GetProcessCreationTime(self, born));
    CHECK(born != 0);

    // Ourselves, correctly identified: a real answer about a process we vouched for.
    {
        std::vector<cd::ObservedProc> v(1);
        v[0].pid = self;
        v[0].creationTime = born;
        cd::CpuSetStageInfo i = cd::ReadCpuSetStage(v, m, 16);
        CHECK_EQ(i.probed, 1);
        CHECK_EQ(i.failed, 0);
        CHECK_EQ(i.notOurs, 0);
        CHECK(i.stage != cd::CpuSetStage::NotRunning);   // we are plainly running
        CHECK(i.stage != cd::CpuSetStage::NoAccess);     // and a process can always open itself
    }

    // The same live process under a creation time that is not its own - which is exactly what
    // a recycled pid looks like from the caller's side. It must not be reported on at all, and
    // with nothing else in the list the honest answer is "-" and not "No access".
    {
        std::vector<cd::ObservedProc> v(1);
        v[0].pid = self;
        v[0].creationTime = born + 1;
        cd::CpuSetStageInfo i = cd::ReadCpuSetStage(v, m, 16);
        CHECK(i.stage == cd::CpuSetStage::NotRunning);
        CHECK_EQ(cd::CpuSetStageLabel(i), L"-");
        CHECK_EQ(i.probed, 1);
        CHECK_EQ(i.notOurs, 1);
        CHECK_EQ(i.failed, 0);
    }

    // No creation time at all. We can open it and we can read it, and we still may not
    // attribute the answer - so it is a process we got nothing usable out of, which is what
    // `failed` means. NOT "not running": it is very obviously running.
    {
        std::vector<cd::ObservedProc> v(1);
        v[0].pid = self;
        v[0].creationTime = 0;
        cd::CpuSetStageInfo i = cd::ReadCpuSetStage(v, m, 16);
        CHECK(i.stage == cd::CpuSetStage::NoAccess);
        CHECK_EQ(i.failed, 1);
        CHECK_EQ(i.notOurs, 0);
    }

    // pid 0 is not a process. It is never opened, and it is never counted as a stranger.
    {
        std::vector<cd::ObservedProc> v(1);
        cd::CpuSetStageInfo i = cd::ReadCpuSetStage(v, m, 16);
        CHECK(i.stage == cd::CpuSetStage::NoAccess);
        CHECK_EQ(i.failed, 1);
        CHECK_EQ(i.notOurs, 0);
    }

    // A stranger between two of ours does not make the family Mixed on the live path either.
    {
        std::vector<cd::ObservedProc> v(3);
        v[0].pid = self;
        v[0].creationTime = born;
        v[1].pid = self;
        v[1].creationTime = born + 1;   // the "recycled" one
        v[2].pid = self;
        v[2].creationTime = born;
        cd::CpuSetStageInfo i = cd::ReadCpuSetStage(v, m, 16);
        CHECK(i.stage != cd::CpuSetStage::Mixed);
        CHECK_EQ(i.probed, 3);
        CHECK_EQ(i.notOurs, 1);
        CHECK_EQ(i.failed, 0);

        // maxProbe still bounds the work, and it counts PIDS, not answers.
        cd::CpuSetStageInfo capped = cd::ReadCpuSetStage(v, m, 1);
        CHECK_EQ(capped.probed, 1);
        CHECK_EQ(capped.notOurs, 0);    // the stranger is second and was never reached
        cd::CpuSetStageInfo uncapped = cd::ReadCpuSetStage(v, m, 0);
        CHECK_EQ(uncapped.probed, 3);   // 0 is "no cap", unchanged by this guard
    }
}

void Test_E3_LabelsAreDistinct() {
    Case("E3 no two states print the same word");
    // A label collision would make two different facts indistinguishable on screen, which is
    // the one failure this feature cannot afford: the user is reading it precisely to tell
    // "we could not ask" from "nothing is assigned".
    cd::CpuSetStage all[] = { cd::CpuSetStage::NotRunning, cd::CpuSetStage::NoAccess,
                              cd::CpuSetStage::AllCores,   cd::CpuSetStage::Custom,
                              cd::CpuSetStage::Mixed };
    std::vector<std::wstring> seen;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        cd::CpuSetStageInfo info;
        info.stage = all[i];
        const std::wstring w = cd::CpuSetStageLabel(info);
        CHECK(!w.empty());
        for (size_t j = 0; j < seen.size(); ++j) CHECK_NE(w, seen[j]);
        seen.push_back(w);
    }

    // And a Named stage prints the MASK's name, not a word of its own - so it can never be
    // confused with any of the above unless a mask is literally called "Mixed".
    cd::CpuSetStageInfo named;
    named.stage = cd::CpuSetStage::Named;
    named.name = L"Freq 2";
    CHECK_EQ(cd::CpuSetStageLabel(named), L"Freq 2");
}

// ===========================================================================
// F. The auto-pin readout, and whose window the foreground answer belongs to.
// ===========================================================================

cd::GovernedProcess Gov(DWORD pid, const wchar_t* name, bool autoPinned) {
    cd::GovernedProcess g;
    g.pid = pid;
    g.name = name;
    g.maskName = L"Freq";
    g.autoPinned = autoPinned;
    return g;
}

void Test_F1_AutoPinnedExeNames() {
    Case("F1 AutoPinnedExeNames collapses pids to executables and drops what is already "
         "listed");
    cd::EngineStatus st;
    st.governed.push_back(Gov(101, L"chrome.exe", true));
    st.governed.push_back(Gov(102, L"chrome.exe", true));       // another pid, same exe
    st.governed.push_back(Gov(103, L"CHROME.EXE", true));       // and another casing
    st.governed.push_back(Gov(104, L"NVIDIA Broadcast.exe", true));
    st.governed.push_back(Gov(105, L"Overwatch.exe", false));   // governed, not by rule 4
    st.governed.push_back(Gov(106, L"", true));                 // name could not be read

    std::vector<std::wstring> none;
    std::vector<std::wstring> v = cd::AutoPinnedExeNames(st, none);
    CHECK_EQ((int)v.size(), 2);
    // Sorted on the lowercased name, so the rows do not re-order themselves once a second.
    CHECK_EQ(v[0], L"chrome.exe");
    CHECK_EQ(v[1], L"NVIDIA Broadcast.exe");

    // A process the user listed themselves is dropped, case-insensitively...
    std::vector<std::wstring> listed;
    listed.push_back(L"CHROME.EXE");
    std::vector<std::wstring> v2 = cd::AutoPinnedExeNames(st, listed);
    CHECK_EQ((int)v2.size(), 1);
    CHECK_EQ(v2[0], L"NVIDIA Broadcast.exe");

    // ...and whether their entry was written as a name or as a full path.
    std::vector<std::wstring> byPath;
    byPath.push_back(L"C:\\Program Files\\NVIDIA Broadcast.exe");
    std::vector<std::wstring> v3 = cd::AutoPinnedExeNames(st, byPath);
    CHECK_EQ((int)v3.size(), 1);
    CHECK_EQ(v3[0], L"chrome.exe");

    // Nothing auto-pinned is an empty list, never one blank row.
    cd::EngineStatus quiet;
    quiet.governed.push_back(Gov(201, L"Overwatch.exe", false));
    CHECK_EQ((int)cd::AutoPinnedExeNames(quiet, none).size(), 0);
}

void Test_F2_ResolveForegroundPid() {
    Case("F2 our own window never becomes the foreground answer");
    const DWORD kUs = 4242;

    // The hook has a value. It was recorded under WINEVENT_SKIPOWNPROCESS, so it is already
    // the last foreground that was NOT ours and is used verbatim - including at the instant
    // our own Settings window is the one in front.
    CHECK_EQ(cd::ResolveForegroundPid(1000, kUs, kUs), 1000u);
    CHECK_EQ(cd::ResolveForegroundPid(1000, 500, kUs), 1000u);

    // No cached value and our own window is in front: NOT KNOWN. Answering with our own pid
    // is what let opening the window to watch the rule be the thing that stopped it.
    CHECK_EQ(cd::ResolveForegroundPid(0, kUs, kUs), 0u);

    // No cached value and somebody else's window is in front: that is a real answer.
    CHECK_EQ(cd::ResolveForegroundPid(0, 500, kUs), 500u);

    // No self pid known at all still answers, and pid 0 stays "nothing".
    CHECK_EQ(cd::ResolveForegroundPid(0, 500, 0), 500u);
    CHECK_EQ(cd::ResolveForegroundPid(0, 0, kUs), 0u);
}

void Test_G1_FullyParkedWarningNamesRunningVCacheCause() {
    Case("G1 fully parked warning names the running AMD V-Cache cause");
    const std::wstring expected =
        L"Warning: all 16 processors in \"Freq\" are currently parked. The running AMD 3D "
        L"V-Cache optimizer (amd3dvcacheUser.exe) is the likely cause. It normally parks the "
        L"non-cache CCD while a game is running. While that CCD is parked, a background mask "
        L"pointing at it is effectively inert. Stopping the AMD service also stops this agent, "
        L"and takes effect immediately. Windows can accept assignments to that CCD and then ignore them.";
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, true, true, false, true), expected);
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, true, true, false, false), expected);
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, true, true, true, true),
        L"Warning: all 8 processors in \"Cache\" are currently parked. The running AMD 3D "
        L"V-Cache optimizer (amd3dvcacheUser.exe) is the likely cause. It normally parks the "
        L"non-cache CCD while a game is running. While that CCD is parked, a background mask "
        L"pointing at it is effectively inert. Stopping the AMD service also stops this agent, "
        L"and takes effect immediately. Windows can accept assignments to that CCD and then ignore them.");
}

void Test_G2_FullyParkedWarningExoneratesOptimizerWhenAgentIsNotRunning() {
    Case("G2 fully parked warning exonerates optimizer when agent is not running");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, false, false, true, true);
    CHECK_EQ(
        warning,
        L"Warning: all 16 processors in \"Freq\" are currently parked. AMD's 3D V-Cache "
        L"optimizer service or driver is running, but the part that actively steers "
        L"(amd3dvcacheUser.exe) is not, so this is probably not coming from the optimizer. "
        L"A BIOS option can park a CCD below the operating system. Look for a game-aware or "
        L"adaptive CCD parking setting - not the CCD or SMT controls that disable a CCD at "
        L"boot, which are a different feature. Windows can accept assignments to a parked "
        L"CCD and then ignore them.");
    CHECK(warning.find(L"BIOS") != std::wstring::npos);
    CHECK(warning.find(L"amd3dvcacheUser.exe") != std::wstring::npos);

    const std::wstring otherWarning =
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, false, true, false, true);
    CHECK_EQ(
        otherWarning,
        L"Warning: all 8 processors in \"Cache\" are currently parked. AMD's 3D V-Cache "
        L"optimizer service or driver is running, but the part that actively steers "
        L"(amd3dvcacheUser.exe) is not, so this is probably not coming from the optimizer. "
        L"A BIOS option can park a CCD below the operating system. Look for a game-aware or "
        L"adaptive CCD parking setting - not the CCD or SMT controls that disable a CCD at "
        L"boot, which are a different feature. Windows can accept assignments to a parked "
        L"CCD and then ignore them.");
    CHECK(otherWarning.find(L"BIOS") != std::wstring::npos);
}

void Test_G3_FullyParkedWarningPointsToFirmwareWhenVCacheIsPresent() {
    Case("G3 fully parked warning points to firmware when AMD V-Cache is present but stopped");
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, false, false, false, true),
        L"Warning: all 16 processors in \"Freq\" are currently parked. AMD's 3D V-Cache "
        L"optimizer is installed, and neither its service nor its driver is running, so "
        L"nothing on the Windows side explains this. A BIOS option can park a CCD below the "
        L"operating system. Look for a game-aware or adaptive CCD parking setting - not the "
        L"CCD or SMT controls that disable a CCD at boot, which are a different feature. "
        L"Windows can accept assignments to a parked CCD and then ignore them.");
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, false, false, false, true),
        L"Warning: all 8 processors in \"Cache\" are currently parked. AMD's 3D V-Cache "
        L"optimizer is installed, and neither its service nor its driver is running, so "
        L"nothing on the Windows side explains this. A BIOS option can park a CCD below the "
        L"operating system. Look for a game-aware or adaptive CCD parking setting - not the "
        L"CCD or SMT controls that disable a CCD at boot, which are a different feature. "
        L"Windows can accept assignments to a parked CCD and then ignore them.");
}

void Test_G4_FullyParkedWarningStaysGenericWithoutVCache() {
    Case("G4 fully parked warning stays generic without AMD V-Cache");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, false, false, false, false);
    CHECK_EQ(
        warning,
        L"Warning: all 16 processors in \"Freq\" are currently parked. Windows can accept "
        L"an assignment to a fully parked mask and then ignore it - the process keeps "
        L"running elsewhere.");
    CHECK(warning.find(L"BIOS") == std::wstring::npos);
    CHECK(warning.find(L"V-Cache") == std::wstring::npos);

    const std::wstring otherWarning =
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, false, false, false, false);
    CHECK_EQ(
        otherWarning,
        L"Warning: all 8 processors in \"Cache\" are currently parked. Windows can accept "
        L"an assignment to a fully parked mask and then ignore it - the process keeps "
        L"running elsewhere.");
    CHECK(otherWarning.find(L"BIOS") == std::wstring::npos);
    CHECK(otherWarning.find(L"V-Cache") == std::wstring::npos);
}

void Test_T1_AgentRunningNamesTheActiveOptimizer() {
    Case("T1 agent running names the active optimizer, not the service");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, true, false, false, true);
    CHECK(warning.find(L"amd3dvcacheUser.exe") != std::wstring::npos);
    CHECK(warning.find(L"The running AMD 3D V-Cache optimizer (amd3dvcacheUser.exe) is the likely cause.") !=
          std::wstring::npos);
    CHECK(warning.find(L"Stopping the AMD service also stops this agent") != std::wstring::npos);
}

void Test_T2_ServiceRunningWithoutAgentDoesNotBlameService() {
    Case("T2 service running with agent not running does not blame service, mentions BIOS");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, false, true, false, true);
    CHECK(warning.find(L"BIOS") != std::wstring::npos);
    CHECK(warning.find(L"this is probably not coming from the optimizer") != std::wstring::npos);
    CHECK(warning.find(L"amd3dvcacheUser.exe") != std::wstring::npos);
    CHECK(warning.find(L"The running AMD 3D V-Cache") == std::wstring::npos);
}

void Test_T3_PresentButNotRunningPointsToFirmware() {
    Case("T3 present but not running points to firmware unchanged");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, false, false, false, true);
    CHECK_EQ(
        warning,
        L"Warning: all 8 processors in \"Cache\" are currently parked. AMD's 3D V-Cache "
        L"optimizer is installed, and neither its service nor its driver is running, so "
        L"nothing on the Windows side explains this. A BIOS option can park a CCD below the "
        L"operating system. Look for a game-aware or adaptive CCD parking setting - not the "
        L"CCD or SMT controls that disable a CCD at boot, which are a different feature. "
        L"Windows can accept assignments to a parked CCD and then ignore them.");
}

void Test_T4_NothingPresentStaysGeneric() {
    Case("T4 nothing present stays generic unchanged");
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Cache", 8, false, false, false, false);
    CHECK_EQ(
        warning,
        L"Warning: all 8 processors in \"Cache\" are currently parked. Windows can accept "
        L"an assignment to a fully parked mask and then ignore it - the process keeps "
        L"running elsewhere.");
}

// ===========================================================================
// U. The Stop toggle, and the anti-stranding restore control.
// ===========================================================================

void Test_U1_StopBoxIsCheckedWhenTheOptimizerIsNotActive() {
    Case("U1 the Stop box is checked exactly when the optimizer is NOT active");
    // "Checked" means stopped. Getting this backwards would show every user a ticked box on a
    // machine where AMD's optimizer is running normally.
    CHECK(cd::VCacheStopBoxChecked(false));
    CHECK(!cd::VCacheStopBoxChecked(true));
}

void Test_U2_StopBoxDependsOnNothingButTheLiveFlag() {
    Case("U2 the Stop box depends on the live flag alone, never on config");
    // This control has NO persisted value. It is a live mirror of the machine, so its helper
    // takes exactly one argument and there is nowhere for a config value to enter. A stored
    // value would survive the user stopping or starting the service outside our UI, and the
    // box would then assert a state the machine is not in.
    CHECK(cd::VCacheStopBoxChecked(false) == true);
    CHECK(cd::VCacheStopBoxChecked(true) == false);
}

void Test_U3_RestoreControlAppearsOnlyForUsersTheOldFeatureStranded() {
    Case("U3 the restore control appears only when a driver Start value was recorded");
    // -1 means "we never disabled the driver", so there is nothing to restore and the control
    // must not exist. Any recorded value means the user has the driver at Start=4 with no other
    // route back, and removing their only way out is the stranding defect this project has
    // already had to fix once.
    CHECK(!cd::ShowVCacheRestoreControl(-1));
    CHECK(cd::ShowVCacheRestoreControl(0));
    CHECK(cd::ShowVCacheRestoreControl(3));
    CHECK(cd::ShowVCacheRestoreControl(4));
}

void Test_U4_ParkedWarningNoLongerClaimsStoppingTheServiceIsUseless() {
    Case("U4 the parked-mask warning no longer says stopping the service will not help");
    // The old sentence was measured FALSE and shipped: stopping amd3dvcacheSvc DOES stop the
    // agent. This test exists so it cannot come back.
    const std::wstring warning =
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, true, true, true, true);
    CHECK(warning.find(L"will not stop this") == std::wstring::npos);
    CHECK(warning.find(L"also stops this agent") != std::wstring::npos);
}

void Test_U5_StoppingDisablesAndClearingRestoresAmdsOwnDefault() {
    Case("U5 stopping sets Disabled(4); clearing restores AMD's shipped Automatic(2)");
    // 4 is SERVICE_DISABLED, 2 is SERVICE_AUTO_START. main.cpp static_asserts both against the
    // Windows headers, so this test and the SDK cannot drift apart silently.
    CHECK_EQ(cd::VCacheServiceStartTypeFor(true), 4);
    // 🔴 2, not 3. AMD's own INF installs this service as StartType = 2 (SERVICE_AUTO_START), so
    // "put it back" means Automatic. Restoring it to Manual would look like a restore and would in
    // fact leave the service permanently dead - it would never start at sign-in again, which is
    // exactly the trap the operator caught in the shipped helper text.
    CHECK_EQ(cd::VCacheServiceStartTypeFor(false), 2);
}

// ===========================================================================
// H. Live environment wording.
// ===========================================================================

void Test_H1_GameModeEnvironmentWordingCoversEveryState() {
    Case("H1 Game Mode environment wording covers on, off, and indeterminate");
    CHECK_EQ(cd::FormatGameModeEnvironmentStatus(cd::GameModeState::On),
             L"Windows Game Mode: On");
    CHECK_EQ(cd::FormatGameModeEnvironmentStatus(cd::GameModeState::Off),
             L"Windows Game Mode: Off");
    CHECK_EQ(cd::FormatGameModeEnvironmentStatus(cd::GameModeState::NotDeterminable),
             L"Windows Game Mode: Not determinable");
}

void Test_H2_AmdVCacheEnvironmentWordingCoversEveryState() {
    Case("H2 AMD V-Cache environment wording covers every service state");
    CHECK_EQ(cd::FormatAmdVCacheEnvironmentStatus(cd::AmdVCacheServiceState::NotInstalled),
             L"AMD 3D V-Cache Performance Optimizer: Not installed");
    CHECK_EQ(cd::FormatAmdVCacheEnvironmentStatus(
                 cd::AmdVCacheServiceState::InstalledButStopped),
             L"AMD 3D V-Cache Performance Optimizer: Installed but stopped");
    CHECK_EQ(cd::FormatAmdVCacheEnvironmentStatus(cd::AmdVCacheServiceState::Running),
             L"AMD 3D V-Cache Performance Optimizer: Running");
    CHECK_EQ(cd::FormatAmdVCacheEnvironmentStatus(
                 cd::AmdVCacheServiceState::NotDeterminable),
             L"AMD 3D V-Cache Performance Optimizer: Not determinable");
}

void Test_H3_RunningVCacheEffectMatchesTheParkedMaskWarning() {
    Case("H3 running AMD V-Cache effect matches the parked-mask warning");
    const std::wstring effect = cd::AmdVCacheRunningEffectText();
    CHECK_EQ(effect,
             L"It normally parks the non-cache CCD while a game is running. While that CCD "
             L"is parked, a background mask pointing at it is effectively inert.");
    CHECK(cd::FormatFullyParkedMaskWarning(L"Freq", 16, true, true, false, true).find(effect) !=
          std::wstring::npos);
}

// ===========================================================================
// I. Autostart command construction and old-entry detection.
// ===========================================================================

void Test_I1_AutostartCommandEndsWithTrayFlag() {
    Case("I1 autostart command ends with the exact --tray flag");
    CHECK(EndsWith(cd::AutostartCommand(L"C:\\Game Optimizer\\GameOptimizer.exe"),
                   L" --tray"));
}

void Test_I2_AutostartCommandQuotesExePath() {
    Case("I2 autostart command quotes the executable path");
    const std::wstring exePath = L"C:\\Program Files\\Game Optimizer\\GameOptimizer.exe";
    const std::wstring command = cd::AutostartCommand(exePath);
    CHECK_EQ(command.substr(0, exePath.size() + 2), L"\"" + exePath + L"\"");
}

void Test_I3_EmptyAutostartDoesNotNeedMigration() {
    Case("I3 an absent autostart value does not need migration");
    CHECK(!cd::AutostartNeedsMigration(L""));
}

void Test_I4_BareAutostartNeedsMigration() {
    Case("I4 a quoted bare executable path needs migration");
    CHECK(cd::AutostartNeedsMigration(L"\"C:\\Game Optimizer\\GameOptimizer.exe\""));
}

void Test_I5_TrayAutostartDoesNotNeedMigration() {
    Case("I5 an autostart command with --tray does not need migration");
    CHECK(!cd::AutostartNeedsMigration(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\" --tray"));
}

void Test_I6_TrayDetectionIsCaseInsensitive() {
    Case("I6 an uppercase --TRAY flag does not need migration");
    CHECK(!cd::AutostartNeedsMigration(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\" --TRAY"));
}

// ===========================================================================
// J. Heavy-app activity ordering.
// ===========================================================================

void Test_J1_MixedHeavyAppsPutRunningEntriesFirst() {
    Case("J1 mixed heavy apps put running entries first");
    const std::vector<std::wstring> items = {
        L"claude.exe", L"node.exe", L"obs64.exe", L"firefox.exe", L"GameBar.exe"};
    const std::vector<std::wstring> keys = {
        L"claude.exe", L"node.exe", L"obs64.exe", L"firefox.exe", L"gamebar.exe"};
    const std::set<std::wstring> running = {L"claude.exe", L"firefox.exe", L"gamebar.exe"};
    const std::vector<std::wstring> expected = {
        L"claude.exe", L"firefox.exe", L"GameBar.exe", L"node.exe", L"obs64.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, keys, running), expected);
}

void Test_J2_RunningHeavyAppsKeepRelativeOrder() {
    Case("J2 running heavy apps keep their relative order");
    const std::vector<std::wstring> items = {L"second.exe", L"dead.exe", L"first.exe"};
    const std::set<std::wstring> running = {L"first.exe", L"second.exe"};
    const std::vector<std::wstring> expected = {L"second.exe", L"first.exe", L"dead.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, items, running), expected);
}

void Test_J3_InactiveHeavyAppsKeepRelativeOrder() {
    Case("J3 inactive heavy apps keep their relative order");
    const std::vector<std::wstring> items = {L"dead-b.exe", L"live.exe", L"dead-a.exe"};
    const std::set<std::wstring> running = {L"live.exe"};
    const std::vector<std::wstring> expected = {L"live.exe", L"dead-b.exe", L"dead-a.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, items, running), expected);
}

void Test_J4_EmptyRunningSetKeepsHeavyAppOrder() {
    Case("J4 an empty running set keeps heavy app order unchanged");
    const std::vector<std::wstring> items = {L"one.exe", L"two.exe", L"three.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, items, std::set<std::wstring>()), items);
}

void Test_J5_EmptyHeavyAppListStaysEmpty() {
    Case("J5 an empty heavy app list stays empty");
    CHECK(cd::OrderHeavyByActivity(std::vector<std::wstring>(),
                                   std::vector<std::wstring>(), {L"live.exe"}).empty());
}

void Test_J6_FullPathMatchesOnBasename() {
    Case("J6 a full path heavy app matches on its basename");
    const std::vector<std::wstring> items = {
        L"dead.exe", L"C:\\Program Files\\obs\\obs64.exe"};
    const std::vector<std::wstring> keys = {L"dead.exe", L"obs64.exe"};
    const std::vector<std::wstring> expected = {
        L"C:\\Program Files\\obs\\obs64.exe", L"dead.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, keys, {L"obs64.exe"}), expected);
}

void Test_J7_HeavyAppMatchingIsCaseInsensitive() {
    Case("J7 caller-folded heavy app keys match case-insensitively");
    const std::vector<std::wstring> items = {L"dead.exe", L"OBS64.EXE"};
    const std::vector<std::wstring> keys = {L"dead.exe", L"obs64.exe"};
    const std::vector<std::wstring> expected = {L"OBS64.EXE", L"dead.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, keys, {L"obs64.exe"}), expected);
}

void Test_J8_MismatchedItemAndKeyCountsKeepInputOrder() {
    Case("J8 mismatched item and key counts keep input order unchanged");
    const std::vector<std::wstring> items = {L"dead.exe", L"live.exe"};
    const std::vector<std::wstring> keys = {L"live.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, keys, {L"live.exe"}), items);
}

void Test_J9_CaseDifferentItemUsesCallerSuppliedKey() {
    Case("J9 an item's case-different caller key controls activity ordering");
    const std::vector<std::wstring> items = {L"dead.exe", L"ACTIVE.EXE"};
    const std::vector<std::wstring> keys = {L"dead.exe", L"active.exe"};
    const std::vector<std::wstring> expected = {L"ACTIVE.EXE", L"dead.exe"};
    CHECK_EQ(cd::OrderHeavyByActivity(items, keys, {L"active.exe"}), expected);
}

void Test_J10_DisplayedActivityOrderRestoresCanonicalOrder() {
    Case("J10 displayed activity order restores canonical heavy-app order");
    const std::vector<std::wstring> displayed = {L"live.exe", L"first.exe", L"second.exe"};
    const std::vector<std::wstring> canonical = {L"first.exe", L"second.exe", L"live.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, canonical), canonical);
}

void Test_J11_NewHeavyAppIsAppendedAfterCanonicalEntries() {
    Case("J11 a heavy app absent from canonical order is appended last");
    const std::vector<std::wstring> displayed = {L"new.exe", L"second.exe", L"first.exe"};
    const std::vector<std::wstring> canonical = {L"first.exe", L"second.exe"};
    const std::vector<std::wstring> expected = {L"first.exe", L"second.exe", L"new.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, canonical), expected);
}

void Test_J12_NewHeavyAppsKeepDisplayedRelativeOrder() {
    Case("J12 new heavy apps keep their displayed relative order at the end");
    const std::vector<std::wstring> displayed = {
        L"new-b.exe", L"second.exe", L"new-a.exe", L"first.exe"};
    const std::vector<std::wstring> canonical = {L"first.exe", L"second.exe"};
    const std::vector<std::wstring> expected = {
        L"first.exe", L"second.exe", L"new-b.exe", L"new-a.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, canonical), expected);
}

void Test_J13_RemovedHeavyAppDoesNotReturnFromCanonicalOrder() {
    Case("J13 a removed heavy app does not return from canonical order");
    const std::vector<std::wstring> displayed = {L"third.exe", L"first.exe"};
    const std::vector<std::wstring> canonical = {L"first.exe", L"second.exe", L"third.exe"};
    const std::vector<std::wstring> expected = {L"first.exe", L"third.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, canonical), expected);
}

void Test_J14_EmptyCanonicalOrderKeepsDisplayedOrder() {
    Case("J14 an empty canonical order keeps displayed order unchanged");
    const std::vector<std::wstring> displayed = {L"second.exe", L"first.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, std::vector<std::wstring>()), displayed);
}

void Test_J15_EmptyDisplayedOrderStaysEmpty() {
    Case("J15 an empty displayed order stays empty");
    CHECK(cd::RestoreCanonicalOrder(std::vector<std::wstring>(), {L"first.exe"}).empty());
}

void Test_J16_DuplicateCanonicalStringsUseFirstIndexWithoutDroppingEntries() {
    Case("J16 duplicate canonical strings use the first index without dropping an entry");
    const std::vector<std::wstring> displayed = {L"second.exe", L"dup.exe", L"dup.exe"};
    const std::vector<std::wstring> canonical = {L"dup.exe", L"second.exe", L"dup.exe"};
    const std::vector<std::wstring> expected = {L"dup.exe", L"dup.exe", L"second.exe"};
    CHECK_EQ(cd::RestoreCanonicalOrder(displayed, canonical), expected);
}

// ===========================================================================
// K. Settings live-config reconciliation.
// ===========================================================================
void Test_K1_LiveEntryAddedBehindTheWindowIsReported() {
    Case("K1 a live entry absent from baseline and work is reported");
    const std::vector<std::size_t> expected = {1};
    CHECK(cd::IndicesAddedBehindTheWindow(
              {L"existing", L"tray-added"}, {L"existing"}, {L"existing"}) == expected);
}

void Test_K2_ProfileDeletedInSettingsIsNotResurrected() {
    Case("K2 an entry present in baseline but deleted from work is not reported");
    CHECK(cd::IndicesAddedBehindTheWindow(
              {L"kept", L"deleted"}, {L"kept", L"deleted"}, {L"kept"})
              .empty());
}

void Test_K3_LiveEntryAlreadyInWorkIsNotDuplicated() {
    Case("K3 a live entry already present in work is not reported");
    CHECK(cd::IndicesAddedBehindTheWindow(
              {L"existing", L"already-present"}, {L"existing"},
              {L"existing", L"already-present"})
              .empty());
}

void Test_K4_SeveralLiveAdditionsKeepAscendingIndexOrder() {
    Case("K4 several live additions are reported in ascending index order");
    const std::vector<std::size_t> expected = {0, 2, 3};
    CHECK(cd::IndicesAddedBehindTheWindow(
              {L"new-zero", L"existing", L"new-two", L"new-three"},
              {L"existing"}, {L"existing"}) == expected);
}

void Test_K5_EmptyLiveConfigReturnsNoIndices() {
    Case("K5 an empty live config returns no indices");
    CHECK(cd::IndicesAddedBehindTheWindow({}, {L"baseline"}, {L"work"}).empty());
}

void Test_K6_EmptyBaselineAndWorkReportEveryLiveIndex() {
    Case("K6 empty baseline and work report every live index");
    const std::vector<std::size_t> expected = {0, 1, 2};
    CHECK(cd::IndicesAddedBehindTheWindow({L"zero", L"one", L"two"}, {}, {}) == expected);
}

void Test_K7_KeyComparisonIsExactAndDoesNotFoldCase() {
    Case("K7 keys differing only by case are treated as different");
    const std::vector<std::size_t> expected = {0};
    CHECK(cd::IndicesAddedBehindTheWindow({L"Profile"}, {L"profile"}, {L"profile"}) ==
          expected);
}

// Pins the ABSENCE of the driver line. The kernel driver is PnP-loaded and runs whether or
// not the service does, so its state is noise; showing it read as "the stop failed".
void Test_L1_VCacheEnvironmentNamesOnlyTheService() {
    Case("L1 V-Cache environment wording names only the service");
    CHECK_EQ(cd::FormatAmdVCacheComponentsEnvironmentStatus(
                 cd::AmdVCacheServiceState::InstalledButStopped),
             L"AMD 3D V-Cache Performance Optimizer\r\n"
             L"  service (amd3dvcacheSvc): Installed but stopped");
}

void Test_M1_MissingVCacheOriginalStartDefaultsMinusOne() {
    Case("M1 missing vcache_original_start defaults -1");
    cd::Config c;
    std::wstring err;
    CHECK(cd::ParseConfig(L"[general]\nnotifications=1\n", c, &err));
    CHECK_EQ(c.vcacheOriginalStart, -1);
}

void Test_M2_VCacheOriginalStartThreeRoundTrips() {
    Case("M2 vcache_original_start=3 parses and round-trips as 3");
    cd::Config parsed;
    std::wstring err;
    CHECK(cd::ParseConfig(L"[general]\nvcache_original_start=3\n", parsed, &err));
    CHECK_EQ(parsed.vcacheOriginalStart, 3);
    const std::wstring serialized = cd::SerializeConfig(parsed);
    CHECK(serialized.find(L"vcache_original_start=3") != std::wstring::npos);
    cd::Config roundTripped;
    CHECK(cd::ParseConfig(serialized, roundTripped, &err));
    CHECK_EQ(roundTripped.vcacheOriginalStart, 3);
}

void Test_M7_VCacheRestoreHintPinsMissingManualAndBootStartValues() {
    Case("M7 V-Cache restore hint is absent only when no original Start value is recorded");
    const std::wstring missing = cd::FormatVCacheRestoreHint(-1);
    CHECK(missing.empty());

    CHECK_EQ(cd::FormatVCacheRestoreHint(3),
             L"Game Optimizer recorded this driver's original Start value as 3. Turn this "
             L"setting off before deleting the app's settings folder, or that value is lost "
             L"and the driver stays disabled.");

    const std::wstring bootStart = cd::FormatVCacheRestoreHint(0);
    CHECK(!bootStart.empty());
    CHECK_EQ(bootStart,
             L"Game Optimizer recorded this driver's original Start value as 0. Turn this "
             L"setting off before deleting the app's settings folder, or that value is lost "
             L"and the driver stays disabled.");
}

void Test_M3_DisabledWhileRunningRequiresRestart() {
    Case("M3 configured Disabled plus Running says restart required");
    CHECK_EQ(cd::FormatAmdVCacheComponentEnvironmentLine(
                 L"driver", L"amd3dvcache", cd::AmdVCacheServiceState::Running, 4),
             L"driver (amd3dvcache): Running, start type Disabled - restart required");
}

void Test_M4_ManualWhileRunningNeedsNoNotice() {
    Case("M4 configured Manual plus Running has no restart notice");
    CHECK_EQ(cd::FormatAmdVCacheComponentEnvironmentLine(
                 L"driver", L"amd3dvcache", cd::AmdVCacheServiceState::Running, 3),
             L"driver (amd3dvcache): Running, start type Manual");
}

void Test_M5_DisabledWhileStoppedNeedsNoNotice() {
    Case("M5 configured Disabled plus Stopped has no restart notice");
    CHECK_EQ(cd::FormatAmdVCacheComponentEnvironmentLine(
                 L"driver", L"amd3dvcache",
                 cd::AmdVCacheServiceState::InstalledButStopped, 4),
             L"driver (amd3dvcache): Installed but stopped, start type Disabled");
}

void Test_M6_ManualWhileStoppedRequiresRestart() {
    Case("M6 configured Manual plus Stopped says restart required");
    CHECK_EQ(cd::FormatAmdVCacheComponentEnvironmentLine(
                 L"driver", L"amd3dvcache",
                 cd::AmdVCacheServiceState::InstalledButStopped, 3),
             L"driver (amd3dvcache): Installed but stopped, start type Manual - restart required");
}

void Test_N1_NoDetectedConditionDoesNotShow() {
    Case("N1 Game Mode Off and no V-Cache does not show");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(!decision.Any());
}

void Test_N2_MultiDomainAmdGameModeIsActionable() {
    Case("N2 Game Mode On on multi-domain AMD is actionable");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd());
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Actionable);
}

void Test_N3_SingleDomainGameModeIsInformational() {
    Case("N3 Game Mode On on one domain is informational");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Informational);
}

void Test_N4_NonAmdGameModeIsInformational() {
    Case("N4 Game Mode On on non-AMD multi-domain CPU is informational");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = false;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd());
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Informational);
}

void Test_N5_NotDeterminableDoesNotShowGameMode() {
    Case("N5 Game Mode NotDeterminable does not show Game Mode");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::NotDeterminable;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd());
    CHECK(!decision.showGameMode);
}

void Test_N6_VCacheOnlyShows() {
    Case("N6 V-Cache agent running with Game Mode Off shows V-Cache only");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    env.amdVCacheServicePresent = true;
    env.amdVCacheServiceRunning = true;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(!decision.showGameMode);
    CHECK(decision.showVCache);
    CHECK(decision.Any());
}

void Test_N7_BothDetectedConditionsShow() {
    Case("N7 Game Mode On and V-Cache agent running show both");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.amdVCacheDriverPresent = true;
    env.amdVCacheDriverRunning = true;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(decision.showGameMode);
    CHECK(decision.showVCache);
    CHECK(decision.Any());
}

void Test_N8_WizardVCacheLeadInIsUnchanged() {
    Case("N8 wizard V-Cache text keeps the existing lead-in");
    const cd::EnvironmentInfo env;
    const std::wstring text = cd::Page2VCacheText(env, true);
    const std::wstring expected =
        L"Separately, and regardless of the Game Mode setting above:";
    CHECK(text.compare(0, expected.size(), expected) == 0);
}

void Test_N9_StandaloneVCacheTextOmitsLeadIn() {
    Case("N9 standalone V-Cache text omits the wizard-only lead-in");
    const cd::EnvironmentInfo env;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"above:") == std::wstring::npos);
    CHECK(text.compare(0, 10, L"Separately") != 0);
}

// == S. Page2VCacheText now keys on agent running state, not service state ==

void Test_S1_AgentRunningShowsActive() {
    Case("S1 agent running -> Page2VCacheText contains 'is ACTIVE' and 'amd3dvcacheUser.exe'");
    cd::EnvironmentInfo env;
    env.amdVCacheServiceState = cd::AmdVCacheServiceState::NotInstalled;
    env.amdVCacheAgentRunning = true;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"is ACTIVE") != std::wstring::npos);
    CHECK(text.find(L"amd3dvcacheUser.exe") != std::wstring::npos);
}

void Test_S2_AgentNotRunningServiceStoppedShowsNotActive() {
    Case("S2 agent NOT running, service InstalledButStopped -> text contains 'NOT ACTIVE' and "
         "does NOT contain old sentence, contains new one");
    cd::EnvironmentInfo env;
    env.amdVCacheServiceState = cd::AmdVCacheServiceState::InstalledButStopped;
    env.amdVCacheAgentRunning = false;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"NOT ACTIVE") != std::wstring::npos);
    CHECK(text.find(L"not expressing a preference") == std::wstring::npos);  // old text gone
    CHECK(text.find(L"is not running, so nothing here is expressing a CCD preference") != std::wstring::npos);
}

void Test_S3_ServiceRunningButAgentNotRunningShowsNotActive() {
    Case("S3 REGRESSION: service Running but agent NOT running -> text must say NOT ACTIVE");
    cd::EnvironmentInfo env;
    env.amdVCacheServiceState = cd::AmdVCacheServiceState::Running;
    env.amdVCacheAgentRunning = false;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"NOT ACTIVE") != std::wstring::npos);
    // Under the old code, this would have said "INSTALLED and RUNNING" - prove it does not now
    CHECK(text.find(L"INSTALLED and RUNNING") == std::wstring::npos);
}

void Test_S4_ServiceStoppedButAgentRunningShowsActive() {
    Case("S4 REGRESSION: service InstalledButStopped but agent RUNNING -> text must say ACTIVE");
    cd::EnvironmentInfo env;
    env.amdVCacheServiceState = cd::AmdVCacheServiceState::InstalledButStopped;
    env.amdVCacheAgentRunning = true;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"is ACTIVE") != std::wstring::npos);
    // Prove it does not wrongly say NOT RUNNING
    CHECK(text.find(L"is not running") == std::wstring::npos);
}

void Test_S5_NotInstalledAlwaysShowsNotInstalled() {
    Case("S5 NotInstalled -> contains 'NOT installed', lead-in rule still holds");
    cd::EnvironmentInfo env;
    env.amdVCacheServiceState = cd::AmdVCacheServiceState::NotInstalled;
    env.amdVCacheAgentRunning = false;
    const std::wstring text = cd::Page2VCacheText(env, false);
    CHECK(text.find(L"NOT installed") != std::wstring::npos);
    CHECK(text.find(L"Separately, and regardless") == std::wstring::npos);  // lead-in omitted when false
}

void Test_N10_VCacheInstalledButNotRunningDoesNotShow() {
    Case("N10 V-Cache installed but not running does not show");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    env.amdVCacheServicePresent = true;
    env.amdVCacheDriverPresent = true;
    // Nothing is running, and in particular the agent is not: the policy engine
    // is installed but idle, so no warning.
    env.amdVCacheAgentRunning = false;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(!decision.showVCache);
    CHECK(!decision.Any());
}

void Test_N11_DriverAndServiceRunningWithoutAgentDoesNotShow() {
    Case("N11 driver and service running without the agent does not show");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    env.amdVCacheServicePresent = true;
    env.amdVCacheServiceRunning = true;
    env.amdVCacheDriverPresent = true;
    env.amdVCacheDriverRunning = true;
    env.amdVCacheAgentRunning = false;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(!decision.showVCache);
}

void Test_N12_AgentRunningShowsVCache() {
    Case("N12 agent running shows V-Cache");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    env.amdVCacheServicePresent = true;
    env.amdVCacheServiceRunning = true;
    env.amdVCacheDriverPresent = true;
    env.amdVCacheDriverRunning = true;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain());
    CHECK(decision.showVCache);
    CHECK(decision.Any());
}

cd::Mask MakeMask(const std::wstring& name, const std::vector<ULONG>& ids, bool derived) {
    cd::Mask mask;
    mask.name = name;
    mask.ids = ids;
    mask.derived = derived;
    return mask;
}

void CheckMasksExactly(const std::vector<cd::Mask>& actual,
                       const std::vector<cd::Mask>& expected) {
    CHECK_EQ(actual.size(), expected.size());
    const size_t count = (std::min)(actual.size(), expected.size());
    for (size_t i = 0; i < count; ++i) {
        CHECK_EQ(actual[i].name, expected[i].name);
        CHECK_EQ(actual[i].ids, expected[i].ids);
        CHECK_EQ(actual[i].derived, expected[i].derived);
    }
}

void Test_O1_NoCustomMasksReturnsDerivedExactly() {
    Case("O1 no custom masks returns derived exactly in derived order");
    const std::vector<cd::Mask> derived = {
        MakeMask(L"Cache", {256u, 257u}, true),
        MakeMask(L"All", {256u, 257u, 258u}, true),
    };
    const std::vector<cd::Mask> existing = {
        MakeMask(L"Old Cache", {300u}, true),
        MakeMask(L"Old All", {300u, 301u}, true),
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), derived);
}

void Test_O2_OneCustomIsAppendedAfterDerived() {
    Case("O2 one non-colliding custom follows every derived mask");
    const std::vector<cd::Mask> derived = {
        MakeMask(L"Cache", {256u}, true),
        MakeMask(L"All", {256u, 257u}, true),
    };
    const cd::Mask custom = MakeMask(L"Streaming", {257u}, false);
    const std::vector<cd::Mask> existing = {custom};
    const std::vector<cd::Mask> expected = {derived[0], derived[1], custom};

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), expected);
}

void Test_O3_ExactNameCollisionKeepsDerivedAndDropsCustom() {
    Case("O3 exact name collision keeps DERIVED and drops custom");
    const cd::Mask live = MakeMask(L"Cache", {256u, 257u}, true);
    const std::vector<cd::Mask> derived = {live};
    const std::vector<cd::Mask> existing = {
        MakeMask(L"Cache", {999u}, false),
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), {live});
}

void Test_O4_CaseInsensitiveCollisionKeepsDerivedAndDropsCustom() {
    Case("O4 case-insensitive collision keeps DERIVED and drops custom");
    const cd::Mask live = MakeMask(L"Cache", {256u, 257u}, true);
    const std::vector<cd::Mask> derived = {live};
    const std::vector<cd::Mask> existing = {
        MakeMask(L"cache", {999u}, false),
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), {live});
}

void Test_O5_SeveralCustomsKeepTheirRelativeOrder() {
    Case("O5 several customs retain existing relative order after derived masks");
    const cd::Mask live = MakeMask(L"All", {256u, 257u, 258u}, true);
    const cd::Mask first = MakeMask(L"Game", {256u}, false);
    const cd::Mask second = MakeMask(L"Capture", {257u}, false);
    const cd::Mask third = MakeMask(L"Compile", {258u}, false);
    const std::vector<cd::Mask> derived = {live};
    const std::vector<cd::Mask> existing = {first, second, third};
    const std::vector<cd::Mask> expected = {live, first, second, third};

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), expected);
}

void Test_O6_StaleDerivedMaskIsDropped() {
    Case("O6 stale derived mask absent from live hardware is dropped");
    const cd::Mask live = MakeMask(L"All", {256u, 257u}, true);
    const std::vector<cd::Mask> derived = {live};
    const std::vector<cd::Mask> existing = {
        MakeMask(L"Old CCD", {900u, 901u}, true),
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, existing), {live});
}

void Test_O7_EmptyExistingReturnsDerivedExactly() {
    Case("O7 empty existing list returns derived exactly");
    const std::vector<cd::Mask> derived = {
        MakeMask(L"Cache", {256u}, true),
        MakeMask(L"All", {256u, 257u}, true),
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom(derived, {}), derived);
}

void Test_O8_EmptyDerivedStillPreservesCustoms() {
    Case("O8 empty derived list still preserves custom masks");
    const cd::Mask first = MakeMask(L"Game", {256u}, false);
    const cd::Mask second = MakeMask(L"Capture", {257u}, false);
    const std::vector<cd::Mask> existing = {
        first,
        MakeMask(L"Stale derived", {900u}, true),
        second,
    };

    CheckMasksExactly(cd::MergeMasksPreservingCustom({}, existing), {first, second});
}

void Test_O9_PreservedCustomIndicatorMatchesTheMerge() {
    Case("O9 preserved-custom indicator is true for O2 and false for O1");
    const std::vector<cd::Mask> derived = {
        MakeMask(L"Cache", {256u}, true),
        MakeMask(L"All", {256u, 257u}, true),
    };
    const std::vector<cd::Mask> o2Existing = {
        MakeMask(L"Streaming", {257u}, false),
    };
    const std::vector<cd::Mask> o1Existing = {
        MakeMask(L"Old Cache", {300u}, true),
    };

    CHECK(cd::MergePreservedCustomMasks(derived, o2Existing));
    CHECK(!cd::MergePreservedCustomMasks(derived, o1Existing));
}

// ===========================================================================
// P. Startup warning popup wording.
//
// The popup and the first-run wizard have DELIBERATELY DIVERGED. The wizard keeps its
// page-2 body; the popup gets its own short text, because the wizard is something the user
// chose to open and the popup opens itself at every login. P1-P4 pin what the popup says,
// P5 pins that the wizard was not edited into it.
// ===========================================================================
void Test_P1_PopupActionableTextIsExact() {
    Case("P1 popup actionable Game Mode text matches character for character");
    const std::wstring expected =
        L"Windows Game Mode is on. On a multi-CCD AMD part it applies a machine-wide CCD preference "
        L"to whatever it decides is the game. That is not per-game, and it competes with the masks "
        L"Game Optimizer applies. If your game ends up on the wrong CCD, check this first.";
    CHECK_EQ(cd::PopupGameModeText(true), expected);
}

void Test_P2_PopupInformationalTextIsExact() {
    Case("P2 popup informational Game Mode text matches character for character");
    const std::wstring expected =
        L"Windows Game Mode is on. This CPU has a single cache domain, so Game Mode has no CCD "
        L"preference to apply here and is not competing with Game Optimizer.";
    CHECK_EQ(cd::PopupGameModeText(false), expected);
}

void Test_P3_PopupSaysNeitherPageNorAttention() {
    Case("P3 popup text names no page and asks for no attention");
    // THE REGRESSION GUARD FOR THIS WHOLE CHANGE. The wizard body this window used to print
    // ends "Nothing on this page needs your attention" - a sentence with two defects here:
    // there is no page, and a window that opened by itself to say nothing needs attention
    // argues against its own existence. If anyone pastes it back in, this fails.
    const std::wstring info = cd::PopupGameModeText(false);
    CHECK(info.find(L"page") == std::wstring::npos);
    CHECK(info.find(L"attention") == std::wstring::npos);
    CHECK(info.find(L"Nothing on this page needs your attention") == std::wstring::npos);
    // The actionable branch is held to the same bar; it is the louder of the two.
    const std::wstring act = cd::PopupGameModeText(true);
    CHECK(act.find(L"page") == std::wstring::npos);
    CHECK(act.find(L"attention") == std::wstring::npos);
}

void Test_P4_PopupShowsNoRegistryPath() {
    Case("P4 neither popup branch prints a registry value name");
    // The popup is not the wizard. A user who wants the registry detail can open the wizard
    // or the Settings environment panel, both of which still carry it.
    CHECK(cd::PopupGameModeText(true).find(L"AutoGameModeEnabled") == std::wstring::npos);
    CHECK(cd::PopupGameModeText(false).find(L"AutoGameModeEnabled") == std::wstring::npos);
}

void Test_P5_WizardGameModeTextIsUnchanged() {
    Case("P5 wizard page-2 Game Mode text still says its own sentence");
    // ASSERTS THE TWO SURFACES GENUINELY DIVERGED rather than one having been edited into
    // the other. Game Mode ON with a single cache domain is the wizard informational branch:
    // the same machine shape P2 covers for the popup, and the two must not agree.
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    bool warn = true;
    const std::wstring wizard = cd::Page2GameModeText(env, MakeSingleDomain(), warn);
    CHECK(!warn);
    CHECK(wizard.find(L"Nothing on this page needs your attention") != std::wstring::npos);
    CHECK_NE(wizard, cd::PopupGameModeText(false));
}

// ===========================================================================
// Q. The fields Settings never edits.
//
// ApplyChanges ends by writing the Settings window's snapshot over the live config. Three
// fields have no control in that window at all and are written from OUTSIDE it while it sits
// open: the tray's Pause item, a newer build's unparsed sections, and vcache_original_start,
// which the ELEVATED `--vcache-set` child writes to config.ini on disk.
//
// The last of those is the one that costs the user something, and the startup warning window
// can now trigger it: the warning disables the driver, the child records the original Start
// value 3, the user then closes the warning and clicks OK in the Settings window that was
// open behind it. If the snapshot won, the only record able to restore that driver would be
// overwritten with -1 and the driver would be stranded disabled.
//
// THE RULE IS "THE SNAPSHOT NEVER WINS", NOT "THE LARGER VALUE WINS" - hence Q2, which runs
// the same reconciliation in the opposite direction.
// ===========================================================================
void Test_Q1_LiveVCacheOriginalStartSurvivesTheSettingsSnapshot() {
    Case("Q1 a vcache_original_start recorded behind the window survives ApplyChanges");
    cd::Config live;
    live.vcacheOriginalStart = 3;                    // the child recorded Manual, on disk
    live.paused = true;                              // the tray paused, while Settings was open
    live.unknown[L"future"].push_back(L"key=value"); // a newer build's section

    cd::Config work;                                 // the snapshot, taken before all of that
    work.vcacheOriginalStart = -1;
    work.paused = false;

    cd::PreserveFieldsSettingsNeverEdits(live, work);
    CHECK_EQ(work.vcacheOriginalStart, 3);
    // The two fields this reconciliation already protected must still be protected - the
    // helper was extracted from ApplyChanges, and dropping one of them would be silent.
    CHECK(work.paused);
    CHECK(work.unknown.find(L"future") != work.unknown.end());
}

void Test_Q2_ClearedVCacheOriginalStartAlsoBeatsTheSnapshot() {
    Case("Q2 a cleared live vcache_original_start also beats a stale recorded value");
    cd::Config live;
    live.vcacheOriginalStart = -1;   // the driver was restored and the record cleared
    live.paused = false;             // and the tray un-paused, likewise behind the window

    cd::Config work;
    work.vcacheOriginalStart = 3;    // Settings still holds what it read when it opened
    work.paused = true;

    cd::PreserveFieldsSettingsNeverEdits(live, work);
    CHECK_EQ(work.vcacheOriginalStart, -1);
    CHECK(!work.paused);
}

// ===========================================================================
// R. Re-checking AMD's V-Cache agent at pin time
//
// The engine probes for amd3dvcacheUser.exe on the tick, because the agent can be launched
// after this app was and a startup-only probe would never see it. A tick runs four times a
// second, so the probe RESULT must not reach the log - only a change in it may. That decision
// is the pure function these cases drive; the probe itself is a process snapshot and is not
// testable here.
// ===========================================================================
void Test_R1_UnchangedAgentStateDoesNotLog() {
    Case("R1 an unchanged agent state produces no log line");
    CHECK(!cd::ShouldLogAgentChange(cd::AgentSeen::Absent, false));
    CHECK(!cd::ShouldLogAgentChange(cd::AgentSeen::Present, true));
    // The same question in the two-state form, which is what a caller that has already probed
    // once is really asking.
    CHECK(!cd::ShouldLogAgentChange(false, false));
    CHECK(!cd::ShouldLogAgentChange(true, true));
}

void Test_R2_AgentAppearingLogs() {
    Case("R2 the agent appearing mid-session logs once");
    CHECK(cd::ShouldLogAgentChange(cd::AgentSeen::Absent, true));
    CHECK(cd::ShouldLogAgentChange(false, true));
}

void Test_R3_AgentDisappearingLogs() {
    Case("R3 the agent going away mid-session logs once");
    CHECK(cd::ShouldLogAgentChange(cd::AgentSeen::Present, false));
    CHECK(cd::ShouldLogAgentChange(true, false));
}

void Test_R4_FirstProbeOfARunAlwaysLogs() {
    Case("R4 the first probe of a run logs whichever way it lands");
    // Never is not Absent. A machine where the agent has been running since boot never
    // CHANGES, so a bool seeded false would call the first probe a change it was not, and a
    // bool seeded true would report nothing at all on a machine that has the agent.
    CHECK(cd::ShouldLogAgentChange(cd::AgentSeen::Never, true));
    CHECK(cd::ShouldLogAgentChange(cd::AgentSeen::Never, false));
}

void Test_R5_TheSecondProbeOfAnUnchangedRunIsSilent() {
    Case("R5 after the first probe an unchanged answer is silent");
    // The engine's own sequence: probe, log, remember, probe again. AgentSeenFrom is the
    // "remember" step, and getting it wrong would log the same line four times a second.
    const bool measured = true;
    CHECK(cd::ShouldLogAgentChange(cd::AgentSeen::Never, measured));
    CHECK(!cd::ShouldLogAgentChange(cd::AgentSeenFrom(measured), measured));
    CHECK(cd::AgentSeenFrom(true) == cd::AgentSeen::Present);
    CHECK(cd::AgentSeenFrom(false) == cd::AgentSeen::Absent);
}

// ===========================================================================
// V. Naming a custom mask (mask_edit.h). Written from the header comments and from
// docs\superpowers\specs\2026-08-31-custom-masks-design.md section 5.
// ===========================================================================

void Test_V1_EmptyAndWhitespaceOnlyNamesAreEmpty() {
    Case("V1 empty and whitespace-only names are rejected as Empty");
    const std::vector<cd::Mask> none;
    CHECK_EQ(cd::ValidateNewMaskName(L"", none, none), cd::MaskNameProblem::Empty);
    CHECK_EQ(cd::ValidateNewMaskName(L" ", none, none), cd::MaskNameProblem::Empty);
    CHECK_EQ(cd::ValidateNewMaskName(L"   ", none, none), cd::MaskNameProblem::Empty);
    CHECK_EQ(cd::ValidateNewMaskName(L"\t \r\n", none, none), cd::MaskNameProblem::Empty);
    // Empty is checked FIRST: a blank name is Empty even when the lists could match it.
    const std::vector<cd::Mask> blank = {MakeMask(L"", {256u}, true)};
    CHECK_EQ(cd::ValidateNewMaskName(L"", blank, blank), cd::MaskNameProblem::Empty);
}

void Test_V2_CaseInsensitiveDuplicateOfExistingMask() {
    Case("V2 \"cache\" duplicates an existing \"Cache\" (case-insensitive)");
    const std::vector<cd::Mask> existing = {
        MakeMask(L"Cache", {256u, 257u}, true),
        MakeMask(L"All", {256u, 257u, 258u}, true),
    };
    // Deliberately empty, so only the Duplicate rule can produce a rejection here.
    const std::vector<cd::Mask> derived;
    CHECK_EQ(cd::ValidateNewMaskName(L"cache", existing, derived),
             cd::MaskNameProblem::Duplicate);
    CHECK_EQ(cd::ValidateNewMaskName(L"CACHE", existing, derived),
             cd::MaskNameProblem::Duplicate);
    CHECK_EQ(cd::ValidateNewMaskName(L"Cache", existing, derived),
             cd::MaskNameProblem::Duplicate);
    // Trimming happens before the comparison.
    CHECK_EQ(cd::ValidateNewMaskName(L"  cache  ", existing, derived),
             cd::MaskNameProblem::Duplicate);
    // A hand-made existing mask is protected exactly like a derived one.
    const std::vector<cd::Mask> custom = {MakeMask(L"Streaming", {257u}, false)};
    CHECK_EQ(cd::ValidateNewMaskName(L"streaming", custom, derived),
             cd::MaskNameProblem::Duplicate);
}

void Test_V3_NameDeriveMasksEmitsIsReserved() {
    Case("V3 a name DeriveMasks emits for this machine is reserved even when absent today");
    const std::vector<cd::Mask> derived = cd::DeriveMasks(MakeSymmetricDualCcd());
    CHECK(MaskNamed(derived, L"CCD0") != nullptr);  // guard: the helper really emits it
    const std::vector<cd::Mask> existing;            // no mask of that name exists yet
    CHECK_EQ(cd::ValidateNewMaskName(L"CCD0", existing, derived),
             cd::MaskNameProblem::ReservedDerivedName);
    CHECK_EQ(cd::ValidateNewMaskName(L"ccd0", existing, derived),
             cd::MaskNameProblem::ReservedDerivedName);
    CHECK_EQ(cd::ValidateNewMaskName(L"CCD0 no SMT", existing, derived),
             cd::MaskNameProblem::ReservedDerivedName);
    // Duplicate is checked BEFORE Reserved when both would fire.
    const std::vector<cd::Mask> already = {MakeMask(L"CCD0", {256u}, true)};
    CHECK_EQ(cd::ValidateNewMaskName(L"ccd0", already, derived),
             cd::MaskNameProblem::Duplicate);
}

void Test_V4_FreshNameIsAcceptedAndTrimmed() {
    Case("V4 a fresh name passes, and TrimMaskName strips both ends");
    const std::vector<cd::Mask> derived = cd::DeriveMasks(MakeSymmetricDualCcd());
    const std::vector<cd::Mask> existing = {MakeMask(L"Streaming", {257u}, false)};
    CHECK_EQ(cd::ValidateNewMaskName(L"Recording", existing, derived),
             cd::MaskNameProblem::None);
    CHECK_EQ(cd::ValidateNewMaskName(L"  Recording  ", existing, derived),
             cd::MaskNameProblem::None);
    CHECK_EQ(cd::TrimMaskName(L"  Recording \t"), std::wstring(L"Recording"));
    CHECK_EQ(cd::TrimMaskName(L"\r\nRecording"), std::wstring(L"Recording"));
    CHECK_EQ(cd::TrimMaskName(L"Recording"), std::wstring(L"Recording"));
    CHECK_EQ(cd::TrimMaskName(L"Two words "), std::wstring(L"Two words"));  // inner kept
    CHECK_EQ(cd::TrimMaskName(L"   "), std::wstring());
    CHECK_EQ(cd::TrimMaskName(L""), std::wstring());
}

void Test_V5_ProfilesReferencingMaskListsEachOnceInConfigOrder() {
    Case("V5 ProfilesReferencingMask: every referencing profile once, in config order");
    cd::Config c;
    auto add = [&](const wchar_t* name, const wchar_t* game, const wchar_t* heavy) {
        cd::Profile p;
        p.name = name;
        p.gameMask = game;
        p.heavyMask = heavy;
        c.profiles.push_back(p);
    };
    add(L"Overwatch", L"Streaming", L"All");   // game only
    add(L"Untouched", L"CCD0", L"All");        // neither
    add(L"Compile", L"CCD0", L"streaming");    // heavy only, different case
    add(L"Both", L"Streaming", L"Streaming");  // both roles - listed once

    const std::vector<std::wstring> want = {L"Overwatch", L"Compile", L"Both"};
    CHECK_EQ(cd::ProfilesReferencingMask(c, L"Streaming"), want);
    CHECK_EQ(cd::ProfilesReferencingMask(c, L"STREAMING"), want);
    CHECK_EQ(cd::ProfilesReferencingMask(c, L"Nobody"), std::vector<std::wstring>());
    const std::vector<std::wstring> allUsers = {L"Overwatch", L"Untouched"};
    CHECK_EQ(cd::ProfilesReferencingMask(c, L"All"), allUsers);
}

void Test_V8_CanRemoveMaskOnlyForCustomMasks() {
    Case("V8 CanRemoveMask: null -> false, derived -> false, custom -> true, derived NAME -> false");
    // What DeriveMasks would emit for this machine; the predicate must refuse these names
    // whatever flag the selected mask carries.
    const std::vector<cd::Mask> derivedList = {
        MakeMask(L"CCD0", {256u}, true),
        MakeMask(L"Cache", {256u, 257u}, true),
    };
    CHECK(!cd::CanRemoveMask(nullptr, derivedList));
    const cd::Mask derived = MakeMask(L"CCD0", {256u}, true);
    CHECK(!cd::CanRemoveMask(&derived, derivedList));
    const cd::Mask custom = MakeMask(L"Streaming", {257u}, false);
    CHECK(cd::CanRemoveMask(&custom, derivedList));
    // A hand-edited derived mask reports derived == false (the Core map write-back clears the
    // flag on any edit) but keeps its name. The merge that would re-derive it runs only on a
    // topology-signature change, so on the same hardware a removed "CCD0" is gone for good and
    // Add then refuses the name as reserved. The predicate therefore keys on the NAME as well
    // as the flag - case-insensitively, because FindMask and the merge compare with IEquals.
    // (This case used to assert true, when the rule keyed on the flag alone.)
    const cd::Mask editedDerivedName = MakeMask(L"ccd0", {258u}, false);
    CHECK(!cd::CanRemoveMask(&editedDerivedName, derivedList));
}

void Test_V9_DerivableNamesAreReservedEverywhere() {
    Case("V9 derivable names are reserved on EVERY machine, even when this one emits none");
    // Empty existing list and empty derived list: the only thing that can refuse a name is the
    // machine-independent vocabulary check, so each ReservedDerivedName below proves that check
    // alone. Founder ruling 2026-09-01 ("Reserve all"): a custom "Cache" made on a single-domain
    // CPU used to be accepted, and won the merge as the hardware domain the day the config moved
    // to an X3D part.
    const std::vector<cd::Mask> none;
    const auto v = [&none](const wchar_t* raw) { return cd::ValidateNewMaskName(raw, none, none); };
    const cd::MaskNameProblem reserved = cd::MaskNameProblem::ReservedDerivedName;
    const cd::MaskNameProblem ok = cd::MaskNameProblem::None;
    CHECK(v(L"Cache") == reserved);
    CHECK(v(L"cache") == reserved);
    CHECK(v(L"CCD0") == reserved);
    CHECK(v(L"ccd12") == reserved);
    CHECK(v(L"P-cores") == reserved);
    CHECK(v(L"e-cores") == reserved);
    CHECK(v(L"All") == reserved);
    CHECK(v(L"all no smt") == reserved);
    CHECK(v(L"  CCD3  ") == reserved);  // trimmed before the check, like every other name
    // DeriveMasks also emits "<label> no SMT" beside every domain label (topology.cpp,
    // `groups[i].label + L" no SMT"`), so those are reserved too.
    CHECK(v(L"Cache no SMT") == reserved);
    CHECK(v(L"ccd1 no smt") == reserved);
    CHECK(v(L"P-cores no SMT") == reserved);
    CHECK(v(L"E-cores no SMT") == reserved);
    // The Freq family, on an AmdAsymmetricCache part: every domain that is not the largest L3
    // is "Freq", "Freq 2", "Freq 3", ... (topology.cpp:203-204), each with a " no SMT" twin.
    // THIS WHOLE FAMILY WAS MISSED ON THE FIRST PASS BECAUSE THE LABEL IS BUILT WITH A TERNARY
    // (`g.label = (freqSeq == 1) ? std::wstring(L"Freq") : (L"Freq " + std::to_wstring(...))`)
    // RATHER THAN A PLAIN STRING LITERAL, so the grep for `label = L"` that enumerated the
    // vocabulary could not see it - and README.md documents `Freq` / `Freq no SMT` as derived
    // masks on a 9950X3D the whole time. A vocabulary check is only as complete as the sweep
    // that built it.
    CHECK(v(L"Freq") == reserved);
    CHECK(v(L"freq") == reserved);
    CHECK(v(L"Freq 2") == reserved);
    CHECK(v(L"Freq 10") == reserved);
    CHECK(v(L"Freq no SMT") == reserved);
    CHECK(v(L"Freq 2 no SMT") == reserved);
    CHECK(v(L"FREQ NO SMT") == reserved);
    // Not derivable: CCD needs at least one digit and nothing after them, and the suffix alone
    // reserves nothing.
    CHECK(v(L"CCD") == ok);
    CHECK(v(L"CCDx") == ok);
    CHECK(v(L"CCD 0") == ok);
    CHECK(v(L"Streaming") == ok);
    CHECK(v(L"Cache2") == ok);
    CHECK(v(L"Streaming no SMT") == ok);
    // Freq needs the single space to_wstring is appended after, then digits and nothing else.
    // None of these four is a name BaseGroups can emit, so none may be taken from the user.
    CHECK(v(L"Freq2") == ok);
    CHECK(v(L"Freqx") == ok);
    CHECK(v(L"Freq x") == ok);
    CHECK(v(L"Frequency") == ok);
    CHECK(cd::IsDerivableMaskName(L"CCD7"));
    CHECK(!cd::IsDerivableMaskName(L"ccd"));
    CHECK(!cd::IsDerivableMaskName(L"CCD-1"));
    CHECK(cd::IsDerivableMaskName(L"Freq 3"));
    CHECK(!cd::IsDerivableMaskName(L"Freq "));  // prefix alone, no index: never emitted
}

void Test_V6_TopologyChangedPreservedSentence() {
    Case("V6 topology-changed sentence: silent at zero, names the count otherwise");
    CHECK_EQ(cd::TopologyChangedPreservedSentence(0), std::wstring());

    const std::wstring one = cd::TopologyChangedPreservedSentence(1);
    const std::wstring three = cd::TopologyChangedPreservedSentence(3);
    CHECK(!one.empty());
    CHECK(!three.empty());
    CHECK(one.find(L"1 custom mask") != std::wstring::npos);
    CHECK(three.find(L"3 custom masks") != std::wstring::npos);

    // Pinned wording. The sentence is appended to a MessageBox nobody can assert on.
    CHECK_EQ(one, std::wstring(L"1 custom mask you created was kept, but the processor "
                               L"numbers inside it may now refer to different cores - open "
                               L"the Core map and check it."));
    CHECK_EQ(three, std::wstring(L"3 custom masks you created were kept, but the processor "
                                 L"numbers inside them may now refer to different cores - "
                                 L"open the Core map and check each one."));
}

}  // namespace

// ===========================================================================
int main() {
    std::printf("Game Optimizer unit tests\n");
    std::printf("=======================\n");

    std::printf("\n== A. Topology ==\n");
    Test_A1_ReferenceMachine();
    Test_A2_InvertedDomainOrder();
    Test_A3_IntelHybrid();
    Test_A4_SymmetricDualCcd();
    Test_A5_SingleDomain();
    Test_A6_NoSmtMachine();
    Test_A7_ReduceToNoSmt();
    Test_A8_DefaultGameMaskAlwaysExists();
    Test_A9_DefaultFallsBackWhenTheGroupHasNoMask();

    std::printf("\n== B. Config ==\n");
    Test_B1_B2_B3_RoundTrip();
    Test_B4_LineEndingsAndBom();
    Test_B5_MalformedLineInTheMiddle();
    Test_B6_UnknownPreserved();
    Test_B7_ValidateAndRepair();
    Test_B8_DefaultConfig();
    Test_B9_IsExcludedCaseInsensitive();
    Test_B10_AllGamesAndLastUsedRoundTrip();
    Test_B11_ProfilesForDisplay();
    Test_B12_ValidateAndRepairNewRules();
    Test_B13_MarkProfileUsed();
    Test_B14_DefaultExclusionsProtectAtieclxx();
    Test_B15_DefaultExclusionsProtectAtiesrxx();
    Test_B16_DefaultExclusionsProtectAmdow();
    Test_B17_DefaultExclusionsProtectAmd3dvcachePrefix();
    Test_B18_DefaultExclusionsProtectAmdfendrPrefix();
    Test_B19_DefaultExclusionsProtectAmdAppCompatPrefix();
    Test_B20_DefaultExclusionsProtectAmdPpkgPrefix();
    Test_B21_DefaultExclusionsProtectAmdRsSourceExtension();

    std::printf("\n== C. ComputeDesired ==\n");
    Test_C1_NoMatch();
    Test_C2_C3_C4_C5_C8();
    Test_C8b_ZeroAndFourViaHeavyList();
    Test_C6_AutoPinInertWhenForegroundNotInGameSet();
    Test_C7_Stickiness();
    Test_C9_SelfSubtreeIsNeverAutoPinned();
    Test_C10_SelfSubtreeVetoesAStickyName();
    Test_C11_AutoPinnedOutIsExactlyRuleFour();
    Test_C12_AutoPinExpandsExeGroupWithPerPidVetoes();

    std::printf("\n== D. BuildTooltip ==\n");
    Test_D_BuildTooltip();

    std::printf("\n== E. CPU Sets readback ==\n");
    Test_E1_MaskNameForIds();
    Test_E2_ClassifyCpuSetStage();
    Test_E3_LabelsAreDistinct();
    Test_E4_PidReuseGuard();
    Test_E5_LiveReadbackChecksIdentity();

    std::printf("\n== F. Auto-pin readout and foreground ==\n");
    Test_F1_AutoPinnedExeNames();
    Test_F2_ResolveForegroundPid();

    std::printf("\n== G. Parked-mask warning ==\n");
    Test_G1_FullyParkedWarningNamesRunningVCacheCause();
    Test_G2_FullyParkedWarningExoneratesOptimizerWhenAgentIsNotRunning();
    Test_G3_FullyParkedWarningPointsToFirmwareWhenVCacheIsPresent();
    Test_G4_FullyParkedWarningStaysGenericWithoutVCache();

    std::printf("\n== T. Agent-aware diagnostic refactor ==\n");
    Test_T1_AgentRunningNamesTheActiveOptimizer();
    Test_T2_ServiceRunningWithoutAgentDoesNotBlameService();
    Test_T3_PresentButNotRunningPointsToFirmware();
    Test_T4_NothingPresentStaysGeneric();

    std::printf("\n== U. The Stop toggle and the restore safety net ==\n");
    Test_U1_StopBoxIsCheckedWhenTheOptimizerIsNotActive();
    Test_U2_StopBoxDependsOnNothingButTheLiveFlag();
    Test_U3_RestoreControlAppearsOnlyForUsersTheOldFeatureStranded();
    Test_U4_ParkedWarningNoLongerClaimsStoppingTheServiceIsUseless();
    Test_U5_StoppingDisablesAndClearingRestoresAmdsOwnDefault();

    std::printf("\n== V. Naming a custom mask ==\n");
    Test_V1_EmptyAndWhitespaceOnlyNamesAreEmpty();
    Test_V2_CaseInsensitiveDuplicateOfExistingMask();
    Test_V3_NameDeriveMasksEmitsIsReserved();
    Test_V4_FreshNameIsAcceptedAndTrimmed();
    Test_V5_ProfilesReferencingMaskListsEachOnceInConfigOrder();
    Test_V6_TopologyChangedPreservedSentence();
    Test_V8_CanRemoveMaskOnlyForCustomMasks();
    Test_V9_DerivableNamesAreReservedEverywhere();

    std::printf("\n== H. Live environment wording ==\n");
    Test_H1_GameModeEnvironmentWordingCoversEveryState();
    Test_H2_AmdVCacheEnvironmentWordingCoversEveryState();
    Test_H3_RunningVCacheEffectMatchesTheParkedMaskWarning();

    std::printf("\n== I. Autostart command ==\n");
    Test_I1_AutostartCommandEndsWithTrayFlag();
    Test_I2_AutostartCommandQuotesExePath();
    Test_I3_EmptyAutostartDoesNotNeedMigration();
    Test_I4_BareAutostartNeedsMigration();
    Test_I5_TrayAutostartDoesNotNeedMigration();
    Test_I6_TrayDetectionIsCaseInsensitive();

    std::printf("\n== J. Heavy-app activity ordering ==\n");
    Test_J1_MixedHeavyAppsPutRunningEntriesFirst();
    Test_J2_RunningHeavyAppsKeepRelativeOrder();
    Test_J3_InactiveHeavyAppsKeepRelativeOrder();
    Test_J4_EmptyRunningSetKeepsHeavyAppOrder();
    Test_J5_EmptyHeavyAppListStaysEmpty();
    Test_J6_FullPathMatchesOnBasename();
    Test_J7_HeavyAppMatchingIsCaseInsensitive();
    Test_J8_MismatchedItemAndKeyCountsKeepInputOrder();
    Test_J9_CaseDifferentItemUsesCallerSuppliedKey();
    Test_J10_DisplayedActivityOrderRestoresCanonicalOrder();
    Test_J11_NewHeavyAppIsAppendedAfterCanonicalEntries();
    Test_J12_NewHeavyAppsKeepDisplayedRelativeOrder();
    Test_J13_RemovedHeavyAppDoesNotReturnFromCanonicalOrder();
    Test_J14_EmptyCanonicalOrderKeepsDisplayedOrder();
    Test_J15_EmptyDisplayedOrderStaysEmpty();
    Test_J16_DuplicateCanonicalStringsUseFirstIndexWithoutDroppingEntries();

    std::printf("\n== K. Settings live-config reconciliation ==\n");
    Test_K1_LiveEntryAddedBehindTheWindowIsReported();
    Test_K2_ProfileDeletedInSettingsIsNotResurrected();
    Test_K3_LiveEntryAlreadyInWorkIsNotDuplicated();
    Test_K4_SeveralLiveAdditionsKeepAscendingIndexOrder();
    Test_K5_EmptyLiveConfigReturnsNoIndices();
    Test_K6_EmptyBaselineAndWorkReportEveryLiveIndex();
    Test_K7_KeyComparisonIsExactAndDoesNotFoldCase();

    std::printf("\n== L. AMD V-Cache driver detection ==\n");
    Test_L1_VCacheEnvironmentNamesOnlyTheService();

    std::printf("\n== M. AMD V-Cache persistent switch ==\n");
    Test_M1_MissingVCacheOriginalStartDefaultsMinusOne();
    Test_M2_VCacheOriginalStartThreeRoundTrips();
    Test_M3_DisabledWhileRunningRequiresRestart();
    Test_M4_ManualWhileRunningNeedsNoNotice();
    Test_M5_DisabledWhileStoppedNeedsNoNotice();
    Test_M6_ManualWhileStoppedRequiresRestart();
    Test_M7_VCacheRestoreHintPinsMissingManualAndBootStartValues();

    std::printf("\n== N. Startup environment warning ==\n");
    Test_N1_NoDetectedConditionDoesNotShow();
    Test_N2_MultiDomainAmdGameModeIsActionable();
    Test_N3_SingleDomainGameModeIsInformational();
    Test_N4_NonAmdGameModeIsInformational();
    Test_N5_NotDeterminableDoesNotShowGameMode();
    Test_N6_VCacheOnlyShows();
    Test_N7_BothDetectedConditionsShow();
    Test_N8_WizardVCacheLeadInIsUnchanged();
    Test_N9_StandaloneVCacheTextOmitsLeadIn();
    Test_N10_VCacheInstalledButNotRunningDoesNotShow();
    Test_N11_DriverAndServiceRunningWithoutAgentDoesNotShow();
    Test_N12_AgentRunningShowsVCache();

    std::printf("\n== O. Preserve custom masks across topology changes ==\n");
    Test_O1_NoCustomMasksReturnsDerivedExactly();
    Test_O2_OneCustomIsAppendedAfterDerived();
    Test_O3_ExactNameCollisionKeepsDerivedAndDropsCustom();
    Test_O4_CaseInsensitiveCollisionKeepsDerivedAndDropsCustom();
    Test_O5_SeveralCustomsKeepTheirRelativeOrder();
    Test_O6_StaleDerivedMaskIsDropped();
    Test_O7_EmptyExistingReturnsDerivedExactly();
    Test_O8_EmptyDerivedStillPreservesCustoms();
    Test_O9_PreservedCustomIndicatorMatchesTheMerge();

    std::printf("\n== P. Startup warning popup wording ==\n");
    Test_P1_PopupActionableTextIsExact();
    Test_P2_PopupInformationalTextIsExact();
    Test_P3_PopupSaysNeitherPageNorAttention();
    Test_P4_PopupShowsNoRegistryPath();
    Test_P5_WizardGameModeTextIsUnchanged();

    std::printf("\n== Q. Fields Settings never edits ==\n");
    Test_Q1_LiveVCacheOriginalStartSurvivesTheSettingsSnapshot();
    Test_Q2_ClearedVCacheOriginalStartAlsoBeatsTheSnapshot();

    std::printf("\n== R. AMD V-Cache agent re-checked at pin time ==\n");
    Test_R1_UnchangedAgentStateDoesNotLog();
    Test_R2_AgentAppearingLogs();
    Test_R3_AgentDisappearingLogs();
    Test_R4_FirstProbeOfARunAlwaysLogs();
    Test_R5_TheSecondProbeOfAnUnchangedRunIsSilent();

    std::printf("\n== S. AMD V-Cache body text names the agent ==\n");
    Test_S1_AgentRunningShowsActive();
    Test_S2_AgentNotRunningServiceStoppedShowsNotActive();
    Test_S3_ServiceRunningButAgentNotRunningShowsNotActive();
    Test_S4_ServiceStoppedButAgentRunningShowsActive();
    Test_S5_NotInstalledAlwaysShowsNotInstalled();

    std::printf("\n");
    std::printf("TOTAL %d PASSED %d FAILED %d\n", g_total, g_total - g_failed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
