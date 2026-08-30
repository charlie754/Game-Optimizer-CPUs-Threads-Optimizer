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

#include "applier.h"
#include "config.h"
#include "engine.h"
#include "procwatch.h"
#include "settings_environment.h"
#include "settings_warning.h"
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

void Test_G1_FullyParkedWarningWithoutVCacheServiceIsUnchanged() {
    Case("G1 fully parked warning is unchanged without the AMD service running");
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, false),
        L"Warning: all 16 processors in \"Freq\" are currently parked. Windows can accept "
        L"an assignment to a fully parked mask and then ignore it - the process keeps "
        L"running elsewhere.");
}

void Test_G2_FullyParkedWarningNamesLikelyVCacheCause() {
    Case("G2 fully parked warning names the likely AMD V-Cache cause");
    CHECK_EQ(
        cd::FormatFullyParkedMaskWarning(L"Freq", 16, true),
        L"Warning: all 16 processors in \"Freq\" are currently parked. The running AMD 3D "
        L"V-Cache Performance Optimizer service is the likely cause. It normally parks the "
        L"non-cache CCD while a game is running. While that CCD is parked, a background mask "
        L"pointing at it is effectively inert. Windows can accept assignments to that CCD "
        L"and then ignore them.");
}

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
    CHECK(cd::FormatFullyParkedMaskWarning(L"Freq", 16, true).find(effect) !=
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
    Test_G1_FullyParkedWarningWithoutVCacheServiceIsUnchanged();
    Test_G2_FullyParkedWarningNamesLikelyVCacheCause();

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

    std::printf("\n");
    std::printf("TOTAL %d PASSED %d FAILED %d\n", g_total, g_total - g_failed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
