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
#include "apply_rules.h"
#include "config.h"
#include "gpu_cuda.h"
#include "gpu_edit.h"
#include "gpu_pref.h"
#include "gpu_policy.h"
#include "gpu_rows.h"
#include "engine.h"
#include "mask_merge.h"
#include "mask_edit.h"
#include "procwatch.h"
#include "settings_environment.h"
#include "settings_heavy_order.h"
#include "settings_merge.h"
#include "settings_pages.h"
#include "settings_warning.h"
#include "envwarning_text.h"
#include "firstrun_text.h"
#include "irq_policy.h"
#include "irq_probe.h"
#include "startup_warning.h"
#include "topology.h"
#include "util.h"
#include "version_label.h"

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

// The interrupt readout's four enums. THESE ARE REQUIRED, NOT OPTIONAL: there is no generic
// fallback in this harness, so CHECK_EQ on a type with no Show() overload is a COMPILE error
// rather than an ugly diagnostic.
std::string Show(cd::IrqRefusal r) {
    switch (r) {
        case cd::IrqRefusal::None: return "IrqRefusal::None";
        case cd::IrqRefusal::MultipleProcessorGroups: return "IrqRefusal::MultipleProcessorGroups";
        case cd::IrqRefusal::TooManyProcessors: return "IrqRefusal::TooManyProcessors";
        case cd::IrqRefusal::UnexpectedProcessorNumbering:
            return "IrqRefusal::UnexpectedProcessorNumbering";
        case cd::IrqRefusal::CountersUnavailable: return "IrqRefusal::CountersUnavailable";
    }
    return "IrqRefusal::<bad>";
}

std::string Show(cd::IrqPolicyState s) {
    switch (s) {
        case cd::IrqPolicyState::NotConfigured: return "IrqPolicyState::NotConfigured";
        case cd::IrqPolicyState::Configured: return "IrqPolicyState::Configured";
        case cd::IrqPolicyState::Unreadable: return "IrqPolicyState::Unreadable";
    }
    return "IrqPolicyState::<bad>";
}

std::string Show(cd::IrqAgreement a) {
    switch (a) {
        case cd::IrqAgreement::Unreadable: return "IrqAgreement::Unreadable";
        case cd::IrqAgreement::NeitherPresent: return "IrqAgreement::NeitherPresent";
        case cd::IrqAgreement::PolicyOnly: return "IrqAgreement::PolicyOnly";
        case cd::IrqAgreement::TargetOnly: return "IrqAgreement::TargetOnly";
        case cd::IrqAgreement::Agree: return "IrqAgreement::Agree";
        case cd::IrqAgreement::Disagree: return "IrqAgreement::Disagree";
    }
    return "IrqAgreement::<bad>";
}

std::string Show(cd::GameGroupSource g) {
    switch (g) {
        case cd::GameGroupSource::LiveProfile: return "GameGroupSource::LiveProfile";
        case cd::GameGroupSource::MachineDefault: return "GameGroupSource::MachineDefault";
        case cd::GameGroupSource::Unknown: return "GameGroupSource::Unknown";
    }
    return "GameGroupSource::<bad>";
}

// Rule 1's reason code. REQUIRED for CHECK_EQ on a SelectReason, and a new enumerator
// without a case here is a -W3 warning rather than a silent "<bad>" in a failure message.
std::string Show(cd::SelectReason r) {
    switch (r) {
        case cd::SelectReason::Unchanged: return "SelectReason::Unchanged";
        case cd::SelectReason::First: return "SelectReason::First";
        case cd::SelectReason::Foreground: return "SelectReason::Foreground";
        case cd::SelectReason::Released: return "SelectReason::Released";
        case cd::SelectReason::Cleared: return "SelectReason::Cleared";
    }
    return "SelectReason::<bad>";
}

// The applier's outcome enum and the file reader's three-valued answer. REQUIRED, not
// optional - see the note above IrqRefusal: CHECK_EQ on a type with no Show() overload is a
// compile error in this harness rather than an ugly diagnostic.
std::string Show(cd::ApplyResult r) {
    switch (r) {
        case cd::ApplyResult::Ok: return "ApplyResult::Ok";
        case cd::ApplyResult::AccessDenied: return "ApplyResult::AccessDenied";
        case cd::ApplyResult::Gone: return "ApplyResult::Gone";
        case cd::ApplyResult::InvalidParameter: return "ApplyResult::InvalidParameter";
        case cd::ApplyResult::OtherError: return "ApplyResult::OtherError";
    }
    return "ApplyResult::<bad>";
}

std::string Show(cd::FileReadResult r) {
    switch (r) {
        case cd::FileReadResult::Ok: return "FileReadResult::Ok";
        case cd::FileReadResult::Missing: return "FileReadResult::Missing";
        case cd::FileReadResult::Unreadable: return "FileReadResult::Unreadable";
    }
    return "FileReadResult::<bad>";
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
    p2.lastUsed = 0;         // both a set and an unset stamp are exercised

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

void Test_B10_LastUsedRoundTripsAndTheRetiredKeyIsNeverWritten() {
    Case("B10 last_used round-trips, and the retired all_games key is never serialized");
    cd::Config c = MakeRoundTripConfig();
    std::wstring text = cd::SerializeConfig(c);

    // last_used IS emitted (a parser that defaulted it would otherwise pass).
    CHECK(text.find(L"last_used=") != std::wstring::npos);
    CHECK(text.find(L"133700000000000000") != std::wstring::npos);

    // 🔴 AND all_games IS NOT, WHICHEVER WAY THE FLAG IS SET. This is the half of the v0.5.4
    // retirement that a user can see: the key has to leave their file on the next save and
    // never come back. A build that re-emitted it would be writing a setting it refuses to
    // honour, which is the same defect auto_pin_seconds is asserted against above.
    CHECK(text.find(L"all_games") == std::wstring::npos);
    {
        cd::Config withFlag = MakeRoundTripConfig();
        withFlag.profiles[1].legacyAllGames = true;
        CHECK(cd::SerializeConfig(withFlag).find(L"all_games") == std::wstring::npos);
    }

    cd::Config out;
    std::wstring err;
    bool ok = cd::ParseConfig(text, out, &err);
    CHECK(ok);
    if (!ok) std::printf("       parse error: %s\n", Utf8(err).c_str());

    CHECK_EQ((int)out.profiles.size(), 2);
    if (out.profiles.size() == 2) {
        CHECK_EQ(out.profiles[0].lastUsed, kStampMarker);
        CHECK_EQ(out.profiles[1].lastUsed, 0ull);
        // Nothing this build writes can set the migration shim.
        CHECK_EQ(out.profiles[0].legacyAllGames, false);
        CHECK_EQ(out.profiles[1].legacyAllGames, false);
    }

    // An OLDER config - no last_used anywhere - still loads, with the field at its documented
    // default rather than as a preserved unknown key.
    std::vector<std::wstring> lines = SplitLines(ToLf(text));
    std::vector<std::wstring> stripped;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find(L"last_used=") != std::wstring::npos) continue;
        stripped.push_back(lines[i]);
    }
    CHECK((int)stripped.size() < (int)lines.size());

    cd::Config old;
    CHECK(cd::ParseConfig(JoinLines(stripped), old, &err));
    CHECK_EQ((int)old.profiles.size(), 2);
    if (old.profiles.size() == 2) {
        CHECK_EQ(old.profiles[0].lastUsed, 0ull);
        CHECK_EQ(old.profiles[1].lastUsed, 0ull);
    }
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
    // Exactly ONE profile ships: "Overwatch", enabled with autoPin ON.
    // No All Games profile ships. The All Games machinery is retained for configs that already
    // have one (see ValidateAndRepair), but new defaults start with a single worked example.
    Case("B8 DefaultConfig(referenceTopology)");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);

    CHECK_EQ((int)c.profiles.size(), 1);
    if (c.profiles.size() == 1) {
        CHECK_EQ(c.profiles[0].name, L"Overwatch");
        CHECK_EQ(c.profiles[0].enabled, true);
        CHECK_EQ(c.profiles[0].autoPin, true);
        CHECK_EQ(c.profiles[0].game, L"Overwatch.exe");
        CHECK_EQ(c.profiles[0].lastUsed, 0ull);
        CHECK_EQ(c.profiles[0].autoPinPercent, 3);
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

void Test_B8b_TheStructDefaultIsTheOneUsersInherit() {
    Case("B8b Profile::autoPinPercent defaults to 3, which every NEW profile inherits");
    // ASSERTED SEPARATELY FROM THE SHIPPED PROFILE, and that is the whole point of this case.
    // "Add profile..." in settings.cpp and CreateProfileForGame in main.cpp both leave this
    // field at the struct default ON PURPOSE, with comments saying so, so the number a user
    // actually gets comes from HERE and not from DefaultConfig. A change that moved only
    // config.cpp would leave every user-created profile on the old threshold.
    CHECK_EQ(cd::Profile().autoPinPercent, 3);
}

void Test_B8c_TheShippedProfileCarriesTheSameThreshold() {
    Case("B8c the shipped Overwatch profile also carries auto-pin 3%");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    CHECK(!c.profiles.empty());
    if (!c.profiles.empty()) CHECK_EQ(c.profiles[0].autoPinPercent, 3);
}

void Test_B8d_TheNewDefaultIsSelfConsistent() {
    Case("B8d the shipped default needs no repair on its first load");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);

    // Nothing shipped carries the retired migration flag.
    for (size_t i = 0; i < c.profiles.size(); ++i) CHECK_EQ(c.profiles[i].legacyAllGames, false);

    // AND IT STILL LOADS CLEAN. Dropping a profile out of the default is exactly the kind of
    // edit that leaves the remainder invalid - a dangling mask name, a count something else
    // relies on - and ValidateAndRepair is what would notice. An empty list is the claim.
    std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);
    CHECK(rep.empty());
    if (!rep.empty()) {
        for (size_t i = 0; i < rep.size(); ++i)
            std::printf("       unexpected repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
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
        // Add a second ordinary profile to test that autoPinSeconds is forced on EVERY profile
        cd::Profile p2;
        p2.name = L"TestProfile";
        p2.gameMask = t.defaultGameMask;
        p2.heavyMask = t.defaultHeavyMask;
        c.profiles.push_back(p2);

        // Values on both sides of the fixed debounce, and one the old clamp would have
        // accepted silently, so "forced" cannot be confused with "clamped to a range".
        c.profiles[0].autoPinSeconds = 900;
        c.profiles[1].autoPinSeconds = 5;
        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);

        CHECK_EQ((int)c.profiles.size(), 2);
        for (size_t i = 0; i < c.profiles.size(); ++i)
            CHECK_EQ(c.profiles[i].autoPinSeconds, cd::kAutoPinDebounceTicks);

        // It is not the user's setting any more, so it is not reported as a repair. The
        // whole list must still be empty here: the new config is otherwise valid.
        CHECK(rep.empty());
        CHECK(!AnyContains(rep, L"auto-pin hold"));
        CHECK(!AnyContains(rep, L"seconds"));
        if (!rep.empty()) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       unexpected repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
        }
    }

    Case("B12b a profile carrying the RETIRED all_games flag is migrated, and that IS reported");
    {
        // THE ONE MIGRATION v0.5.4 OWES ITS USERS. "All Games" is gone; a config written by an
        // earlier version still carries all_games=1, and ParseConfig puts it in the shim.
        //
        // 🔴 THE `game` FIELD IS THE POINT. The old rule cleared it on every load, so a
        // hand-edited config can carry a real executable there that has been dead for the
        // profile's whole life. Migrating without clearing it would bring that value LIVE and
        // hand the profile an executable the user never chose for it.
        cd::Config c = cd::DefaultConfig(t);
        cd::Profile legacy;
        legacy.name = L"All Games";
        legacy.legacyAllGames = true;
        legacy.game = L"Palworld.exe";
        legacy.gameMask = t.defaultGameMask;
        legacy.heavyMask = t.defaultHeavyMask;
        c.profiles.push_back(legacy);

        std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);

        // KEPT, NOT DELETED - removing a row from the user's file is destroying their data.
        CHECK_EQ((int)c.profiles.size(), 2);
        if (c.profiles.size() == 2) {
            CHECK_EQ(c.profiles[1].name, L"All Games");
            CHECK_EQ(c.profiles[1].legacyAllGames, false);   // the flag is consumed
            CHECK_EQ(c.profiles[1].game, L"");               // and the stale game with it
            CHECK_EQ(c.profiles[0].game, L"Overwatch.exe");  // the ordinary one is untouched
        }
        // REPORTED, naming the profile and the game it lost. The operator chose log-only
        // delivery, so this note is the whole user-visible record of the migration.
        CHECK(!rep.empty());
        CHECK(AnyContains(rep, L"All Games"));
        CHECK(AnyContains(rep, L"Palworld.exe"));
        if (!AnyContains(rep, L"Palworld.exe")) {
            for (size_t i = 0; i < rep.size(); ++i)
                std::printf("       repair[%d]: %s\n", (int)i, Utf8(rep[i]).c_str());
        }
    }

    Case("B12c migrating the retired flag is idempotent and silent the second time");
    {
        // A CONTROL. The note must fire ONCE, on the load that finds the key - not on every
        // launch forever. Without this, a rule that reported unconditionally would pass B12b.
        cd::Config c = cd::DefaultConfig(t);
        cd::Profile legacy;
        legacy.name = L"All Games";
        legacy.legacyAllGames = true;
        legacy.gameMask = t.defaultGameMask;
        legacy.heavyMask = t.defaultHeavyMask;
        c.profiles.push_back(legacy);

        std::vector<std::wstring> first = cd::ValidateAndRepair(c, t);
        CHECK(AnyContains(first, L"All Games"));

        std::vector<std::wstring> second = cd::ValidateAndRepair(c, t);
        CHECK(second.empty());
        if (!second.empty()) {
            for (size_t i = 0; i < second.size(); ++i)
                std::printf("       unexpected repeat[%d]: %s\n", (int)i, Utf8(second[i]).c_str());
        }
    }
}

void Test_B12d_TheRetiredFlagCannotSurviveASaveThatSkipsTheRepair() {
    // 🔴 THE REGRESSION v0.5.4 ALMOST SHIPPED, found by adversarial review and fixed in
    // ParseConfig rather than in the caller.
    //
    // RunVCacheSet (main.cpp) loads the config, edits one field and SAVES IT - with no
    // ValidateAndRepair anywhere on that path. It dispatches before startup and returns.
    // Since v0.5.4 the serializer no longer writes `all_games`, so that round trip would have
    // DROPPED THE FLAG AND KEPT THE EXECUTABLE, and the next launch would have handed the
    // profile a game the user never chose for it. The old code was safe only by accident: it
    // wrote the key back out, so the pairing survived until a real repair ran.
    //
    // This asserts the fix WITHOUT calling ValidateAndRepair even once, because the whole
    // point is the path that never calls it.
    Case("B12d a retired all_games profile cannot carry its stale game through a repair-less save");

    const std::wstring legacy =
        L"[general]\r\nversion=1\r\n\r\n"
        L"[masks]\r\nCache=0 1 2 3\r\n\r\n"
        L"[profile:All Games]\r\n"
        L"enabled=1\r\n"
        L"game=Palworld.exe\r\n"
        L"game_mask=Cache\r\n"
        L"all_games=1\r\n";

    cd::Config c;
    std::wstring err;
    CHECK(cd::ParseConfig(legacy, c, &err));
    CHECK_EQ(err, L"");
    CHECK_EQ((int)c.profiles.size(), 1);
    if (!c.profiles.empty()) {
        // The flag is seen...
        CHECK_EQ(c.profiles[0].legacyAllGames, true);
        // ...and the stale executable is ALREADY gone, before any repair runs.
        CHECK_EQ(c.profiles[0].game, L"");
    }

    // EXACTLY WHAT THE HELPER DOES: serialize without repairing.
    const std::wstring saved = cd::SerializeConfig(c);
    CHECK(saved.find(L"all_games") == std::wstring::npos);
    CHECK(saved.find(L"Palworld.exe") == std::wstring::npos);

    cd::Config back;
    CHECK(cd::ParseConfig(saved, back, &err));
    CHECK_EQ((int)back.profiles.size(), 1);
    if (!back.profiles.empty()) {
        CHECK_EQ(back.profiles[0].legacyAllGames, false);
        CHECK_EQ(back.profiles[0].game, L"");   // nothing for the engine to match
    }

    Case("B12d2 and the clear does not depend on the order of keys in the file");
    {
        // A hand-edited section can put all_games ABOVE game. config.ini is documented as
        // hand-editable, so this is reachable - and a clear done inside the parse branch
        // instead of in a post-pass would be undone by the very next line.
        const std::wstring reversed =
            L"[general]\r\nversion=1\r\n\r\n"
            L"[masks]\r\nCache=0 1 2 3\r\n\r\n"
            L"[profile:All Games]\r\n"
            L"enabled=1\r\n"
            L"all_games=1\r\n"
            L"game=Palworld.exe\r\n"
            L"game_mask=Cache\r\n";

        cd::Config r;
        CHECK(cd::ParseConfig(reversed, r, &err));
        CHECK_EQ((int)r.profiles.size(), 1);
        if (!r.profiles.empty()) {
            CHECK_EQ(r.profiles[0].legacyAllGames, true);
            CHECK_EQ(r.profiles[0].game, L"");
        }
    }

    Case("B12d3 CONTROL - an ordinary profile with no retired flag keeps its game");
    {
        // Without this, everything above would pass on a parser that cleared `game` always.
        const std::wstring ordinary =
            L"[general]\r\nversion=1\r\n\r\n"
            L"[masks]\r\nCache=0 1 2 3\r\n\r\n"
            L"[profile:Palworld]\r\n"
            L"enabled=1\r\n"
            L"game=Palworld.exe\r\n"
            L"game_mask=Cache\r\n";

        cd::Config o;
        CHECK(cd::ParseConfig(ordinary, o, &err));
        CHECK_EQ((int)o.profiles.size(), 1);
        if (!o.profiles.empty()) {
            CHECK_EQ(o.profiles[0].legacyAllGames, false);
            CHECK_EQ(o.profiles[0].game, L"Palworld.exe");
        }
    }
}

void Test_B13_MarkProfileUsed() {
    Case("B13 MarkProfileUsed stamps the named profile and is a no-op for an unknown name");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    // A SECOND PROFILE, CONSTRUCTED HERE RATHER THAN BORROWED. This case proves "only the
    // named one moved", which needs a second profile to be silent about. The default used
    // to ship one (All Games) and no longer does, so the control is built. Its masks come
    // from the same topology, so the config stays valid.
    {
        cd::Profile second;
        second.name = L"Second";
        second.gameMask = t.defaultGameMask;
        second.heavyMask = t.defaultHeavyMask;
        c.profiles.push_back(second);
    }
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

void Test_U1_StopBoxIsCheckedWhenTheServiceIsDisabled() {
    Case("U1 the Stop box is checked exactly when the SERVICE is configured Disabled");
    // "Checked" means stopped, and stopped is a SERVICE START TYPE, not a running process.
    // Getting this backwards would show every user a ticked box on a machine where AMD's
    // optimizer is configured normally.
    CHECK(cd::VCacheStopBoxChecked(4));
    CHECK(!cd::VCacheStopBoxChecked(2));
}

void Test_U2_StopBoxDependsOnNothingButTheServiceStartType() {
    Case("U2 the Stop box depends on the service start type alone, never on config");
    // This control has NO persisted value. It is a live mirror of the machine's service
    // configuration, so its helper takes exactly one argument and there is nowhere for a config
    // value to enter. A stored value would survive the user changing the service outside our UI,
    // and the box would then assert a state the machine is not in.
    CHECK(cd::VCacheStopBoxChecked(4) == true);
    CHECK(cd::VCacheStopBoxChecked(2) == false);
}

void Test_X1_ServiceDisabledIsChecked() {
    Case("X1 SERVICE_DISABLED (4) -> box checked");
    CHECK(cd::VCacheStopBoxChecked(4) == true);
}

void Test_X2_ServiceAutomaticIsUnchecked() {
    Case("X2 SERVICE_AUTO (2) -> box unchecked");
    CHECK(cd::VCacheStopBoxChecked(2) == false);
}

void Test_X3_ServiceManualIsUnchecked() {
    Case("X3 SERVICE_MANUAL (3) -> box unchecked");
    CHECK(cd::VCacheStopBoxChecked(3) == false);
}

void Test_X4_UnreadableServiceIsUnchecked() {
    Case("X4 unreadable or missing service (-1) -> box unchecked, the SAFE direction");
    // It never claims a stop that was not configured. On a machine with no AMD optimizer
    // at all, the key cannot be read and we must not show a checked box.
    CHECK(cd::VCacheStopBoxChecked(-1) == false);
}

void Test_X5_RoundTripProperty() {
    Case("X5 round-trip: what the click writes and the box shows must agree");
    // The click writes via VCacheServiceStartTypeFor, the box reads via VCacheStopBoxChecked.
    CHECK(cd::VCacheStopBoxChecked(cd::VCacheServiceStartTypeFor(true))  == true);
    CHECK(cd::VCacheStopBoxChecked(cd::VCacheServiceStartTypeFor(false)) == false);
}

void Test_Y1_AutostartExeFromQuotedCommandWithTray() {
    Case("Y1 a quoted command with --tray yields just the executable path");
    CHECK(cd::AutostartExeFromCommand(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\" --tray") ==
        L"C:\\Game Optimizer\\GameOptimizer.exe");
}

void Test_Y2_AutostartExeFromBareCommandWithTray() {
    Case("Y2 a bare command with --tray yields just the executable path");
    CHECK(cd::AutostartExeFromCommand(L"C:\\Apps\\GameOptimizer.exe --tray") ==
        L"C:\\Apps\\GameOptimizer.exe");
}

void Test_Y3_AutostartExeEmptyAndUnclosedQuote() {
    Case("Y3 empty commands and unclosed quotes yield an empty executable path");
    CHECK(cd::AutostartExeFromCommand(L"") == L"");
    CHECK(cd::AutostartExeFromCommand(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe --tray") == L"");
}

void Test_Y4_SameExeDifferentCaseWithTray() {
    Case("Y4 the same executable with different letter case and --tray needs no migration");
    CHECK(cd::AutostartNeedsMigration(
        L"\"C:\\GAME OPTIMIZER\\GAMEOPTIMIZER.EXE\" --tray",
        L"c:\\game optimizer\\gameoptimizer.exe") == false);
}

void Test_Y5_DifferentExePathsWithTray() {
    Case("Y5 a stale executable path needs migration even with --tray");
    CHECK(cd::AutostartNeedsMigration(
        L"\"C:\\Old Copy\\GameOptimizer.exe\" --tray",
        L"C:\\Game Optimizer\\GameOptimizer.exe") == true);
}

void Test_Y6_CurrentExeEmptyDifferentPaths() {
    Case("Y6 an unreadable current executable path must not trigger migration");
    CHECK(cd::AutostartNeedsMigration(
        L"\"C:\\Old Copy\\GameOptimizer.exe\" --tray", L"") == false);
}

void Test_Y7_FlaglessCommandSamePathsReturnsTrue() {
    Case("Y7 a matching executable path still needs migration without --tray");
    CHECK(cd::AutostartNeedsMigration(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\"",
        L"C:\\Game Optimizer\\GameOptimizer.exe") == true);
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
// V. V-Cache restore logic helpers.
// ===========================================================================

void Test_V1_RestoreDriverStartWithRecordedThree() {
    Case("V1 VCacheRestoreDriverStart(3) == 3");
    CHECK_EQ(cd::VCacheRestoreDriverStart(3), 3);
}

void Test_V2_RestoreDriverStartWithRecordedTwo() {
    Case("V2 VCacheRestoreDriverStart(2) == 2");
    CHECK_EQ(cd::VCacheRestoreDriverStart(2), 2);
}

void Test_V3_RestoreDriverStartWithNegativeOne() {
    Case("V3 VCacheRestoreDriverStart(-1) == 3");
    CHECK_EQ(cd::VCacheRestoreDriverStart(-1), 3);
}

void Test_V4_ServiceRestoreTypeIsAlwaysTwo() {
    Case("V4 VCacheServiceStartTypeFor(false) == 2 — independent of recorded driver original");
    // This is the actual bug: the service's restore value is INDEPENDENT of the driver's
    // original recorded value. AMD's INF always installs the service as SERVICE_AUTO_START (2),
    // so restore always means 2. The fix ensures driver and service are written separately.
    CHECK_EQ(cd::VCacheServiceStartTypeFor(false), 2);
}

void Test_V5_ServiceDisableTypeIsAlwaysFour() {
    Case("V5 VCacheServiceStartTypeFor(true) == 4 — the disable direction is unchanged");
    // On disable, both driver and service go to Disabled (4). This direction is correct and unchanged.
    CHECK_EQ(cd::VCacheServiceStartTypeFor(true), 4);
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
    CHECK(!cd::AutostartNeedsMigration(L"", L"C:\\Game Optimizer\\GameOptimizer.exe"));
}

void Test_I4_BareAutostartNeedsMigration() {
    Case("I4 a quoted bare executable path needs migration");
    CHECK(cd::AutostartNeedsMigration(L"\"C:\\Game Optimizer\\GameOptimizer.exe\"", L"C:\\Game Optimizer\\GameOptimizer.exe"));
}

void Test_I5_TrayAutostartDoesNotNeedMigration() {
    Case("I5 an autostart command with --tray does not need migration");
    CHECK(!cd::AutostartNeedsMigration(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\" --tray", L"C:\\Game Optimizer\\GameOptimizer.exe"));
}

void Test_I6_TrayDetectionIsCaseInsensitive() {
    Case("I6 an uppercase --TRAY flag does not need migration");
    CHECK(!cd::AutostartNeedsMigration(
        L"\"C:\\Game Optimizer\\GameOptimizer.exe\" --TRAY", L"C:\\Game Optimizer\\GameOptimizer.exe"));
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
    CHECK(!decision.Any());
}

void Test_N2_MultiDomainAmdGameModeIsActionable() {
    Case("N2 Game Mode On on multi-domain AMD is actionable");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd(), true);
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Actionable);
}

void Test_N3_SingleDomainGameModeIsInformational() {
    Case("N3 Game Mode On on one domain is informational");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Informational);
}

void Test_N4_NonAmdGameModeIsInformational() {
    Case("N4 Game Mode On on non-AMD multi-domain CPU is informational");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = false;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd(), true);
    CHECK(decision.showGameMode);
    CHECK(decision.gameModeTone == cd::WarningTone::Informational);
}

void Test_N5_NotDeterminableDoesNotShowGameMode() {
    Case("N5 Game Mode NotDeterminable does not show Game Mode");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::NotDeterminable;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd(), true);
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
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
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
    CHECK(decision.showVCache);
    CHECK(decision.Any());
}

// ===========================================================================
// T. Per-warning startup preference (separate from the earlier agent-diagnostic T block).
//
// Suppression belongs to the V-Cache section alone. In particular, Game Mode must still
// open the warning when the optimizer is active but the user has hidden its section.
// Config round-trips pin both the opt-out and the upgrade default: an older config must
// keep showing the warning until its user explicitly asks otherwise.
// ===========================================================================
void Test_T1_AgentRunningWithWarningEnabledShowsVCache() {
    Case("T1 agent running with startup warning enabled shows V-Cache");
    cd::EnvironmentInfo env;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
    CHECK(decision.showVCache);
}

void Test_T2_AgentRunningWithWarningSuppressedHidesVCache() {
    Case("T2 agent running with startup warning suppressed hides V-Cache");
    cd::EnvironmentInfo env;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain(), false);
    CHECK(!decision.showVCache);
}

void Test_T3_AgentNotRunningWithWarningEnabledHidesVCache() {
    Case("T3 agent not running with startup warning enabled hides V-Cache");
    cd::EnvironmentInfo env;
    env.amdVCacheAgentRunning = false;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain(), true);
    CHECK(!decision.showVCache);
}

void Test_T4_VCacheSuppressionDoesNotSuppressGameMode() {
    Case("T4 V-Cache suppression leaves Game Mode On visible and opens the warning");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::On;
    env.isAmd = true;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSymmetricDualCcd(), false);
    CHECK(decision.showGameMode);
    CHECK(decision.Any());
    CHECK(!decision.showVCache);
}

void Test_T5_VCacheSuppressedAndGameModeOffDoesNotShow() {
    Case("T5 agent running but suppressed with Game Mode Off does not open the warning");
    cd::EnvironmentInfo env;
    env.gameModeState = cd::GameModeState::Off;
    env.amdVCacheAgentRunning = true;
    const cd::StartupWarningDecision decision =
        cd::DecideStartupWarning(env, MakeSingleDomain(), false);
    CHECK(!decision.Any());
}

void Test_T6_VCacheWarningSuppressionRoundTrips() {
    Case("T6 V-Cache startup warning opt-out is written and survives parsing");
    cd::Config c;
    c.showVCacheWarning = false;
    const std::wstring serialized = cd::SerializeConfig(c);
    CHECK(serialized.find(L"show_vcache_warning") != std::wstring::npos);
    cd::Config roundTripped;
    std::wstring err;
    CHECK(cd::ParseConfig(serialized, roundTripped, &err));
    CHECK(!roundTripped.showVCacheWarning);
}

void Test_T7_MissingVCacheWarningPreferenceDefaultsTrue() {
    Case("T7 an older config without the V-Cache warning key keeps the warning enabled");
    cd::Config c;
    // Parse into a previously suppressed config too: the missing key must use the member
    // default, not inherit whatever happened to be in the destination before loading.
    c.showVCacheWarning = false;
    std::wstring err;
    CHECK(cd::ParseConfig(L"[general]\nfirst_run_done=1\n", c, &err));
    CHECK(c.showVCacheWarning);
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

void Test_V10_AddMaskCaptionIsNotAValidMaskName() {
    Case("V10 the \"Add mask...\" combo caption is reserved: exact, any case, and trimmed");
    // The Profiles page now shows cd::kAddMaskEntryCaption as a ROW inside each of its two mask
    // combos, so a mask carrying that same name would sit in the same list drawn identically to
    // the action beside it. The ROW is told apart by its combo item data, not by its text
    // (src\settings.cpp, kMaskItemAdd), so this reservation is the second belt rather than the
    // first - but without it the ordinary Add flow, the one the user actually drives, would put
    // such a mask into the config in the first place.
    //
    // Empty existing list and empty derived list, exactly as V9: the only thing that can refuse
    // any name below is the machine-independent reservation, so each result proves that check
    // alone rather than a Duplicate or a live derived name.
    const std::vector<cd::Mask> none;
    const auto v = [&none](const wchar_t* raw) { return cd::ValidateNewMaskName(raw, none, none); };
    const cd::MaskNameProblem reserved = cd::MaskNameProblem::ReservedDerivedName;
    const cd::MaskNameProblem ok = cd::MaskNameProblem::None;

    // THE CONSTANT ITSELF, not a copy of its text. A literal-only test keeps passing after
    // someone rewords the row, which is the exact drift the shared constant exists to prevent -
    // so the caption is asserted both ways, and the pair fails the day they stop agreeing.
    CHECK(v(cd::kAddMaskEntryCaption) == reserved);
    CHECK(v(L"Add mask...") == reserved);
    // Case-insensitively: FindMask, the merge and every other name comparison in this program
    // use IEquals, so "add mask..." and "Add mask..." are one name to all of them.
    CHECK(v(L"add mask...") == reserved);
    CHECK(v(L"ADD MASK...") == reserved);
    CHECK(v(L"AdD mAsK...") == reserved);
    // Trimmed first, like every other name. This is not cosmetic: TrimMaskName is what the
    // caller STORES, so "  Add mask...  " would not merely resemble the caption, it would
    // become it.
    CHECK(v(L"  Add mask...") == reserved);
    CHECK(v(L"Add mask...   ") == reserved);
    CHECK(v(L"\t Add mask... \r\n") == reserved);
    // NEGATIVE CONTROLS. Without these the eight above are satisfied by a validator that
    // refuses everything, which would be a far worse bug than the one they guard.
    CHECK(v(L"Streaming") == ok);
    CHECK(v(L"Add mask") == ok);      // no ellipsis
    CHECK(v(L"Add mask..") == ok);    // two dots, not three
    CHECK(v(L"Add masks...") == ok);
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
    // v0.5.6: the tab it sends the user to is labelled "CPU Core Map" now, so the sentence names it that way.
    // Read from the tab labels as well, so renaming the tab without these sentences fails here.
    CHECK(one.find(cd::kSettingsPageLabels[1]) != std::wstring::npos &&
          three.find(cd::kSettingsPageLabels[1]) != std::wstring::npos);
    CHECK_EQ(one, std::wstring(L"1 custom mask you created was kept, but the processor "
                               L"numbers inside it may now refer to different cores - open "
                               L"the CPU Core Map and check it."));
    CHECK_EQ(three, std::wstring(L"3 custom masks you created were kept, but the processor "
                                 L"numbers inside them may now refer to different cores - "
                                 L"open the CPU Core Map and check each one."));
}

// ===========================================================================
// W / X / Y.  The interrupt and DPC readout (src\irq_policy.h).
//
// Every one of these is PURE. The readout's OS half - PDH, cfgmgr32, the registry reads -
// lives in irq_probe.cpp, which no test links; what is testable was deliberately put in the
// header so this harness reaches it with no change to tools\build-tests.bat.
//
// NOTE ON THE LETTERS: the banners in main() had W, X and Y free, but the FUNCTION prefixes
// Test_X1..X5 and Test_Y1..Y7 were already taken by the V-Cache and autostart groups. Every
// name below therefore carries an Irq infix, so the group letters can match the banners
// without colliding with an existing identifier.
// ===========================================================================

// ---- Synthetic machines this readout has to refuse ------------------------

cd::Topology MakeTwoProcessorGroups() {
    cd::Topology t = MakeReference(false);
    // FinishTopology forces groupCount to 1, so this is set AFTERWARDS on purpose.
    t.groupCount = 2;
    return t;
}

cd::Topology MakeGroupCountOneButAnEntryInGroupOne() {
    cd::Topology t = MakeReference(false);
    t.groupCount = 1;
    t.entries[17].Group = 1;
    return t;
}

cd::Topology MakeNinetySixLogicalProcessors() {
    cd::Topology t;
    for (ULONG lp = 0; lp < 96; ++lp) {
        const ULONG llc = (lp < 48) ? 0u : 48u;
        t.entries.push_back(
            Entry(lp, (lp / 2u) * 2u, llc, (BYTE)0, llc == 0u ? 98304ull : 32768ull));
    }
    FinishTopology(t);
    return t;
}

cd::Topology MakeNumberingThatStartsAtSixtyFour() {
    cd::Topology t;
    for (ULONG lp = 64; lp < 96; ++lp) {
        const ULONG llc = (lp < 80) ? 0u : 16u;
        t.entries.push_back(Entry(lp, ((lp - 64) / 2u) * 2u, llc, (BYTE)0,
                                  llc == 0u ? 98304ull : 32768ull));
    }
    FinishTopology(t);
    return t;
}

cd::CoreDpcStat IrqStat(ULONG lp, int samples, double dpc, double isr, bool inGroup) {
    cd::CoreDpcStat c;
    c.lp = lp;
    c.samples = samples;
    c.meanDpcPct = dpc;
    c.minDpcPct = dpc;
    c.maxDpcPct = dpc;
    c.meanIsrPct = isr;
    c.minIsrPct = isr;
    c.maxIsrPct = isr;
    c.inGameGroup = inGroup;
    return c;
}

void AllPairwiseDistinct(const std::vector<std::wstring>& v, const char* what) {
    for (size_t i = 0; i < v.size(); ++i) {
        ++g_total;
        if (v[i].empty())
            Fail(__FILE__, __LINE__, what, "an empty sentence", "a non-empty sentence");
        for (size_t j = i + 1; j < v.size(); ++j) {
            ++g_total;
            if (v[i] == v[j])
                Fail(__FILE__, __LINE__, what, Utf8(v[i]),
                     "a sentence used by only one value");
        }
    }
}

// ---------------------------------------------------------------------------
// W. Mask arithmetic and machine scope.
// ---------------------------------------------------------------------------

void Test_W1_IrqGoldenVector() {
    Case("W1 golden vector - the measured bytes decode to the measured processors");
    // [M] These are the exact bytes read back off the reference machine's RTX 4090 on
    // 2026-09-08: AssignmentSetOverride = 00 00 ff ff 00 00 00 00.
    const BYTE golden[8] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    ULONG_PTR mask = 0;
    CHECK(cd::IrqRegBytesToMask(golden, sizeof(golden), mask));
    CHECK_EQ(mask, 0x00000000FFFF0000ull);
    CHECK_EQ(cd::FormatCpuList(mask), L"16-31");
    CHECK_EQ(cd::FormatCpuListWithCount(mask), L"CPUs 16-31 (16 processors)");
    CHECK_EQ(cd::IrqPopCount(mask), 16);
}

void Test_W2_IrqTranspositionIsAFailureInTheSuite() {
    Case("W2 a transposed buffer decodes to the GAME's own processors and is not the golden mask");
    // THE TRANSPOSITION HAZARD IS DOCUMENTED HERE RATHER THAN IN A COMMENT. ff ff 00 00 is
    // the same eight bytes in the wrong order, and it names CPUs 0-15 - the half of the
    // reference machine the game is put on. Nothing in this product writes this value, and
    // the decoder must still not quietly agree with the correct one.
    const BYTE transposed[8] = { 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    ULONG_PTR mask = 0;
    CHECK(cd::IrqRegBytesToMask(transposed, sizeof(transposed), mask));
    CHECK_EQ(mask, 0x000000000000FFFFull);
    CHECK_EQ(cd::FormatCpuList(mask), L"0-15");
    CHECK_NE(mask, 0x00000000FFFF0000ull);
}

void Test_W3_IrqDecoderLengths() {
    Case("W3 the decoder accepts 4 and 8 bytes and refuses every other length");
    const BYTE four[4] = { 0x00, 0x00, 0xFF, 0xFF };
    ULONG_PTR mask = 0;
    CHECK(cd::IrqRegBytesToMask(four, sizeof(four), mask));
    CHECK_EQ(mask, 0x00000000FFFF0000ull);

    const BYTE odd[16] = { 0 };
    ULONG_PTR untouched = 0x1234ull;
    CHECK(!cd::IrqRegBytesToMask(odd, 3, untouched));
    CHECK(!cd::IrqRegBytesToMask(odd, 16, untouched));
    CHECK(!cd::IrqRegBytesToMask(odd, 0, untouched));
    CHECK(!cd::IrqRegBytesToMask(nullptr, 8, untouched));
    CHECK_EQ(untouched, 0x1234ull);   // a refused decode leaves the caller's value alone
}

void Test_W13_IrqRegistryReadGuards() {
    Case("W13 a registry value that changed between the two size queries is refused, not indexed");
    // [M] Council review 2026-09-08 called this the blocker. ReadBinaryValue checked the size
    // the FIRST RegQueryValueExW reported and then trusted the SECOND, so a value that shrank
    // to zero bytes between the two returned true holding an EMPTY vector - and the caller in
    // irq_probe.cpp took &bytes[0] of it, which is undefined behaviour. These are the two
    // guards that removed it, both lifted into the pure half so this test can reach them.
    CHECK(cd::IrqRegSecondReadIsUsable(ERROR_SUCCESS, 8, 8));
    CHECK(cd::IrqRegSecondReadIsUsable(ERROR_SUCCESS, 8, 4));    // shrank, but really read
    CHECK(!cd::IrqRegSecondReadIsUsable(ERROR_SUCCESS, 8, 0));   // THE BLOCKER
    CHECK(!cd::IrqRegSecondReadIsUsable(ERROR_SUCCESS, 8, 9));   // grew past the buffer
    CHECK(!cd::IrqRegSecondReadIsUsable(ERROR_MORE_DATA, 8, 8));
    CHECK(!cd::IrqRegSecondReadIsUsable(ERROR_ACCESS_DENIED, 8, 8));

    // AND THE CALL SITE DOES NOT RELY ON THE CALLEE. The vector form refuses an empty buffer
    // instead of forming &bytes[0] on it, so the undefined behaviour cannot be reached even
    // if some future reader hands it one.
    std::vector<BYTE> empty;
    ULONG_PTR untouched = 0x1234ull;
    CHECK(!cd::IrqRegBytesToMask(empty, untouched));
    CHECK_EQ(untouched, 0x1234ull);

    // [M] The reference machine's own bytes, through the vector form this time.
    const BYTE goldenBytes[8] = { 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    std::vector<BYTE> golden(goldenBytes, goldenBytes + 8);
    ULONG_PTR mask = 0;
    CHECK(cd::IrqRegBytesToMask(golden, mask));
    CHECK_EQ(mask, 0x00000000FFFF0000ull);

    // A length the decoder refuses is still refused through the vector form, and still leaves
    // the caller's mask alone.
    std::vector<BYTE> three(3, 0);
    CHECK(!cd::IrqRegBytesToMask(three, untouched));
    CHECK_EQ(untouched, 0x1234ull);
}

void Test_W4_IrqFormatCpuList() {
    Case("W4 processor lists collapse runs and never invent one");
    CHECK_EQ(cd::FormatCpuList(0x0000000000000111ull), L"0, 4, 8");
    CHECK_EQ(cd::FormatCpuList(0x000000000000023Bull), L"0-1, 3-5, 9");
    CHECK_EQ(cd::FormatCpuList(0ull), L"");
    CHECK_EQ(cd::FormatCpuList(0x8000000000000000ull), L"63");
    CHECK_EQ(cd::FormatCpuList(0x0000000000000001ull), L"0");
    CHECK_EQ(cd::FormatCpuListWithCount(0ull), L"no processors");
    CHECK_EQ(cd::FormatCpuListWithCount(0x0000000000000001ull), L"CPUs 0 (1 processor)");
    CHECK_EQ(cd::IrqPopCount(0ull), 0);
    CHECK_EQ(cd::IrqPopCount(0xFFFFFFFFFFFFFFFFull), 64);
}

void Test_W5_IrqTemporalTargetSetDecode() {
    Case("W5 the two measured temporal target sets decode to complementary striped halves");
    // [M] 2026-09-08: the reference machine's RTX 5090 read TargetSet = 0x99999999 and its
    // RTX 4090 read 0x66666666. They are exact bitwise complements over 32 processors, which
    // is what made the striped split visible in the first place.
    const ULONG_PTR five = cd::IrqTargetSetToMask(0x99999999u);
    const ULONG_PTR four = cd::IrqTargetSetToMask(0x66666666u);
    CHECK_EQ(cd::FormatCpuList(five), L"0, 3-4, 7-8, 11-12, 15-16, 19-20, 23-24, 27-28, 31");
    CHECK_EQ(cd::FormatCpuList(four), L"1-2, 5-6, 9-10, 13-14, 17-18, 21-22, 25-26, 29-30");
    CHECK_EQ(cd::IrqPopCount(five), 16);
    CHECK_EQ(cd::IrqPopCount(four), 16);
    CHECK_EQ(five & four, 0ull);
    CHECK_EQ(five | four, 0x00000000FFFFFFFFull);
}

void Test_W6_IrqReferenceMachineIsInScope() {
    Case("W6 POSITIVE CONTROL - the reference machine is not refused");
    // Without this the refusal tests below could all pass on an evaluator that refuses every
    // machine it is ever shown.
    CHECK_EQ(cd::IrqEvaluateMachine(MakeReference(false)), cd::IrqRefusal::None);
    CHECK_EQ(cd::IrqEvaluateMachine(MakeIntelHybrid()), cd::IrqRefusal::None);
    CHECK_EQ(cd::IrqEvaluateMachine(MakeSingleDomain()), cd::IrqRefusal::None);
    CHECK(!cd::IrqRefusalIsHard(cd::IrqRefusal::None));
}

void Test_W7_IrqTwoGroupsRefused() {
    Case("W7 two processor groups are refused, by EITHER of two independent tests");
    // groupCount alone cannot carry this gate: topology.cpp rewrites a failed group query
    // from 0 to 1, so a machine whose query failed looks single-group. The per-entry Group
    // field comes from a different API and is checked separately.
    CHECK_EQ(cd::IrqEvaluateMachine(MakeTwoProcessorGroups()),
             cd::IrqRefusal::MultipleProcessorGroups);
    CHECK_EQ(cd::IrqEvaluateMachine(MakeGroupCountOneButAnEntryInGroupOne()),
             cd::IrqRefusal::MultipleProcessorGroups);
    CHECK(cd::IrqRefusalIsHard(cd::IrqRefusal::MultipleProcessorGroups));
}

void Test_W8_IrqTooManyProcessorsRefused() {
    Case("W8 more than 64 logical processors is refused");
    CHECK_EQ(cd::IrqEvaluateMachine(MakeNinetySixLogicalProcessors()),
             cd::IrqRefusal::TooManyProcessors);
}

void Test_W9_IrqUnexpectedNumberingRefused() {
    Case("W9 numbering that does not start at 0 is refused rather than guessed at");
    CHECK_EQ(cd::IrqEvaluateMachine(MakeNumberingThatStartsAtSixtyFour()),
             cd::IrqRefusal::UnexpectedProcessorNumbering);
    cd::Topology empty;
    CHECK_EQ(cd::IrqEvaluateMachine(empty), cd::IrqRefusal::UnexpectedProcessorNumbering);
}

void Test_W10_IrqBuildMaskWholeChain() {
    Case("W10 the whole chain - named mask, ids, lps, mask - on the reference machine");
    const cd::Topology t = MakeReference(false);
    const std::vector<cd::Mask> masks = cd::DeriveMasks(t);

    const cd::Mask* freq = MaskNamed(masks, L"Freq");
    CHECK(freq != nullptr);
    if (freq) {
        ULONG_PTR mask = 0;
        CHECK(cd::BuildIrqMask(t, cd::LpsForIds(t, freq->ids), mask));
        CHECK_EQ(mask, 0x00000000FFFF0000ull);
        CHECK_EQ(cd::FormatCpuListWithCount(mask), L"CPUs 16-31 (16 processors)");
    }

    const cd::Mask* cache = MaskNamed(masks, L"Cache");
    CHECK(cache != nullptr);
    if (cache) {
        ULONG_PTR mask = 0;
        CHECK(cd::BuildIrqMask(t, cd::LpsForIds(t, cache->ids), mask));
        CHECK_EQ(mask, 0x000000000000FFFFull);
    }
}

void Test_W11_IrqBuildMaskRefuses() {
    Case("W11 the mask builder refuses rather than silently dropping a processor");
    const cd::Topology t = MakeReference(false);
    ULONG_PTR mask = 0xAAAAull;
    CHECK(!cd::BuildIrqMask(t, std::vector<ULONG>(), mask));

    std::vector<ULONG> absent;
    absent.push_back(0);
    absent.push_back(99);
    CHECK(!cd::BuildIrqMask(t, absent, mask));

    std::vector<ULONG> tooHigh;
    tooHigh.push_back(64);
    CHECK(!cd::BuildIrqMask(t, tooHigh, mask));
    CHECK_EQ(mask, 0xAAAAull);   // untouched on every refusal
}

void Test_W12_IrqRefusalReasonsAreDistinct() {
    Case("W12 every refusal has its own non-empty sentence, None included");
    const cd::IrqRefusal all[] = {
        cd::IrqRefusal::None, cd::IrqRefusal::MultipleProcessorGroups,
        cd::IrqRefusal::TooManyProcessors, cd::IrqRefusal::UnexpectedProcessorNumbering,
        cd::IrqRefusal::CountersUnavailable
    };
    std::vector<std::wstring> said;
    for (size_t i = 0; i < ARRAYSIZE(all); ++i) said.push_back(cd::IrqRefusalReason(all[i]));
    AllPairwiseDistinct(said, "IrqRefusalReason is non-empty and pairwise distinct");
    // IrqRefusalIsHard is true for every value BUT None. A refusal that reads as "not a
    // refusal" would let the window show a readout it has already decided is wrong.
    for (size_t i = 0; i < ARRAYSIZE(all); ++i)
        CHECK_EQ(cd::IrqRefusalIsHard(all[i]), all[i] != cd::IrqRefusal::None);
}

// ---------------------------------------------------------------------------
// X. The honesty of the readout.
// ---------------------------------------------------------------------------

void Test_X1_IrqTooFewSamplesIsNeverMeasuredAndNeverHot() {
    Case("X1 fewer than three valid samples is not measured, whatever the mean says");
    const cd::CoreDpcStat loudButThin = IrqStat(3, 2, 36.87, 13.97, true);
    CHECK(!cd::IrqCoreIsMeasured(loudButThin));
    CHECK(!cd::IrqCoreIsHot(loudButThin));
    const cd::CoreDpcStat loudAndReal = IrqStat(3, 3, 36.87, 13.97, true);
    CHECK(cd::IrqCoreIsMeasured(loudAndReal));
    CHECK(cd::IrqCoreIsHot(loudAndReal));
}

void Test_X2_IrqHotIsInterruptPlusDpc() {
    Case("X2 hot is interrupt service PLUS DPC, at or above the printed threshold");
    CHECK_EQ(cd::IrqCombinedPct(IrqStat(0, 7, 3.0, 2.0, true)), 5.0);
    CHECK(cd::IrqCoreIsHot(IrqStat(0, 7, 3.0, 2.0, true)));     // exactly at the threshold
    CHECK(!cd::IrqCoreIsHot(IrqStat(0, 7, 2.0, 2.0, true)));
    // Either half alone can carry it: an ISR-heavy processor is as unreachable by a CPU Set
    // as a DPC-heavy one.
    CHECK(cd::IrqCoreIsHot(IrqStat(0, 7, 0.0, 9.0, true)));
    CHECK(cd::IrqCoreIsHot(IrqStat(0, 7, 9.0, 0.0, true)));
}

void Test_X3_IrqGroupMembershipIsExact() {
    Case("X3 group membership is set from the group's own processor list and cleared otherwise");
    std::vector<cd::CoreDpcStat> cores;
    for (ULONG lp = 0; lp < 8; ++lp) cores.push_back(IrqStat(lp, 7, 0.0, 0.0, true));
    std::vector<ULONG> group;
    group.push_back(2);
    group.push_back(5);
    cd::IrqMarkGameGroup(cores, group);
    for (size_t i = 0; i < cores.size(); ++i)
        CHECK_EQ(cores[i].inGameGroup, cores[i].lp == 2 || cores[i].lp == 5);
    std::vector<ULONG> allLps;
    for (ULONG lp = 0; lp < 8; ++lp) allLps.push_back(lp);
    CHECK_EQ(cd::IrqCoresInGroup(allLps, group), 2);

    // An EMPTY group list marks nothing - it must not fall back to "everything".
    cd::IrqMarkGameGroup(cores, std::vector<ULONG>());
    for (size_t i = 0; i < cores.size(); ++i) CHECK(!cores[i].inGameGroup);
    CHECK_EQ(cd::IrqCoresInGroup(allLps, std::vector<ULONG>()), 0);
}

void Test_X4_IrqHottestInGroupIgnoresTheRestOfTheMachine() {
    Case("X4 the in-group hottest ignores a louder processor outside the group");
    std::vector<cd::CoreDpcStat> cores;
    cores.push_back(IrqStat(1, 7, 36.87, 13.97, false));   // loudest, but not in the group
    cores.push_back(IrqStat(18, 7, 0.10, 0.05, true));
    cores.push_back(IrqStat(19, 7, 0.40, 0.05, true));
    CHECK_EQ(cd::IrqHottestInGroupIndex(cores), 2);
    CHECK_EQ(cd::IrqHottestOverallIndex(cores), 0);
    CHECK(!cd::IrqHotCoreIsInGameGroup(cores));
}

void Test_X5_IrqNothingMeasuredInTheGroup() {
    Case("X5 a group with no valid samples reports -1, never a processor it did not measure");
    std::vector<cd::CoreDpcStat> cores;
    cores.push_back(IrqStat(1, 7, 36.87, 13.97, false));
    cores.push_back(IrqStat(18, 1, 0.10, 0.05, true));     // one sample: not measured
    CHECK_EQ(cd::IrqHottestInGroupIndex(cores), -1);
    CHECK(!cd::IrqHotCoreIsInGameGroup(cores));
    CHECK_EQ(cd::IrqQuietCoresInGroup(cores), 0);          // unmeasured is not quiet either
}

void Test_X6_IrqQuietCount() {
    Case("X6 the quiet count is measured in-group processors at or under the printed floor");
    std::vector<cd::CoreDpcStat> cores;
    cores.push_back(IrqStat(0, 7, 0.00, 0.00, true));
    cores.push_back(IrqStat(1, 7, 0.50, 0.50, true));      // exactly 1.00 - quiet
    cores.push_back(IrqStat(2, 7, 0.90, 0.20, true));      // 1.10 - not quiet
    cores.push_back(IrqStat(3, 7, 36.87, 13.97, true));
    cores.push_back(IrqStat(16, 7, 0.00, 0.00, false));    // outside the group
    std::vector<ULONG> allLps, group;
    for (ULONG lp = 0; lp < 4; ++lp) { allLps.push_back(lp); group.push_back(lp); }
    allLps.push_back(16);
    CHECK_EQ(cd::IrqCoresInGroup(allLps, group), 4);
    CHECK_EQ(cd::IrqQuietCoresInGroup(cores), 2);
    CHECK(cd::IrqHotCoreIsInGameGroup(cores));
}

void Test_X7_IrqReferenceMachineWarningFires() {
    Case("X7 the reference machine's own numbers produce the warning this page exists for");
    // [M] 2026-09-08: ISR 13.97% / DPC 36.87% on one processor while the rest of that CCD
    // sat at 0.00%.
    std::vector<cd::CoreDpcStat> cores;
    cores.push_back(IrqStat(3, 7, 36.87, 13.97, true));
    for (ULONG lp = 4; lp < 16; ++lp) cores.push_back(IrqStat(lp, 7, 0.00, 0.00, true));
    CHECK(cd::IrqHotCoreIsInGameGroup(cores));

    // The same numbers on a processor OUTSIDE the group are not this warning.
    std::vector<cd::CoreDpcStat> outside;
    outside.push_back(IrqStat(3, 7, 36.87, 13.97, false));
    for (ULONG lp = 16; lp < 32; ++lp) outside.push_back(IrqStat(lp, 7, 0.00, 0.00, true));
    CHECK(!cd::IrqHotCoreIsInGameGroup(outside));
}

void Test_X8_IrqAgreementCoversEveryState() {
    Case("X8 the two interrupt keys are classified, including the measured disagreement");
    cd::IrqPolicyReadout r;
    r.readFailed = true;
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::Unreadable);

    r = cd::IrqPolicyReadout();
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::NeitherPresent);

    r = cd::IrqPolicyReadout();
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::PolicyOnly);

    r = cd::IrqPolicyReadout();
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x99999999u);
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::TargetOnly);

    r = cd::IrqPolicyReadout();
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    r.targetPresent = true;
    r.targetMask = 0x00000000FFFF0000ull;
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::Agree);

    // [M] THE REFERENCE MACHINE'S OWN RTX 4090, 2026-09-08. The static policy named CPUs
    // 16-31 and Windows' own temporal target set named the striped complement, which
    // includes the processor that was carrying the load. Showing only the first would have
    // reported the input and called it the outcome.
    r = cd::IrqPolicyReadout();
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x66666666u);
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::Disagree);
    const std::wstring said = cd::IrqAgreementText(cd::IrqAgreement::Disagree, r);
    CHECK(said.find(L"CPUs 16-31 (16 processors)") != std::wstring::npos);
    CHECK(said.find(L"1-2, 5-6, 9-10, 13-14, 17-18, 21-22, 25-26, 29-30") !=
          std::wstring::npos);
}

void Test_X9_IrqSwitchCompleteness() {
    Case("X9 every enum value has its own sentence - a new value fails here, not on screen");
    cd::IrqPolicyReadout r;
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x66666666u);

    const cd::IrqAgreement agreements[] = {
        cd::IrqAgreement::Unreadable, cd::IrqAgreement::NeitherPresent,
        cd::IrqAgreement::PolicyOnly, cd::IrqAgreement::TargetOnly,
        cd::IrqAgreement::Agree, cd::IrqAgreement::Disagree
    };
    std::vector<std::wstring> said;
    for (size_t i = 0; i < ARRAYSIZE(agreements); ++i)
        said.push_back(cd::IrqAgreementText(agreements[i], r));
    AllPairwiseDistinct(said, "IrqAgreementText covers every value distinctly");

    const cd::IrqPolicyState states[] = { cd::IrqPolicyState::NotConfigured,
                                          cd::IrqPolicyState::Configured,
                                          cd::IrqPolicyState::Unreadable };
    std::vector<std::wstring> stateText;
    for (size_t i = 0; i < ARRAYSIZE(states); ++i)
        stateText.push_back(cd::IrqPolicyStateText(states[i], L"CPUs 16-31 (16 processors)"));
    AllPairwiseDistinct(stateText, "IrqPolicyStateText covers every value distinctly");

    const cd::GameGroupSource sources[] = { cd::GameGroupSource::LiveProfile,
                                            cd::GameGroupSource::MachineDefault,
                                            cd::GameGroupSource::Unknown };
    std::vector<std::wstring> phrases;
    for (size_t i = 0; i < ARRAYSIZE(sources); ++i)
        phrases.push_back(cd::GameGroupPhrase(sources[i]));
    AllPairwiseDistinct(phrases, "GameGroupPhrase covers every value distinctly");
}

void Test_X12_IrqPresentButUndecodablePolicyIsNeverAbsent() {
    Case("X12 a policy that is there and will not decode reads as unreadable, never as not set");
    // [M] Council review 2026-09-08, C2 / H3. An AssignmentSetOverride of a length this page
    // cannot decode used to leave policyPresent false with nothing else set, so the device
    // reported "Not set. Windows chooses this device's interrupt processors." That is the one
    // fold the comment above IrqPolicyState forbids: unreadable folded into absent.
    cd::IrqPolicyReadout r;
    r.policyUnreadable = true;
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::Unreadable);
    CHECK_EQ(cd::IrqPolicyStateOf(r), cd::IrqPolicyState::Unreadable);

    // ...and it does not become "this device carries no interrupt affinity policy" merely
    // because Windows recorded a target set of its own beside it.
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x66666666u);
    CHECK_EQ(cd::IrqClassifyAgreement(r), cd::IrqAgreement::Unreadable);
    CHECK_EQ(cd::IrqPolicyStateOf(r), cd::IrqPolicyState::Unreadable);

    // THE TWO STATES THAT MUST NOT MOVE. A device with nothing under Affinity Policy is still
    // "not set", and a decoded policy is still "set to".
    cd::IrqPolicyReadout absent;
    CHECK_EQ(cd::IrqClassifyAgreement(absent), cd::IrqAgreement::NeitherPresent);
    CHECK_EQ(cd::IrqPolicyStateOf(absent), cd::IrqPolicyState::NotConfigured);
    cd::IrqPolicyReadout configured;
    configured.policyPresent = true;
    configured.policyMask = 0x00000000FFFF0000ull;
    CHECK_EQ(cd::IrqClassifyAgreement(configured), cd::IrqAgreement::PolicyOnly);
    CHECK_EQ(cd::IrqPolicyStateOf(configured), cd::IrqPolicyState::Configured);

    // THE WIRING, NOT ONLY THE CLASSIFIER. IrqDevice::Readout is where the probe's two flags
    // are folded, and overridePresent used to be written there and read nowhere at all.
    cd::IrqDevice undecodable;
    undecodable.overridePresent = true;    // the value IS in the registry
    undecodable.overrideDecoded = false;   // and this page could not read it
    CHECK(undecodable.Readout().policyUnreadable);
    CHECK_EQ(cd::IrqPolicyStateOf(undecodable.Readout()), cd::IrqPolicyState::Unreadable);
    CHECK_EQ(cd::IrqClassifyAgreement(undecodable.Readout()), cd::IrqAgreement::Unreadable);

    cd::IrqDevice nothingThere;
    CHECK(!nothingThere.Readout().policyUnreadable);
    CHECK_EQ(cd::IrqPolicyStateOf(nothingThere.Readout()), cd::IrqPolicyState::NotConfigured);

    cd::IrqDevice decoded;
    decoded.overridePresent = true;
    decoded.overrideDecoded = true;
    decoded.overrideMask = 0x00000000FFFF0000ull;
    CHECK(!decoded.Readout().policyUnreadable);
    CHECK_EQ(cd::IrqPolicyStateOf(decoded.Readout()), cd::IrqPolicyState::Configured);
}

void Test_X13_IrqUnmeasuredGroupMemberIsStillInTheGroup() {
    Case("X13 a group processor the counters never returned is still in the group and still in M");
    // [M] Council review 2026-09-08, C3 / H8. Membership used to be read off the SAMPLED
    // results, so a processor PDH never named left the group entirely: it painted as though it
    // were outside, and "N of M processors in that group" quietly described a smaller set than
    // the mask the same page had just printed.
    std::vector<ULONG> allLps;
    for (ULONG lp = 0; lp < 32; ++lp) allLps.push_back(lp);
    std::vector<ULONG> group;
    for (ULONG lp = 0; lp < 16; ++lp) group.push_back(lp);

    // Only two of the sixteen returned anything at all.
    std::vector<cd::CoreDpcStat> cores;
    cores.push_back(IrqStat(3, 7, 36.87, 13.97, false));
    cores.push_back(IrqStat(4, 7, 0.00, 0.00, false));
    cd::IrqMarkGameGroup(cores, group);

    CHECK_EQ(cd::IrqCoresInGroup(allLps, group), 16);   // the GROUP's size, not the sample's
    CHECK(cd::IrqLpIsInGameGroup(9, group));            // never sampled, still in the group
    CHECK(cd::IrqLpIsInGameGroup(3, group));
    CHECK(!cd::IrqLpIsInGameGroup(16, group));

    // The counts that are ABOUT MEASUREMENT still are: an unmeasured processor is neither
    // quiet nor loud, and it does not become quiet by being in the group.
    CHECK_EQ(cd::IrqQuietCoresInGroup(cores), 1);
    CHECK(cd::IrqHotCoreIsInGameGroup(cores));

    // An empty group list is not "everything", and a group naming processors this machine does
    // not have does not inflate the count.
    CHECK_EQ(cd::IrqCoresInGroup(allLps, std::vector<ULONG>()), 0);
    std::vector<ULONG> beyond;
    beyond.push_back(40);
    beyond.push_back(99);
    CHECK_EQ(cd::IrqCoresInGroup(allLps, beyond), 0);
    CHECK_EQ(cd::IrqCoresInGroup(std::vector<ULONG>(), group), 0);
}

// EVERY user-facing string this readout can put on screen, in one place. Both halves of the
// wording gate run over exactly this list, so a sentence added to irq_policy.h and not added
// here is the one way the gate can be got round - which is why the list is assembled from the
// accessor functions rather than from copies of their text.
std::vector<std::wstring> IrqEveryUserString() {
    std::vector<std::wstring> v;
    v.push_back(cd::IrqBenchIntroText());
    v.push_back(cd::IrqDeviceHeadingText());
    v.push_back(cd::IrqNoDevicesText());
    v.push_back(cd::IrqWindowsChoosesText());
    v.push_back(cd::IrqUnreadableColumnText());
    v.push_back(cd::IrqNotRecordedText());
    v.push_back(cd::IrqNotSetText());
    v.push_back(cd::IrqNotStartedText());
    v.push_back(cd::IrqCardHeadingText());
    v.push_back(cd::IrqCardLineText());
    v.push_back(cd::IrqCardButtonCaption());
    v.push_back(cd::IrqReproductionCommand());

    const cd::IrqRefusal refusals[] = {
        cd::IrqRefusal::None, cd::IrqRefusal::MultipleProcessorGroups,
        cd::IrqRefusal::TooManyProcessors, cd::IrqRefusal::UnexpectedProcessorNumbering,
        cd::IrqRefusal::CountersUnavailable
    };
    for (size_t i = 0; i < ARRAYSIZE(refusals); ++i)
        v.push_back(cd::IrqRefusalReason(refusals[i]));

    const cd::IrqPolicyState states[] = { cd::IrqPolicyState::NotConfigured,
                                          cd::IrqPolicyState::Configured,
                                          cd::IrqPolicyState::Unreadable };
    for (size_t i = 0; i < ARRAYSIZE(states); ++i)
        v.push_back(cd::IrqPolicyStateText(states[i], L"CPUs 16-31 (16 processors)"));

    cd::IrqPolicyReadout r;
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x66666666u);
    const cd::IrqAgreement agreements[] = {
        cd::IrqAgreement::Unreadable, cd::IrqAgreement::NeitherPresent,
        cd::IrqAgreement::PolicyOnly, cd::IrqAgreement::TargetOnly,
        cd::IrqAgreement::Agree, cd::IrqAgreement::Disagree
    };
    for (size_t i = 0; i < ARRAYSIZE(agreements); ++i)
        v.push_back(cd::IrqAgreementText(agreements[i], r));

    const cd::GameGroupSource sources[] = { cd::GameGroupSource::LiveProfile,
                                            cd::GameGroupSource::MachineDefault,
                                            cd::GameGroupSource::Unknown };
    for (size_t i = 0; i < ARRAYSIZE(sources); ++i) {
        v.push_back(cd::GameGroupPhrase(sources[i]));
        v.push_back(cd::IrqGameGroupSentence(sources[i], L"Cache", 0x000000000000FFFFull));
        v.push_back(cd::IrqNoGroupMeasuredSentence(sources[i], L"Cache"));
        v.push_back(cd::IrqHotCoreSentence(IrqStat(3, 7, 36.87, 13.97, true), 14, 16,
                                           sources[i], L"Cache"));
        v.push_back(cd::IrqHotCoreOutsideGroupSentence(IrqStat(1, 7, 36.87, 13.97, false),
                                                       sources[i], L"Cache"));
    }
    v.push_back(cd::IrqNoHotCoreSentence(IrqStat(4, 7, 0.20, 0.11, true)));
    v.push_back(cd::IrqNotMeasuredSentence(9));
    v.push_back(cd::FormatCpuListWithCount(0x00000000FFFF0000ull));
    v.push_back(cd::FormatCpuListWithCount(0ull));
    return v;
}

void Test_X10_IrqWordingTripwire() {
    Case("X10 THE TRIPWIRE - no string on this page promises anything the product cannot keep");
    // A TRIPWIRE, NOT THE GATE. It catches the words that would turn an instrument into a
    // claim; the exact-text pins in group Y are what stop a rewrite that avoids all of them
    // and still says something new. Said here so no future session mistakes one for the
    // other.
    //
    // IT RUNS AT ALL BECAUSE THE PRODUCT'S OWN NAME IS ABSENT FROM src\irq_policy.h BY
    // DESIGN - every sentence there says "this page". An earlier draft of this feature would
    // have banned the same token while its copy said the application's name four times, so
    // the gate would have been relaxed on its first run and by exception forever.
    const wchar_t* const tier1[] = {
        L"fps", L"framerate", L"frame rate", L"smoother", L"boost", L"speed up", L"faster",
        L"improve", L"better", L"guarantee", L"optimi", L"fix", L"verified", L"proven",
        L"working", L"active", L"will help", L"should help", L"in effect"
    };
    const std::vector<std::wstring> strings = IrqEveryUserString();

    // POSITIVE CONTROL: the scan must be able to find a banned token when one is really
    // there, or a clean run proves nothing at all.
    {
        const std::wstring planted = L"this page will make your game FASTER";
        bool found = false;
        for (size_t t = 0; t < ARRAYSIZE(tier1); ++t)
            if (cd::ToLower(planted).find(cd::ToLower(tier1[t])) != std::wstring::npos)
                found = true;
        CHECK(found);
    }

    CHECK(strings.size() > 30);
    for (size_t i = 0; i < strings.size(); ++i) {
        const std::wstring folded = cd::ToLower(strings[i]);
        for (size_t t = 0; t < ARRAYSIZE(tier1); ++t) {
            ++g_total;
            if (folded.find(cd::ToLower(tier1[t])) != std::wstring::npos) {
                Fail(__FILE__, __LINE__, "Tier-1 wording ban", Utf8(strings[i]),
                     std::string("a sentence with no \"") + Utf8(tier1[t]) + "\" in it");
            }
        }
    }
}

void Test_X11_IrqEveryStringIsNonEmpty() {
    Case("X11 no user-facing string on this page is empty");
    // A blank label reads as a machine with nothing to say. The one string allowed to be
    // empty is FormatCpuList of an empty mask, and it is deliberately not in this list.
    const std::vector<std::wstring> strings = IrqEveryUserString();
    for (size_t i = 0; i < strings.size(); ++i) CHECK(!strings[i].empty());
}

// ---------------------------------------------------------------------------
// Y. The sentences that carry numbers, pinned character for character.
//
// THIS IS THE GATE. Any edit to any of these sentences fails the build until a human
// re-approves it, which is the only check that can catch "this makes your game run well"
// while still permitting "this is not a remedy". Modelled on Test_P1 above, this tree's
// existing precedent for pinning a literal.
// ---------------------------------------------------------------------------

void Test_Y1_IrqHotCoreSentenceIsExact() {
    Case("Y1 the hot-processor warning matches character for character");
    cd::CoreDpcStat c = IrqStat(3, 7, 36.87, 13.97, true);
    c.minDpcPct = 36.80;
    c.maxDpcPct = 36.94;
    const std::wstring expected =
        L"CPU 3 spent 36.87% of the capture on DPCs and 13.97% on interrupt service "
        L"(DPC range 36.80-36.94% over 7 samples). It is in \"Cache\", the group your game is "
        L"assigned to. 14 of the 16 processors in that group stayed at or under 1.00%.";
    CHECK_EQ(cd::IrqHotCoreSentence(c, 14, 16, cd::GameGroupSource::LiveProfile, L"Cache"),
             expected);
}

void Test_Y2_IrqNoHotCoreSentenceIsExact() {
    Case("Y2 the quiet-group sentence matches character for character");
    const std::wstring expected =
        L"Nothing in the game's group is above 5.00% of interrupt and DPC time together. "
        L"The highest is CPU 4 at 0.31%.";
    CHECK_EQ(cd::IrqNoHotCoreSentence(IrqStat(4, 7, 0.20, 0.11, true)), expected);
}

void Test_Y3_IrqOutsideGroupSentenceIsExact() {
    Case("Y3 the outside-the-group sentence matches character for character");
    const std::wstring expected =
        L"CPU 1 spent 36.87% of the capture on DPCs and 13.97% on interrupt service. "
        L"It is NOT in \"Cache\", this machine's default game group - no game is running "
        L"right now, so it is outside the processors this page would warn about.";
    CHECK_EQ(cd::IrqHotCoreOutsideGroupSentence(IrqStat(1, 7, 36.87, 13.97, false),
                                                cd::GameGroupSource::MachineDefault, L"Cache"),
             expected);
}

void Test_Y4_IrqNothingMeasuredSentencesAreExact() {
    Case("Y4 'not measured' never reads as 0.00%, and says so in as many words");
    const std::wstring group =
        L"No processor in \"Freq\", the group your game is assigned to, returned enough valid "
        L"samples to report. That is not the same as those processors being idle.";
    CHECK_EQ(cd::IrqNoGroupMeasuredSentence(cd::GameGroupSource::LiveProfile, L"Freq"), group);

    const std::wstring one =
        L"CPU 9 - not measured. Fewer than 3 valid samples were returned for it. That is not "
        L"the same as 0.00%.";
    CHECK_EQ(cd::IrqNotMeasuredSentence(9), one);
}

void Test_Y5_IrqGameGroupSentenceIsExact() {
    Case("Y5 the group sentence names its SOURCE, never only the mask");
    const std::wstring expected =
        L"Measured against \"Cache\", this machine's default game group - no game is running "
        L"right now: CPUs 0-15 (16 processors).";
    CHECK_EQ(cd::IrqGameGroupSentence(cd::GameGroupSource::MachineDefault, L"Cache",
                                      0x000000000000FFFFull),
             expected);
}

void Test_Y6_IrqReproductionCommandIsExact() {
    Case("Y6 the reproduction command matches the capture this page actually takes");
    const std::wstring expected =
        L"Check this yourself: Get-Counter '\\Processor Information(*)\\% DPC Time',"
        L"'\\Processor Information(*)\\% Interrupt Time' -SampleInterval 1 -MaxSamples 8 - "
        L"an instance named \"0,3\" is CPU 3 in processor group 0. This page throws away the "
        L"first collection, because a rate counter has nothing to compare against yet, and "
        L"averages the remaining 7.";
    CHECK_EQ(cd::IrqReproductionCommand(), expected);
    // The command has to describe the capture the code takes, not a capture somebody typed
    // once: both numbers in it come from kIrqCollects and kIrqMinSamples.
    CHECK_EQ(cd::kIrqCollects, 8);
    CHECK_EQ(cd::kIrqMinSamples, 3);
}

void Test_Y7_IrqPolicyStateTextIsExact() {
    Case("Y7 the per-device policy sentences match character for character");
    CHECK_EQ(cd::IrqPolicyStateText(cd::IrqPolicyState::NotConfigured, L""),
             L"Not set. Windows chooses this device's interrupt processors.");
    CHECK_EQ(cd::IrqPolicyStateText(cd::IrqPolicyState::Configured,
                                    L"CPUs 16-31 (16 processors)"),
             L"Set to CPUs 16-31 (16 processors). That is what the registry holds; it is not "
             L"a statement about where interrupts land.");
    CHECK_EQ(cd::IrqPolicyStateText(cd::IrqPolicyState::Unreadable, L""),
             L"This page could not read this device's interrupt policy. That is not the same "
             L"as there being none - it means the read failed.");
}

void Test_Y8_IrqDisagreementSentenceIsExact() {
    Case("Y8 the disagreement sentence - the whole point of the readout - is pinned");
    cd::IrqPolicyReadout r;
    r.policyPresent = true;
    r.policyMask = 0x00000000FFFF0000ull;
    r.targetPresent = true;
    r.targetMask = cd::IrqTargetSetToMask(0x66666666u);
    const std::wstring expected =
        L"THESE TWO DISAGREE. The policy on this device names CPUs 16-31 (16 processors), and "
        L"the target set Windows recorded for it names "
        L"CPUs 1-2, 5-6, 9-10, 13-14, 17-18, 21-22, 25-26, 29-30 (16 processors). The policy "
        L"is what somebody asked for; the target set is what Windows wrote down. This page "
        L"cannot tell you which of them the hardware is following.";
    CHECK_EQ(cd::IrqAgreementText(cd::IrqAgreement::Disagree, r), expected);
}

void Test_Y9_IrqNoSentenceHardcodesAGroupName() {
    Case("Y9 no sentence hardcodes a group name - Cache and CCD0 are both real machines");
    // [M] The reference part emits "Cache" / "Freq"; a symmetric dual-CCD part emits
    // "CCD0" / "CCD1". A sentence that named either would be wrong on the other machine.
    const cd::CoreDpcStat c = IrqStat(3, 7, 36.87, 13.97, true);
    const std::wstring cache =
        cd::IrqHotCoreSentence(c, 14, 16, cd::GameGroupSource::LiveProfile, L"Cache");
    const std::wstring ccd0 =
        cd::IrqHotCoreSentence(c, 14, 16, cd::GameGroupSource::LiveProfile, L"CCD0");
    CHECK(cache.find(L"Cache") != std::wstring::npos);
    CHECK(cache.find(L"CCD0") == std::wstring::npos);
    CHECK(ccd0.find(L"CCD0") != std::wstring::npos);
    CHECK(ccd0.find(L"Cache") == std::wstring::npos);

    // An EMPTY mask name drops the quoted name rather than printing empty quotes.
    const std::wstring unnamed =
        cd::IrqHotCoreSentence(c, 14, 16, cd::GameGroupSource::Unknown, L"");
    CHECK(unnamed.find(L"\"\"") == std::wstring::npos);
    CHECK(unnamed.find(L"a group this page could not identify") != std::wstring::npos);
}

void Test_Y11_IrqCardLineIsExact() {
    Case("Y11 the Settings card sentence - the one seen without opening the page - is pinned");
    // THIS PIN IS THE FIX. [M] Council review 2026-09-08, C1 / H1: the card used to say "no
    // processor assignment can move one" and "there is a kind of stutter nothing on this
    // window can reach". The first is contradicted by this project's own measurement - the
    // interrupt and DPC load moved from CPU 1 to CPU 3 when Windows' temporal TargetSet
    // changed - and the second asserts a symptom no frame rate was ever measured for. The
    // deny-list in X10 caught neither, because neither says a banned word. Only an exact pin
    // can catch a sentence that is merely too strong, which is why this test exists.
    const std::wstring expected =
        L"CPU Sets move threads. A driver's deferred procedure call is not a thread - it runs "
        L"above every thread on its processor - so the processor assignment this window makes "
        L"cannot move one. This opens a read-only readout of where interrupt and DPC time is "
        L"landing on this machine. It measures; it changes nothing.";
    CHECK_EQ(std::wstring(cd::IrqCardLineText()), expected);

    // The two struck claims, named so no rewording can quietly restore them.
    const std::wstring folded = cd::ToLower(std::wstring(cd::IrqCardLineText()));
    CHECK(folded.find(L"stutter") == std::wstring::npos);
    CHECK(folded.find(L"no processor assignment") == std::wstring::npos);
    CHECK(folded.find(L"any processor assignment") == std::wstring::npos);

    // The card and the bench intro must not contradict each other: both scope the claim to
    // what moves THREADS, and the intro is the longer form of the same sentence.
    const std::wstring intro = cd::ToLower(std::wstring(cd::IrqBenchIntroText()));
    CHECK(intro.find(L"stutter") == std::wstring::npos);
    CHECK(intro.find(L"cpu sets move threads") != std::wstring::npos);
    CHECK(folded.find(L"cpu sets move threads") != std::wstring::npos);
}

void Test_Y10_IrqPercentFormatting() {
    Case("Y10 every figure is two decimals, so the screen and the clipboard agree");
    CHECK_EQ(cd::IrqFormatPct(0.0), L"0.00");
    CHECK_EQ(cd::IrqFormatPct(36.87), L"36.87");
    CHECK_EQ(cd::IrqFormatPct(100.0), L"100.00");
}


// ===========================================================================
// Z.  EXTREME GAME MODE - the blanket sweep (engine.h rule 4b).
//
// Z is the first free banner letter: A-Y are all taken, and W/X/Y went to the interrupt
// bench. Written from the header comments in config.h, engine.h and settings_warning.h.
//
// The whole rule is reachable without an OS: ExtremeSweepActive and ExtremeSweepEligible are
// pure by construction, and ComputeDesired is driven with the same synthetic snapshots
// section C uses. Nothing below opens a process.
// ===========================================================================

// The snapshot section C uses plus the things a BLANKET sweep has to reason about and the
// other rules never see: an ordinary process nobody named, an excluded process that is NOT on
// the heavy list, a wildcard-excluded process, and a process whose name could not be read.
//
//   pid 400  parent.exe                <- ordinary, nobody named it
//   pid 500  explorer.exe              <- ordinary here (the fixture's exclusion list is its own)
//   pid 1000 Overwatch.exe             <- the game
//   pid 1001/1002 descendants          <- game mask
//   pid 1003 EasyAntiCheat.exe         <- descendant AND excluded
//   pid 1004 OBS.exe                   <- descendant AND heavy
//   pid 2000 notepad.exe               <- THE PROCESS THIS RULE EXISTS FOR
//   pid 3000 OBS.exe                   <- heavy, not a descendant
//   pid 3001 audiodg.exe               <- heavy AND excluded (rule 3's override)
//   pid 5000 encoder.exe               <- EXCLUDED and on nobody's heavy list
//   pid 5001 amd3dvcacheUser.exe       <- excluded by the trailing-* wildcard
//   pid 5002 <empty name>              <- unreadable: the exclusion list cannot be consulted
//   pid 6000 msedgewebview2.exe        <- OUR OWN child (parent 9000 == selfPid)
//   pid 9000 GameOptimizer.exe         <- selfPid
cd::ProcessSnapshot MakeExtremeSnapshot() {
    cd::ProcessSnapshot s = MakeGameSnapshot();
    AddProc(s, 5000, 500, L"encoder.exe", 610, 0, 0.0);
    AddProc(s, 5001, 500, L"amd3dvcacheUser.exe", 620, 0, 0.0);
    AddProc(s, 5002, 500, L"", 630, 0, 0.0);
    AddProc(s, 9000, 500, L"GameOptimizer.exe", 640, 0, 0.0);
    AddProc(s, 6000, 9000, L"msedgewebview2.exe", 650, 0, 0.0);
    return s;
}

cd::Config MakeExtremeConfig(const cd::Topology& t, bool extreme) {
    cd::Config c = MakeEngineConfig(t, false);
    // The wildcard form DefaultExclusions() actually ships, so the test exercises the same
    // matcher the product does rather than a plain name.
    c.exclusions.push_back(L"amd3dvcache*");
    c.profiles[0].extremeMode = extreme;
    return c;
}

void Test_Z1_DefaultIsOff() {
    Case("Z1 extreme game mode is OFF by default - the struct, and every shipped profile");
    cd::Profile fresh;
    CHECK_EQ(fresh.extremeMode, false);

    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    CHECK(c.profiles.size() >= 1);
    for (size_t i = 0; i < c.profiles.size(); ++i) CHECK_EQ(c.profiles[i].extremeMode, false);
}

void Test_Z2_RoundTripAndOldConfigs() {
    Case("Z2 extreme_mode survives a save/load round trip, both ways");
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    // TWO profiles, because the claim is that true and false survive INDEPENDENTLY - one
    // profile could not tell a working round trip from a parser that returns a constant.
    // The default ships one since the All Games profile stopped shipping, so this is built.
    {
        cd::Profile second;
        second.name = L"Second";
        second.gameMask = t.defaultGameMask;
        second.heavyMask = t.defaultHeavyMask;
        c.profiles.push_back(second);
    }
    c.profiles[0].extremeMode = true;
    c.profiles[1].extremeMode = false;

    cd::Config back;
    std::wstring err;
    CHECK(cd::ParseConfig(cd::SerializeConfig(c), back, &err));
    CHECK_EQ(err, L"");
    CHECK(back.profiles.size() >= 2);
    CHECK_EQ(back.profiles[0].extremeMode, true);
    CHECK_EQ(back.profiles[1].extremeMode, false);

    Case("Z2b a config written before this feature loads with the mode OFF, not preserved raw");
    // The key is absent, so the model default has to survive AND the line must not come back
    // as an unknown key - a build that re-emitted it would be writing a setting it cannot read.
    const std::wstring old =
        L"[general]\r\nversion=1\r\n\r\n"
        L"[masks]\r\nCache=0 1 2 3\r\n\r\n"
        L"[profile:Overwatch]\r\nenabled=true\r\ngame=Overwatch.exe\r\ngame_mask=Cache\r\n";
    cd::Config oldCfg;
    CHECK(cd::ParseConfig(old, oldCfg, &err));
    CHECK_EQ((int)oldCfg.profiles.size(), 1);
    if (!oldCfg.profiles.empty()) CHECK_EQ(oldCfg.profiles[0].extremeMode, false);
    // BoolText writes 1/0, not true/false - checked against config.cpp, not assumed.
    CHECK(cd::SerializeConfig(oldCfg).find(L"extreme_mode=0") != std::wstring::npos);
}

void Test_Z3_SweepActiveNeedsAMaskToSweepOnto() {
    Case("Z3 the sweep is active only when it is ON and there is somewhere to sweep to");
    cd::Profile p;
    p.heavyMask = L"Freq";

    p.extremeMode = false;
    CHECK_EQ(cd::ExtremeSweepActive(p), false);

    p.extremeMode = true;
    CHECK_EQ(cd::ExtremeSweepActive(p), true);

    // An empty heavy mask resolves to "clear this process's assignment". Sweeping the whole
    // machine into a clear would strip assignments this program never made.
    p.heavyMask.clear();
    CHECK_EQ(cd::ExtremeSweepActive(p), false);
}

void Test_Z4_EligibilityIsSevenHardGates() {
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeExtremeConfig(t, true);
    std::set<DWORD> gameSet;
    gameSet.insert(1000);
    gameSet.insert(1001);
    std::set<DWORD> selfSet;
    selfSet.insert(9000);
    selfSet.insert(6000);

    Case("Z4 an ordinary process nobody named IS eligible - the positive control");
    CHECK_EQ(cd::ExtremeSweepEligible(c, 2000, L"notepad.exe", gameSet, selfSet), true);

    Case("Z4b pids 0 and 4 are never eligible, whatever they are called");
    CHECK_EQ(cd::ExtremeSweepEligible(c, 0, L"Idle", gameSet, selfSet), false);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 4, L"System", gameSet, selfSet), false);

    Case("Z4c a process whose name could not be read is REFUSED, not assumed safe");
    // The exclusion list is matched BY NAME, so no name means it could not be consulted.
    CHECK_EQ(cd::ExtremeSweepEligible(c, 5002, L"", gameSet, selfSet), false);

    Case("Z4d the game's own set is not swept - it is already on the game mask");
    CHECK_EQ(cd::ExtremeSweepEligible(c, 1000, L"Overwatch.exe", gameSet, selfSet), false);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 1001, L"OverwatchChild.exe", gameSet, selfSet), false);

    Case("Z4e our own subtree is not swept, by PARENTAGE and not by name");
    CHECK_EQ(cd::ExtremeSweepEligible(c, 9000, L"GameOptimizer.exe", gameSet, selfSet), false);
    // The name is not excluded and is not ours; only descent from selfPid saves it.
    CHECK_EQ(cd::ExtremeSweepEligible(c, 6000, L"msedgewebview2.exe", gameSet, selfSet), false);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 6001, L"msedgewebview2.exe", gameSet, selfSet), true);

    Case("Z4f an EXCLUDED name is never swept - exact, case-insensitive, and wildcard");
    CHECK_EQ(cd::ExtremeSweepEligible(c, 5000, L"encoder.exe", gameSet, selfSet), false);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 5000, L"ENCODER.EXE", gameSet, selfSet), false);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 5001, L"amd3dvcacheUser.exe", gameSet, selfSet), false);

    Case("Z4g THE OVERRIDE RULE 3 HAS IS NOT INHERITED: an excluded name on the heavy list "
         "is still refused by the sweep");
    // audiodg.exe is BOTH excluded and on this profile's heavy list. Rule 3 honours it,
    // because that entry is the user naming one process. A blanket sweep is not a user naming
    // anything, so the same name is refused here - and this predicate is not even shown the
    // heavy list, which is the structural reason it cannot be talked round.
    CHECK(std::find(c.profiles[0].heavy.begin(), c.profiles[0].heavy.end(),
                    std::wstring(L"audiodg.exe")) != c.profiles[0].heavy.end());
    CHECK_EQ(c.IsExcluded(L"audiodg.exe"), true);
    CHECK_EQ(cd::ExtremeSweepEligible(c, 3001, L"audiodg.exe", gameSet, selfSet), false);
}

void Test_Z5_TheRuleThroughComputeDesired() {
    cd::Topology t = MakeReference(false);
    cd::ProcessSnapshot s = MakeExtremeSnapshot();

    // Fixture sanity, so nothing below can pass for the wrong reason.
    CHECK(s.Find(2000) != nullptr);
    CHECK(s.Find(5002) != nullptr);

    Case("Z5 WITH THE MODE OFF, a process nobody named gets no mask at all");
    cd::Config off = MakeExtremeConfig(t, false);
    std::vector<std::wstring> sticky1;
    const cd::Profile* m1 = nullptr;
    std::map<DWORD, std::wstring> r1 = cd::ComputeDesired(s, off, 1000, sticky1, &m1, 9000);
    CHECK(m1 != nullptr);
    CHECK(!Has(r1, 2000));   // notepad
    CHECK(!Has(r1, 400));    // parent.exe
    CHECK(!Has(r1, 500));    // explorer.exe

    Case("Z5b WITH THE MODE ON, every one of them is on the heavy mask");
    cd::Config on = MakeExtremeConfig(t, true);
    std::vector<std::wstring> sticky2;
    const cd::Profile* m2 = nullptr;
    std::set<DWORD> autoPinned;
    std::set<DWORD> swept;
    std::map<DWORD, std::wstring> r2 =
        cd::ComputeDesired(s, on, 1000, sticky2, &m2, 9000, &autoPinned, &swept);
    CHECK(m2 != nullptr);
    CHECK_EQ(MaskOf(r2, 2000), L"Freq");
    CHECK_EQ(MaskOf(r2, 400), L"Freq");
    CHECK_EQ(MaskOf(r2, 500), L"Freq");

    Case("Z5c the sweep never overrides the GAME mask");
    CHECK_EQ(MaskOf(r2, 1000), L"Cache no SMT");
    CHECK_EQ(MaskOf(r2, 1001), L"Cache no SMT");
    CHECK_EQ(MaskOf(r2, 1002), L"Cache no SMT");

    Case("Z5d every exclusion is honoured by the sweep - default, user and wildcard");
    CHECK(!Has(r2, 5000));   // encoder.exe, excluded, on nobody's heavy list
    CHECK(!Has(r2, 5001));   // amd3dvcacheUser.exe, excluded by the trailing-* wildcard
    CHECK(!Has(r2, 1003));   // EasyAntiCheat.exe, an excluded descendant

    Case("Z5e a process whose name could not be read is left alone");
    CHECK(!Has(r2, 5002));

    Case("Z5f reserved pids and our own subtree are left alone");
    CHECK(!Has(r2, 0));
    CHECK(!Has(r2, 4));
    CHECK(!Has(r2, 9000));
    CHECK(!Has(r2, 6000));

    Case("Z5g an EXCLUDED name on the heavy list still gets rule 3's mask, and rule 4b has "
         "not quietly taken the credit");
    CHECK_EQ(MaskOf(r2, 3001), L"Freq");            // audiodg.exe, rule 3's override
    CHECK(swept.find(3001) == swept.end());         // ...but not attributed to the sweep
    CHECK(swept.find(2000) != swept.end());         // notepad.exe IS the sweep's
    CHECK(swept.find(1000) == swept.end());         // the game never is
    CHECK_EQ((int)autoPinned.size(), 0);            // autoPin is off in this fixture
}

void Test_Z6_NothingIsSweptWithoutAGoverningProfile() {
    Case("Z6 no profile matches -> the sweep does not run, however extreme the profile is");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeExtremeConfig(t, true);

    // The game is NOT running; everything else on the machine is.
    cd::ProcessSnapshot s;
    AddProc(s, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s, 2000, 500, L"notepad.exe", 120, 0, 0.0);
    AddProc(s, 3000, 500, L"OBS.exe", 150, 0, 0.0);

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = reinterpret_cast<const cd::Profile*>(0x1);
    std::set<DWORD> swept;
    std::map<DWORD, std::wstring> r =
        cd::ComputeDesired(s, c, 500, sticky, &matched, 0, nullptr, &swept);
    CHECK_EQ((int)r.size(), 0);
    CHECK(matched == nullptr);
    CHECK_EQ((int)swept.size(), 0);

    Case("Z6b a DISABLED extreme profile sweeps nothing either");
    cd::Config disabled = MakeExtremeConfig(t, true);
    disabled.profiles[0].enabled = false;
    cd::ProcessSnapshot live = MakeExtremeSnapshot();
    std::vector<std::wstring> sticky2;
    const cd::Profile* m2 = reinterpret_cast<const cd::Profile*>(0x1);
    std::map<DWORD, std::wstring> r2 = cd::ComputeDesired(live, disabled, 1000, sticky2, &m2);
    CHECK_EQ((int)r2.size(), 0);
    CHECK(m2 == nullptr);
}

void Test_Z7_AnEmptyHeavyMaskSweepsNothing() {
    Case("Z7 an empty heavy mask means no sweep - a blanket CLEAR is not what this mode is");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeExtremeConfig(t, true);
    c.profiles[0].heavyMask.clear();
    c.profiles[0].heavy.clear();   // or rule 3 would assign the same empty mask by itself

    cd::ProcessSnapshot s = MakeExtremeSnapshot();
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::set<DWORD> swept;
    std::map<DWORD, std::wstring> r =
        cd::ComputeDesired(s, c, 1000, sticky, &matched, 9000, nullptr, &swept);
    CHECK(matched != nullptr);
    CHECK_EQ((int)swept.size(), 0);
    CHECK(!Has(r, 2000));
    // The game is still governed; only the sweep stood down.
    CHECK_EQ(MaskOf(r, 1000), L"Cache no SMT");
}

void Test_Z8_AutoPinKeepsItsOwnLabel() {
    Case("Z8 a process rule 4 chose keeps the AUTO label and is not re-attributed to 4b");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeExtremeConfig(t, true);
    c.profiles[0].autoPin = true;

    cd::ProcessSnapshot s = MakeExtremeSnapshot();
    // notepad has been over the threshold long enough to qualify, and the game owns the
    // foreground, which is rule 4's other precondition.
    AddProc(s, 2000, 500, L"notepad.exe", 120, cd::kAutoPinDebounceTicks, 40.0);

    std::vector<std::wstring> sticky;
    const cd::Profile* matched = nullptr;
    std::set<DWORD> autoPinned;
    std::set<DWORD> swept;
    std::map<DWORD, std::wstring> r =
        cd::ComputeDesired(s, c, 1000, sticky, &matched, 9000, &autoPinned, &swept);

    CHECK_EQ(MaskOf(r, 2000), L"Freq");
    CHECK(autoPinned.find(2000) != autoPinned.end());   // rule 4 got there first
    CHECK(swept.find(2000) == swept.end());             // so rule 4b did not claim it
    // ...and the sweep still did its own job on everything rule 4 did not qualify.
    CHECK(swept.find(400) != swept.end());
    CHECK_EQ(MaskOf(r, 400), L"Freq");
}

void Test_Z9_AutoPinGreysUnderExtremeMode() {
    Case("Z9 extreme game mode greys the auto-pin group, and nothing else does");
    // The rule the operator asked for, 2026-09-09. Extreme mode moves EVERY non-game,
    // non-excluded process; auto-pin moves the subset above a threshold. One is a strict
    // subset of the other, so the whole auto-pin group is inert while extreme mode is on.
    //
    // IF THIS PREDICATE WERE INVERTED the percent field would be the only control on the
    // page you could type into while it had no effect, and the box that IS live would be
    // greyed. If it were "simplified" to always-true the grey-out silently disappears and
    // nothing else in this project can see that.
    CHECK_EQ(cd::AutoPinControlsEnabled(false), true);
    CHECK_EQ(cd::AutoPinControlsEnabled(true), false);

    Case("Z9b the greyed status line says WHY, and never says the setting is off");
    // The sentence is pinned. The failure it guards is subtle and was nearly shipped: the
    // obvious implementation reuses the existing "Auto-pin is off for this profile"
    // sentence, which tells the operator their stored setting has been cleared - and the
    // whole point of this change is that Profile::autoPin is NOT touched.
    const std::wstring superseded = cd::AutoPinSupersededByExtremeText();
    CHECK_EQ(superseded,
             L"Extreme game mode already covers every background process, so this rule cannot "
             L"add anything while it is on.");
    // AND IT NO LONGER SAYS "moves". Same reason as AC3d: this app requests an assignment
    // and Windows may refuse it, so no status line may state the move as a fact.
    CHECK(superseded.find(L"moves") == std::wstring::npos);
    CHECK(superseded.find(L"is off") == std::wstring::npos);
    CHECK(superseded.find(L"Extreme game mode") != std::wstring::npos);

    Case("Z9c THE TRIPWIRE - the new sentence promises no frame rate either");
    // The same list Z11d applies to the V-Cache row, applied here, with the same positive
    // control: a scan that cannot find a planted word proves nothing about a clean run.
    const wchar_t* banned[] = { L"fps", L"frame", L"faster", L"smoother", L"boost",
                                L"working", L"verified", L"guarantee" };
    {
        const std::wstring planted = cd::ToLower(std::wstring(L"this build is FASTER"));
        bool found = false;
        for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); ++b)
            if (planted.find(cd::ToLower(banned[b])) != std::wstring::npos) found = true;
        CHECK(found);
    }
    const std::wstring folded = cd::ToLower(superseded);
    for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); ++b)
        CHECK(folded.find(cd::ToLower(banned[b])) == std::wstring::npos);

    Case("Z9d GREY, NOT CLEAR - both flags survive a round trip with extreme mode on");
    // The window greys the auto-pin group; it must never uncheck it. That is a UI fact and
    // no headless test can click a check box, so this is the closest runnable guard: the two
    // settings are independent in the model and in the file, so a build that "simplified"
    // the grey-out into clearing autoPin has to break something visible here as well.
    cd::Topology t = MakeReference(false);
    cd::Config c = cd::DefaultConfig(t);
    c.profiles[0].autoPin = true;
    c.profiles[0].autoPinPercent = 12;
    c.profiles[0].extremeMode = true;
    cd::Config back;
    std::wstring err;
    CHECK(cd::ParseConfig(cd::SerializeConfig(c), back, &err));
    CHECK_EQ(err, L"");
    CHECK(back.profiles.size() >= 1);
    CHECK_EQ(back.profiles[0].autoPin, true);
    CHECK_EQ(back.profiles[0].extremeMode, true);
    CHECK_EQ(back.profiles[0].autoPinPercent, 12);
}

void Test_Z12_InfoIconTooltipWording() {
    Case("Z12 the two paragraphs that moved behind an (i) still say what they said");
    // PINNED, character for character, for the same reason AmdVCacheActiveWarningText is:
    // a tooltip is invisible to every other check in this project - no layout measures it,
    // no screenshot contains it unless someone hovers - so the string is the only thing
    // that can be asserted on at all.
    CHECK_EQ(cd::AutoPinInfoTipText(),
             L"While this game is in front, processes that stay above the threshold move to "
             L"the background mask until the game exits. The list above tags them AUTO.");
    CHECK_EQ(cd::ExtremeModeInfoTipText(),
             L"Not only the busy ones and not only the ones you named - everything except "
             L"the game and the exclusion list. Background apps can also be moved to another "
             L"GPU on the GPU Assignment tab.");

    Case("Z12b the AUTO legend survived the move - it lives nowhere else now");
    // The heavy list tags rows AUTO and the caption that explained the tag was this
    // paragraph. Drop the word here and the tag is unexplained anywhere in the product.
    CHECK(cd::AutoPinInfoTipText().find(L"AUTO") != std::wstring::npos);

    Case("Z12c the deleted sentence is GONE, not merely moved into the tooltip");
    // Operator instruction, 2026-09-09: delete "It needs the background mask unparked: use
    // \"Stop AMD's 3D V-Cache optimizer\" on the Setting page." A change that hid it behind
    // the icon instead of deleting it would look identical on screen and is exactly what
    // this checks.
    const std::wstring tip = cd::ExtremeModeInfoTipText();
    // POSITIVE CONTROL: the search must find these fragments when they really are present.
    const std::wstring planted =
        L"It needs the background mask unparked: use \"Stop AMD's 3D V-Cache optimizer\" on "
        L"the Setting page.";
    CHECK(planted.find(L"unparked") != std::wstring::npos);
    CHECK(planted.find(L"V-Cache") != std::wstring::npos);
    CHECK(planted.find(L"Setting page") != std::wstring::npos);
    CHECK(tip.find(L"unparked") == std::wstring::npos);
    CHECK(tip.find(L"V-Cache") == std::wstring::npos);
    CHECK(tip.find(L"Setting page") == std::wstring::npos);

    Case("Z12d THE TRIPWIRE - neither tooltip promises a frame rate");
    const wchar_t* banned[] = { L"fps", L"frame", L"faster", L"smoother", L"boost",
                                L"working", L"verified", L"guarantee" };
    std::vector<std::wstring> all;
    all.push_back(cd::ToLower(cd::AutoPinInfoTipText()));
    all.push_back(cd::ToLower(cd::ExtremeModeInfoTipText()));
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK(!all[i].empty());
        for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); ++b)
            CHECK(all[i].find(cd::ToLower(banned[b])) == std::wstring::npos);
    }

    Case("Z12e the (i) names the tab it points to, and carries no number");
    // Operator instruction 2026-09-12: "statement remain in circle i". The sentence is the
    // only place the page says where GPUs are assigned, so it must NAME that place - a
    // tooltip that says "some apps can move" without saying where leaves the user hunting.
    // v0.5.6: the place is the GPU Assignment TAB. The button the sentence used to name was
    // renamed and moved ABOVE this check box, which made both its name and its "below" false -
    // so the sentence carries no direction word and no "second GPU" (the tab also offers the
    // main GPU), and those are checked here so a later rewording cannot bring them back.
    // Read from the tab labels themselves, so renaming the tab without the sentence fails here.
    CHECK(tip.find(cd::kSettingsPageLabels[2]) != std::wstring::npos);
    CHECK(tip.find(L"below") == std::wstring::npos && tip.find(L"above") == std::wstring::npos &&
          tip.find(L"second GPU") == std::wstring::npos);
    // AND NO DIGIT. The string is registered once as a process-lifetime static, so a count
    // written into it would be frozen at whatever it was when the window first opened.
    // ONE assertion, not one per character. A per-character CHECK loop added ~175 entries to
    // the suite total for a single claim, which inflates the tally without adding coverage.
    CHECK(tip.find_first_of(L"0123456789") == std::wstring::npos);
}

void Test_Z11_VCacheActiveRow() {
    Case("Z11 the row's sentence is the operator's wording, character for character");
    // PINNED, not spot-checked. This is the whole visible product of the feature, it
    // interpolates nothing, and the layout height it is measured against is a constant only
    // as long as the string is. A reworded sentence must fail here and be re-measured.
    CHECK_EQ(cd::AmdVCacheActiveWarningText(),
             L"Warning - AMD 3D V-Cache is active. Game would not be fully optimized. "
             L"Please turn it off in Setting");

    Case("Z11b it points at the tab by its NEW name - 'Setting', not 'General'");
    // The tab was renamed in the same change. A warning that sends the user to a tab that
    // does not exist is worse than no warning, and this is the assertion that ties the two
    // halves of that change together.
    CHECK(cd::AmdVCacheActiveWarningText().find(L"in Setting") != std::wstring::npos);
    CHECK(cd::AmdVCacheActiveWarningText().find(L"General") == std::wstring::npos);

    Case("Z11c the row is driven by the AGENT, and EITHER live source can raise it");
    // The engine's flag is the stronger fact - it is only ever set while a game is actually
    // governed - but it is false at idle by construction, so on its own it would hide the
    // row from a user who opened Settings with no game running. The environment probe is
    // unconditional. Either one alone must raise the row.
    CHECK(cd::ShowAmdVCacheActiveWarning(true, false));    // agent up, no game governed
    CHECK(cd::ShowAmdVCacheActiveWarning(false, true));    // the watcher saw it mid-game
    CHECK(cd::ShowAmdVCacheActiveWarning(true, true));
    CHECK(!cd::ShowAmdVCacheActiveWarning(false, false));  // nothing running: NO row

    Case("Z11d THE TRIPWIRE - the new sentence promises no frame rate either");
    // The same list Z9c applies to the extreme-mode wording, applied to this row. It is run
    // over the string the product actually returns, not over a copy pasted into the test.
    const wchar_t* banned[] = { L"fps", L"frame", L"faster", L"smoother", L"boost",
                                L"working", L"verified", L"guarantee" };
    const std::wstring folded = cd::ToLower(cd::AmdVCacheActiveWarningText());
    // POSITIVE CONTROL: the scan must be able to find one of these when it is really there,
    // or a clean run proves nothing.
    {
        const std::wstring planted = cd::ToLower(std::wstring(L"this build is FASTER"));
        bool found = false;
        for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); ++b)
            if (planted.find(cd::ToLower(banned[b])) != std::wstring::npos) found = true;
        CHECK(found);
    }
    for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); ++b)
        CHECK(folded.find(cd::ToLower(banned[b])) == std::wstring::npos);

    Case("Z11e THIS ROW IS THE SURVIVOR, and it must stay on the page in plain sight");
    // 2026-09-09: the extreme-mode live parked line was DELETED and the two explanatory
    // paragraphs went behind an (i). This row did neither, deliberately - a warning behind
    // an icon is a warning that does not work - and it is now the only voice on the page
    // that names the optimizer. So it must not have drifted into being one of the tooltips,
    // and neither tooltip may have taken over its subject.
    const std::wstring vcache = cd::AmdVCacheActiveWarningText();
    CHECK(vcache != cd::AutoPinInfoTipText());
    CHECK(vcache != cd::ExtremeModeInfoTipText());
    CHECK(vcache.find(L"V-Cache") != std::wstring::npos);
    CHECK(cd::AutoPinInfoTipText().find(L"V-Cache") == std::wstring::npos);
    CHECK(cd::ExtremeModeInfoTipText().find(L"V-Cache") == std::wstring::npos);
}

void Test_Z10_BlockedLineGroupsAndCaps() {
    Case("Z10 nothing blocked - the promise sentence is unchanged");
    CHECK_EQ(cd::FormatBlockedProcessesLine(std::vector<std::wstring>(), 10),
             L"No processes are currently blocked. Game Optimizer runs unelevated on "
             L"purpose; anything it cannot touch will be named here rather than skipped "
             L"silently.");

    Case("Z10b many processes of one app are ONE name, and the two counts are both reported");
    std::vector<std::wstring> many;
    for (int i = 0; i < 40; ++i) many.push_back(L"svchost.exe");
    many.push_back(L"MsMpEng.exe");
    const std::wstring line = cd::FormatBlockedProcessesLine(many, 10);
    CHECK_EQ(line,
             L"Blocked (access denied), 41 processes in 2 apps: MsMpEng.exe, svchost.exe. "
             L"These are elevated or protected processes; no mask was applied to them.");

    Case("Z10c case and path do not create a second app");
    std::vector<std::wstring> mixed;
    mixed.push_back(L"SvcHost.exe");
    mixed.push_back(L"svchost.exe");
    mixed.push_back(L"C:\\Windows\\System32\\SVCHOST.EXE");
    const std::wstring one = cd::FormatBlockedProcessesLine(mixed, 10);
    CHECK(one.find(L"3 processes in 1 apps") != std::wstring::npos);
    CHECK(one.find(L"SvcHost.exe") != std::wstring::npos);   // the casing first seen

    Case("Z10d THE CAP - the line names ten apps and says how many it did not name");
    std::vector<std::wstring> lots;
    for (int i = 0; i < 26; ++i) {
        wchar_t b[32];
        swprintf_s(b, 32, L"app%02d.exe", i);
        lots.push_back(b);
    }
    const std::wstring capped = cd::FormatBlockedProcessesLine(lots, 10);
    CHECK(capped.find(L"26 processes in 26 apps") != std::wstring::npos);
    CHECK(capped.find(L"app00.exe") != std::wstring::npos);
    CHECK(capped.find(L"app09.exe") != std::wstring::npos);
    CHECK(capped.find(L"app10.exe") == std::wstring::npos);
    CHECK(capped.find(L"and 16 more") != std::wstring::npos);
    // The cap exists so a fixed-height control cannot clip the sentence. 460 characters is
    // three lines of Font::UiSmall at this page's own minimum width; the worst case measured
    // 307 and wrapped to two.
    CHECK((int)capped.size() < 460);

    Case("Z10e a cap of 0 means no cap - and a process with no readable name is still counted");
    std::vector<std::wstring> withBlank;
    withBlank.push_back(L"a.exe");
    withBlank.push_back(L"");
    withBlank.push_back(L"   ");
    const std::wstring blanks = cd::FormatBlockedProcessesLine(withBlank, 0);
    CHECK(blanks.find(L"3 processes in 2 apps") != std::wstring::npos);
    CHECK(blanks.find(L"(name unreadable)") != std::wstring::npos);
    CHECK(blanks.find(L"and ") == std::wstring::npos);   // no cap, so nothing was withheld
}

// ===========================================================================
// AC. EXTREME GAME MODE'S READOUT - what the blanket sweep actually moved.
//
// THE DEFECT, in the operator's words: "Extreme Mode should show what other application had
// been pinned into heavy mask. Now, it's 0 show." Rule 4b moves ~150 processes across ~60
// executables and, until this round, not one of them appeared anywhere in the settings
// window - so a working sweep and a switched-off one looked identical.
//
// Every expectation below is written from the header comments on ExtremeSweptExes,
// ExtremeSweptProcessCount (src\engine.h) and FormatExtremeSweptLine
// (src\settings_warning.h).
// ===========================================================================

cd::GovernedProcess Swept(DWORD pid, const wchar_t* name) {
    cd::GovernedProcess g;
    g.pid = pid;
    g.name = name;
    g.maskName = L"Freq";
    g.extremeSwept = true;
    return g;
}

void Test_AC1_SweptExesGroupCountAndSortByCount() {
    // CATCHES: a pid rule 4 chose being re-attributed to the blanket sweep (or the reverse);
    // two casings or a full path counting as two apps; an executable the user listed
    // themselves being reported as something the app chose; and - the one a naive
    // implementation gets wrong - the order degenerating to alphabetical, which would make
    // the display cap show four arbitrary apps instead of the four that moved the most.
    Case("AC1 the sweep groups to executables, counts each, and puts the biggest first");
    cd::EngineStatus st;
    st.governed.push_back(Swept(101, L"svchost.exe"));
    st.governed.push_back(Swept(102, L"SVCHOST.EXE"));                 // same app, other case
    st.governed.push_back(Swept(103, L"C:\\Windows\\svchost.exe"));    // same app, by path
    st.governed.push_back(Swept(104, L"chrome.exe"));
    st.governed.push_back(Swept(105, L"chrome.exe"));
    st.governed.push_back(Swept(106, L"notepad.exe"));
    st.governed.push_back(Gov(107, L"claude.exe", true));   // rule 4, NOT the sweep
    st.governed.push_back(Gov(108, L"Overwatch.exe", false));          // the game itself
    st.governed.push_back(Swept(109, L""));                            // unreadable name

    std::vector<std::wstring> none;
    std::vector<cd::SweptExe> v = cd::ExtremeSweptExes(st, none);
    CHECK_EQ((int)v.size(), 3);
    CHECK_EQ(v[0].name, L"svchost.exe");
    CHECK_EQ((int)v[0].count, 3);
    CHECK_EQ(v[1].name, L"chrome.exe");
    CHECK_EQ((int)v[1].count, 2);
    CHECK_EQ(v[2].name, L"notepad.exe");
    CHECK_EQ((int)v[2].count, 1);

    // The user's own heavy entry is their configuration and is not reported a second time as
    // something the sweep chose - by name or written as a path.
    std::vector<std::wstring> listed;
    listed.push_back(L"C:\\Program Files\\Chrome\\CHROME.EXE");
    std::vector<cd::SweptExe> v2 = cd::ExtremeSweptExes(st, listed);
    CHECK_EQ((int)v2.size(), 2);
    CHECK_EQ(v2[0].name, L"svchost.exe");
    CHECK_EQ(v2[1].name, L"notepad.exe");

    // Nothing swept is an empty list, never one blank row.
    cd::EngineStatus quiet;
    quiet.governed.push_back(Gov(201, L"Overwatch.exe", false));
    CHECK_EQ((int)cd::ExtremeSweptExes(quiet, none).size(), 0);
}

void Test_AC2_SweptProcessCountCountsProcessesNotApps() {
    // CATCHES: the total being summed from the GROUPED list, which silently drops every
    // process whose name could not be read - the app would then under-report what it did, on
    // exactly the protected processes the sweep is most likely to meet.
    Case("AC2 the process total counts processes, including the ones with no readable name");
    cd::EngineStatus st;
    st.governed.push_back(Swept(101, L"svchost.exe"));
    st.governed.push_back(Swept(102, L"svchost.exe"));
    st.governed.push_back(Swept(103, L""));                  // no name; still moved
    st.governed.push_back(Gov(104, L"claude.exe", true));    // rule 4, not the sweep
    CHECK_EQ((int)cd::ExtremeSweptProcessCount(st), 3);

    std::vector<std::wstring> none;
    CHECK_EQ((int)cd::ExtremeSweptExes(st, none).size(), 1);   // one APP, three PROCESSES
}

void Test_AC3_SweepLineReportsBothCountsAndTheMask() {
    // CATCHES: the sentence reporting one number twice (apps as processes or the reverse),
    // and the mask being dropped - "moved 5 processes" without saying where is not an answer.
    Case("AC3 the sweep sentence names the process count, the app count and the mask");
    std::vector<cd::SweptExe> exes;
    cd::SweptExe a; a.name = L"chrome.exe";  a.count = 3; exes.push_back(a);
    cd::SweptExe b; b.name = L"svchost.exe"; b.count = 2; exes.push_back(b);
    CHECK_EQ(cd::FormatExtremeSweptLine(exes, 5, 0, L"Freq", 0, 0),
             L"Extreme game mode also requested Freq for 5 processes in 2 apps: "
             L"chrome.exe x3, svchost.exe x2.");

    Case("AC3b one process of one app reads as singular in both places");
    std::vector<cd::SweptExe> one;
    cd::SweptExe c; c.name = L"a.exe"; c.count = 1; one.push_back(c);
    CHECK_EQ(cd::FormatExtremeSweptLine(one, 1, 0, L"Freq", 0, 0),
             L"Extreme game mode also requested Freq for 1 process in 1 app: a.exe x1.");

    Case("AC3c an unnamed mask is described rather than left blank");
    CHECK(cd::FormatExtremeSweptLine(one, 1, 0, L"", 0, 0)
              .find(L"the background mask") != std::wstring::npos);

    // ---- THE v0.4.3 DEFECT, PINNED --------------------------------------------------
    // CATCHES the sentence claiming EFFECTIVE PLACEMENT for assignments Windows refused.
    // This app runs unelevated on purpose and [M] ~41 processes on the operator's desktop
    // refuse the mask on every tick, so "moved N processes" over-stated the count and made a
    // claim about placement that applier.h's own measurements forbid. The refusals now
    // travel with the count, and the verb is what this app DID.
    Case("AC3d THE WORD 'moved' IS BANNED - the sentence says what was REQUESTED");
    const std::wstring refused = cd::FormatExtremeSweptLine(exes, 147, 41, L"Freq", 0, 0);
    // The refusal clause CLOSES the sentence; it does not introduce the app list. Test_AC7
    // is what pins that placement - see there for the defect it was written from.
    CHECK_EQ(refused,
             L"Extreme game mode also requested Freq for 147 processes in 2 apps: "
             L"chrome.exe x3, svchost.exe x2; 41 were not applied.");
    // POSITIVE CONTROL: the scan must find the banned word when it really is there.
    CHECK(std::wstring(L"also moved 5 processes").find(L"moved") != std::wstring::npos);
    CHECK(refused.find(L"moved") == std::wstring::npos);
    CHECK(cd::FormatExtremeSweptLine(exes, 5, 0, L"Freq", 0, 0).find(L"moved") ==
          std::wstring::npos);

    Case("AC3e no refusals means no refusal clause - the sentence does not carry a zero");
    CHECK(cd::FormatExtremeSweptLine(exes, 5, 0, L"Freq", 0, 0).find(L"not applied") ==
          std::wstring::npos);
    CHECK(cd::FormatExtremeSweptLine(exes, 5, 1, L"Freq", 0, 0).find(L"1 were not applied") !=
          std::wstring::npos);

    Case("AC3f every request refused still reports the request, and never a negative");
    const std::wstring allRefused = cd::FormatExtremeSweptLine(exes, 5, 5, L"Freq", 0, 0);
    CHECK(allRefused.find(L"for 5 processes") != std::wstring::npos);
    CHECK(allRefused.find(L"5 were not applied") != std::wstring::npos);
    // A caller handing in more refusals than requests is clamped, not printed.
    CHECK(cd::FormatExtremeSweptLine(exes, 5, 9, L"Freq", 0, 0).find(L"9 were not applied") ==
          std::wstring::npos);
}

void Test_AC6_SweptNotAppliedCountsTheSettersOwnAnswer() {
    // CATCHES the arithmetic behind AC3d: a refusal count derived from rule INTENT rather
    // than from the setter's result. Every row below is extremeSwept - the rule chose all of
    // them - and only the applyResult tells them apart.
    Case("AC6 the refusal count comes from applyResult, never from the sweep flag");
    cd::EngineStatus st;
    cd::GovernedProcess ok1 = Swept(101, L"a.exe"); ok1.applyResult = cd::ApplyResult::Ok;
    cd::GovernedProcess ok2 = Swept(102, L"a.exe"); ok2.applyResult = cd::ApplyResult::Ok;
    cd::GovernedProcess den = Swept(103, L"b.exe");
    den.applyResult = cd::ApplyResult::AccessDenied;
    den.blocked = true;
    cd::GovernedProcess bad = Swept(104, L"c.exe");
    bad.applyResult = cd::ApplyResult::InvalidParameter;
    bad.blocked = true;
    // Held back because its recovery record could not be written: never attempted, so it
    // carries the default OtherError and blocked is still FALSE. A count keyed on `blocked`
    // would miss this row, which is why the helper reads applyResult.
    cd::GovernedProcess held = Swept(105, L"d.exe");
    st.governed.push_back(ok1);
    st.governed.push_back(ok2);
    st.governed.push_back(den);
    st.governed.push_back(bad);
    st.governed.push_back(held);
    // A rule-4 pid that was refused is NOT the sweep's business and must not be counted.
    cd::GovernedProcess other = Gov(106, L"e.exe", true);
    other.applyResult = cd::ApplyResult::AccessDenied;
    st.governed.push_back(other);

    CHECK_EQ((int)cd::ExtremeSweptProcessCount(st), 5);
    CHECK_EQ((int)cd::ExtremeSweptNotAppliedCount(st), 3);

    Case("AC6b everything accepted reports no refusals at all");
    cd::EngineStatus clean;
    clean.governed.push_back(ok1);
    clean.governed.push_back(ok2);
    CHECK_EQ((int)cd::ExtremeSweptProcessCount(clean), 2);
    CHECK_EQ((int)cd::ExtremeSweptNotAppliedCount(clean), 0);
}

// THE v0.4.4 DEFECT, PINNED. A test that passes both before and after a change proves nothing
// about the change, so this one was run against BOTH: [M] compiled against the pre-change
// formatter (git c59a85a) on 2026-09-09, the assertions here and in AC5c fail 11 of 16 - and
// the one that must NOT fail, the positive control below, passes on both. What v0.4.4 rendered
// for AC7's own inputs was
//   "Extreme game mode also requested Freq for 147 processes in 2 apps; 41 were not applied:
//    chrome.exe x3, svchost.exe x2."
//
// WHAT WENT WRONG, and it was one `+ tail` in the wrong operand: the refusal clause was
// concatenated into the HEAD, immediately in front of the colon that introduces the app list.
// A colon binds what follows it to the number in front of it, so the sentence offered the
// four named apps as the breakdown of the 44 refusals. [M] On the operator's own screen they
// were nothing of the kind - 19+16+14+12 = 61 exceeds 44 outright; the 4 named plus "86 more
// apps" reconstruct the 90 apps of the 197 REQUESTS, not the 38 apps of the refusals; and
// chrome.exe, named in the clause, is absent from the Setting page's own alphabetical blocked
// list where it would sort between choice.exe and ChtIME.exe. All four also wore an amber
// SWEPT badge 300 px up the same page.
//
// IF THE LOGIC BREAKS AGAIN this test says so in the specific way that matters: not "the
// string changed" but "a number is once more offering a list of apps as its own contents".
void Test_AC7_RefusalCountNeverIntroducesTheAppList() {
    Case("AC7 the refusal count is the LAST clause and introduces nothing");
    std::vector<cd::SweptExe> exes;
    cd::SweptExe a; a.name = L"chrome.exe";  a.count = 3; exes.push_back(a);
    cd::SweptExe b; b.name = L"svchost.exe"; b.count = 2; exes.push_back(b);
    const std::wstring s = cd::FormatExtremeSweptLine(exes, 147, 41, L"Freq", 0, 0);

    // 1. NO COLON MAY FOLLOW THE REFUSAL CLAUSE. The colon IS the binding, so its absence
    //    there is the property, and an executable name behind it is the observed symptom.
    CHECK(s.find(L"were not applied:") == std::wstring::npos);
    CHECK(s.find(L"were not applied: chrome.exe") == std::wstring::npos);
    // POSITIVE CONTROL: the scan really can see that shape - it is v0.4.4's own output.
    CHECK(std::wstring(L"in 2 apps; 41 were not applied: chrome.exe x3, svchost.exe x2.")
              .find(L"were not applied: chrome.exe") != std::wstring::npos);

    // 2. THE LIST COMES FIRST, THE REFUSAL CLAUSE LAST. Ordering, not spelling: a rewording
    //    that keeps the words and restores the old order still fails here.
    const size_t firstName = s.find(L"chrome.exe x3");
    const size_t lastName  = s.find(L"svchost.exe x2");
    const size_t refusal   = s.find(L"41 were not applied");
    CHECK(firstName != std::wstring::npos);
    CHECK(lastName != std::wstring::npos);
    CHECK(refusal != std::wstring::npos);
    CHECK(firstName < refusal);
    CHECK(lastName < refusal);
    // 3. And the colon that DOES introduce the list belongs to the app count.
    CHECK(s.find(L"in 2 apps: chrome.exe x3") != std::wstring::npos);
    // 4. Nothing trails the refusal clause but the full stop.
    CHECK_EQ(s.substr(s.size() - 22), std::wstring(L"; 41 were not applied."));

    Case("AC7b a TRUNCATED list and a refusal count in one sentence stay separate");
    // CATCHES the interaction the move creates: "and N more apps" is appended to the LIST and
    // the refusal clause to the SENTENCE, so a naive fix that appends the tail before the
    // truncation clause would read "... x27; 44 were not applied and 26 more apps."
    std::vector<cd::SweptExe> lots;
    for (int i = 0; i < 30; ++i) {
        wchar_t nm[32];
        swprintf_s(nm, 32, L"app%02d.exe", i);
        cd::SweptExe e;
        e.name = nm;
        e.count = static_cast<size_t>(30 - i);
        lots.push_back(e);
    }
    const std::wstring cut = cd::FormatExtremeSweptLine(lots, 465, 44, L"Freq", 4, 85);
    CHECK_EQ(cut,
             L"Extreme game mode also requested Freq for 465 processes in 30 apps: "
             L"app00.exe x30, app01.exe x29, app02.exe x28, app03.exe x27 and 26 more apps; "
             L"44 were not applied.");
    // The truncation clause stays welded to the list it truncates.
    CHECK(cut.find(L"x27 and 26 more apps;") != std::wstring::npos);
    CHECK(cut.find(L"and 26 more apps") < cut.find(L"44 were not applied"));
    CHECK(cut.find(L"were not applied:") == std::wstring::npos);
    // And the whole thing still fits the row it has to fit - see AC4e for that budget.
    CHECK(cut.size() <= 192);

    Case("AC7c with nothing refused the sentence simply ends - no dangling separator");
    const std::wstring none = cd::FormatExtremeSweptLine(exes, 5, 0, L"Freq", 0, 0);
    CHECK_EQ(none,
             L"Extreme game mode also requested Freq for 5 processes in 2 apps: "
             L"chrome.exe x3, svchost.exe x2.");
    CHECK(none.find(L";") == std::wstring::npos);
    CHECK(none.find(L"..") == std::wstring::npos);
}

void Test_AC4_SweepLineCapsByCountAndByLength() {
    // CATCHES THE CLIPPING DEFECT this whole feature had to avoid: a fixed-height STATIC and
    // an unbounded list. A cap on the NUMBER of names is not enough on its own - six names of
    // forty characters overflows exactly as sixty short ones do - so both caps are asserted,
    // and so is the "and N more apps" tail without which the cap would be a silent omission.
    Case("AC4 the count cap names the biggest apps and says how many it did not name");
    std::vector<cd::SweptExe> lots;
    for (int i = 0; i < 30; ++i) {
        wchar_t nm[32];
        swprintf_s(nm, 32, L"app%02d.exe", i);
        cd::SweptExe e;
        e.name = nm;
        e.count = static_cast<size_t>(30 - i);   // strictly descending, so the order is fixed
        lots.push_back(e);
    }
    const std::wstring capped = cd::FormatExtremeSweptLine(lots, 465, 0, L"Freq", 4, 85);
    CHECK(capped.find(L"465 processes in 30 apps") != std::wstring::npos);
    CHECK(capped.find(L"app00.exe x30") != std::wstring::npos);
    CHECK(capped.find(L"app03.exe x27") != std::wstring::npos);
    CHECK(capped.find(L"app04.exe") == std::wstring::npos);
    CHECK(capped.find(L"and 26 more apps") != std::wstring::npos);

    Case("AC4b THE LENGTH CAP - four long names are cut before four short ones would be");
    std::vector<cd::SweptExe> longNames;
    for (int i = 0; i < 4; ++i) {
        wchar_t nm[64];
        swprintf_s(nm, 64, L"averyveryverylongexecutablename%02d.exe", i);
        cd::SweptExe e;
        e.name = nm;
        e.count = static_cast<size_t>(9 - i);
        longNames.push_back(e);
    }
    // Same maxNamed as above, so anything that fails here failed on LENGTH alone.
    const std::wstring cut = cd::FormatExtremeSweptLine(longNames, 30, 0, L"Freq", 4, 85);
    CHECK(cut.find(L"averyveryverylongexecutablename00.exe x9") != std::wstring::npos);
    CHECK(cut.find(L"averyveryverylongexecutablename01.exe x8") != std::wstring::npos);
    CHECK(cut.find(L"averyveryverylongexecutablename02.exe") == std::wstring::npos);
    CHECK(cut.find(L"and 2 more apps") != std::wstring::npos);

    Case("AC4c ONE name is always shown, even when it alone exceeds the budget");
    std::vector<cd::SweptExe> huge;
    cd::SweptExe big;
    big.name = std::wstring(200, L'x') + L".exe";
    big.count = 4;
    huge.push_back(big);
    cd::SweptExe small; small.name = L"b.exe"; small.count = 1; huge.push_back(small);
    const std::wstring forced = cd::FormatExtremeSweptLine(huge, 5, 0, L"Freq", 4, 85);
    CHECK(forced.find(big.name) != std::wstring::npos);
    CHECK(forced.find(L"b.exe") == std::wstring::npos);
    CHECK(forced.find(L"and 1 more app") != std::wstring::npos);

    Case("AC4d a cap of 0 on either axis means no cap");
    CHECK(cd::FormatExtremeSweptLine(lots, 465, 0, L"Freq", 0, 0).find(L"app29.exe x1") !=
          std::wstring::npos);

    Case("AC4e THE WHOLE SENTENCE STILL FITS THE ROW - the budget is re-derived, not copied");
    // CATCHES the failure this budget exists for, in the direction a REWORDING breaks it: the
    // row is a fixed Dp(48), which is [M] three lines of Font::UiSmall at 96 dpi and about 192
    // characters at that column width. kExtremeListChars caps only the LIST, so a longer fixed
    // head silently eats the margin and the promise is clipped instead of truncated.
    //
    // [M] v0.4.3's head was 63 characters and the budget shipped as 114. v0.4.4's honest
    // sentence carried its refusal clause inside the head: 89 + 85 + 18 (" and 58 more
    // apps.") = 192. Moving that clause behind the list splits the same 107 the other way -
    // head 68, and a tail of " and 58 more apps" 17 + "; 41 were not applied" 21 + "." 1 =
    // 39, so 68 + 85 + 39 = 192 and the constant did not have to move. THIS ASSERTS THE SUM
    // AND NOT THE CONSTANT, which is the only reason a pure re-ordering could be checked
    // rather than assumed - and the next rewording, which will not be pure, has to re-derive
    // it too. The `worst` case below carries a refusal count precisely so the tail it now
    // owns is inside the measurement.
    std::vector<cd::SweptExe> typical;
    for (int i = 0; i < 62; ++i) {
        wchar_t nm[64];
        swprintf_s(nm, 64, L"msedgewebview%02d.exe", i);   // 22 chars with its " x12" suffix
        cd::SweptExe e;
        e.name = nm;
        e.count = 12;
        typical.push_back(e);
    }
    const std::wstring worst =
        cd::FormatExtremeSweptLine(typical, 147, 41, L"Freq", 4, 85);
    CHECK(worst.size() <= 192);
    // POSITIVE CONTROL: the same inputs with no cap at all blow straight past the row.
    CHECK(cd::FormatExtremeSweptLine(typical, 147, 41, L"Freq", 0, 0).size() > 192);
}

void Test_AC5_SweepLineIsSilentWithNothingToSay() {
    // CATCHES: a blank row reserving height on every profile that has extreme mode off, and
    // the opposite mistake - a sentence claiming "0 apps" while the sweep is plainly working
    // because every process it moved was one the user had already named.
    Case("AC5 nothing swept produces NO sentence at all, so the row takes no height");
    std::vector<cd::SweptExe> none;
    CHECK_EQ(cd::FormatExtremeSweptLine(none, 0, 0, L"Freq", 4, 85), L"");

    Case("AC5b processes swept but every app already listed still reports the count");
    const std::wstring all = cd::FormatExtremeSweptLine(none, 12, 0, L"Freq", 4, 85);
    CHECK_EQ(all,
             L"Extreme game mode also requested Freq for 12 processes, all of them apps you "
             L"already listed above.");
    CHECK(all.find(L"12 processes") != std::wstring::npos);
    CHECK(all.find(L"already listed above") != std::wstring::npos);
    CHECK(all.find(L"0 apps") == std::wstring::npos);

    Case("AC5c that branch reports refusals too - it is not a place the count can hide");
    const std::wstring allRefused = cd::FormatExtremeSweptLine(none, 12, 4, L"Freq", 4, 85);
    // AND "all of them" QUALIFIES THE TWELVE REQUESTS, NOT THE FOUR REFUSALS. v0.4.4 built
    // this branch as head + tail + ", all of them apps you already listed above.", which
    // reads "; 4 were not applied, all of them apps you already listed above" - the same
    // mis-binding Test_AC7 pins for the list branch, in the branch that has no list.
    CHECK_EQ(allRefused,
             L"Extreme game mode also requested Freq for 12 processes, all of them apps you "
             L"already listed above; 4 were not applied.");
    CHECK(allRefused.find(L"12 processes") != std::wstring::npos);
    CHECK(allRefused.find(L"4 were not applied") != std::wstring::npos);
    CHECK(allRefused.find(L"already listed above") != std::wstring::npos);
    CHECK(allRefused.find(L"moved") == std::wstring::npos);
    // The refusal clause is last, so nothing trails it that could attach to it.
    CHECK(allRefused.find(L"not applied") > allRefused.find(L"already listed above"));
}

// ===========================================================================
// AD. THE PROFILES PANEL FOLLOWS THE PROFILE THAT IS ACTUALLY GOVERNING.
//
// Operator request: "The screen of profile should always show which profile currently using
// ... if I'm playing Overwatch it should show Overwatch Profile Panel. Then I swap to
// Palworld, then it should auto swap to Palworld profile panel."
//
// Written from the header comment on ShouldFollowGoverningProfile in
// src\settings_warning.h. The rule is a pure predicate precisely so it can be driven here:
// the alternative - proving it by clicking a live window - is the thing this project has
// repeatedly found it cannot check.
// ===========================================================================

cd::ProfileFollowInputs Following(int governing, int selected) {
    cd::ProfileFollowInputs in;
    in.haveGoverning = governing >= 0;
    in.governingIndex = governing;
    in.selectedIndex = selected;
    return in;
}

void Test_AD1_ThePanelFollowsTheGameInFront() {
    // CATCHES: the follow never firing at all, which is the bug being fixed, and the follow
    // firing when the panel is ALREADY on the governing profile - which would re-load the
    // editor once a second and reset the heavy list's scroll position under the operator.
    Case("AD1 a governing profile that is not the one on screen is followed");
    CHECK(cd::ShouldFollowGoverningProfile(Following(2, 0)));

    Case("AD1b the panel already showing it does nothing at all");
    CHECK(!cd::ShouldFollowGoverningProfile(Following(2, 2)));
}

void Test_AD2_FollowNeverStealsAnEditInProgress() {
    // CATCHES the failure that would be WORSE than the bug: the panel swapping itself out
    // from under a half-typed executable name, an open mask dropdown, or a rename prompt.
    // Each blocker is asserted on its own, so removing any one of them fails here rather than
    // being masked by the other two.
    Case("AD2 typing in a field on this page suppresses the follow");
    cd::ProfileFollowInputs typing = Following(2, 0);
    typing.editingFocus = true;
    CHECK(!cd::ShouldFollowGoverningProfile(typing));

    Case("AD2b an open combo dropdown suppresses it");
    cd::ProfileFollowInputs dropped = Following(2, 0);
    dropped.dropdownOpen = true;
    CHECK(!cd::ShouldFollowGoverningProfile(dropped));

    Case("AD2c a modal prompt over the window suppresses it");
    cd::ProfileFollowInputs modal = Following(2, 0);
    modal.modalUp = true;
    CHECK(!cd::ShouldFollowGoverningProfile(modal));

    Case("AD2d and every one of them resumes the moment it clears");
    CHECK(cd::ShouldFollowGoverningProfile(Following(2, 0)));
}

void Test_AD3_TheOperatorsOwnChoiceIsHonoured() {
    // CATCHES: the panel yanking itself back to the governing profile a second after the
    // operator deliberately clicked a different one to look at it. The latch is released by
    // the engine changing which profile it governs - the operator swapping games, which is
    // the event they asked to be followed - and NOT by a timer, so the release is asserted
    // as a separate case rather than assumed.
    Case("AD3 a selection the operator made themselves is not taken away");
    cd::ProfileFollowInputs chosen = Following(2, 0);
    chosen.userChoseSelection = true;
    CHECK(!cd::ShouldFollowGoverningProfile(chosen));

    Case("AD3b once the governing profile changes the latch is released and it follows");
    cd::ProfileFollowInputs released = Following(2, 0);
    released.userChoseSelection = false;   // what the window does on a change of profileName
    CHECK(cd::ShouldFollowGoverningProfile(released));
}

void Test_AD4_NothingGoverningNeverMovesTheSelection() {
    // CATCHES: an idle or paused engine dragging the panel onto profile 0, or a -1 index
    // reaching the selection and indexing the profile vector out of range.
    Case("AD4 no game running means the panel stays exactly where the operator left it");
    CHECK(!cd::ShouldFollowGoverningProfile(Following(-1, 0)));

    Case("AD4b a haveGoverning flag with no index is refused too");
    cd::ProfileFollowInputs half = Following(-1, 0);
    half.haveGoverning = true;             // inconsistent input, refused rather than trusted
    CHECK(!cd::ShouldFollowGoverningProfile(half));
}


void Test_AD5_TheFollowPicksTheProfileTheEngineNAMES() {
    // CATCHES the latent defect the v0.4.3 review found: GoverningProfileIndex took the FIRST
    // profile that StatusDescribesProfile accepted, and that helper falls back to matching the
    // GAME EXECUTABLE so a renamed profile is still recognised. With two profiles naming one
    // executable the fallback fires on whichever comes first in the file, so an earlier -
    // possibly disabled - profile takes the NOW pill and the panel-follow while the engine is
    // governing the later one.
    //
    // [M] Not reachable in the operator's current config, where every profile has a distinct
    // game=. It is one duplicated executable away from being reachable.
    Case("AD5 an exact name match ANYWHERE beats an executable fallback that comes first");
    std::vector<bool> exact(3, false), fallback(3, false);
    fallback[0] = true;    // profile A: same exe, wrong profile
    exact[2] = true;       // profile B: the one the engine actually names
    CHECK_EQ(cd::PickGoverningProfile(exact, fallback), 2);

    Case("AD5b with no exact match a SINGLE fallback is still used - renames keep working");
    std::vector<bool> noExact(3, false), oneFallback(3, false);
    oneFallback[1] = true;
    CHECK_EQ(cd::PickGoverningProfile(noExact, oneFallback), 1);

    Case("AD5c two fallbacks say nothing about which is governing, so nothing is chosen");
    std::vector<bool> two(3, false);
    two[0] = true;
    two[2] = true;
    CHECK_EQ(cd::PickGoverningProfile(noExact, two), -1);

    Case("AD5d nothing matching at all is -1, which leaves the operator's selection alone");
    CHECK_EQ(cd::PickGoverningProfile(noExact, noExact), -1);

    Case("AD5e the first exact match wins even when a later one also matches");
    std::vector<bool> twoExact(3, false);
    twoExact[0] = true;
    twoExact[1] = true;
    CHECK_EQ(cd::PickGoverningProfile(twoExact, noExact), 0);
}

// ===========================================================================
// AE. THE RESTORE JOURNAL IS A PROMISE, NOT AN ORDERING.
//
// THE BLOCKER, found by a three-vendor review of the SHIPPED v0.4.3: the engine wrote the
// journal BEFORE applying - which establishes the ORDER - and then applied unconditionally,
// because JournalAddMany returned void and a failed write was only logged. A full disk, a
// denied ACL or a failed flush therefore still pinned every process in the batch, with
// nothing on disk to undo them. After an unclean exit those processes stay on half the
// machine and no launch can find them.
//
// The defect had survived since v0.3.4 UNDER TWO COMMENTS THAT PROMISED THE OPPOSITE, and
// the reason it survived is that not one of these paths could be reached from a test: the
// harness had no call to JournalAddMany, ApplyCpuSets or ClearCpuSets anywhere in it.
//
// So this section drives the real code against REAL FAULTS - a directory that does not
// exist, a file too large to read, a creation time that does not match the live process -
// rather than mocking them. Every case below FAILS against v0.4.3.
// ===========================================================================

std::wstring TempFilePath(const wchar_t* leaf) {
    wchar_t buf[MAX_PATH] = { 0 };
    const DWORD n = ::GetTempPathW(MAX_PATH, buf);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".\\");
    if (!dir.empty() && dir[dir.size() - 1] != L'\\') dir += L'\\';
    return dir + leaf;
}

std::wstring TempDirNoSlash() {
    wchar_t buf[MAX_PATH] = { 0 };
    const DWORD n = ::GetTempPathW(MAX_PATH, buf);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring(L".");
    while (!dir.empty() && (dir[dir.size() - 1] == L'\\' || dir[dir.size() - 1] == L'/'))
        dir.erase(dir.size() - 1);
    return dir;
}

cd::JournalEntry Entry(DWORD pid, ULONGLONG created, const wchar_t* name) {
    cd::JournalEntry e;
    e.pid = pid;
    e.creationTime = created;
    e.name = name;
    return e;
}

bool FileSizeOf(const std::wstring& path, LONGLONG& out) {
    out = -1;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    const bool ok = ::GetFileSizeEx(h, &sz) != 0;
    ::CloseHandle(h);
    if (ok) out = sz.QuadPart;
    return ok;
}

void Test_AE1_TheFourJournalRules() {
    // CATCHES all three bookkeeping defects the review named, at the one place each rule is
    // now written. Each assertion below is a row of the review's own counterexample table.
    Case("AE1 a pid with no record on disk needs one - even when the map already knows it");
    // The F2 defect in one line: the FIRST assignment failed, which leaves a map entry
    // (blocked) and REMOVES the journal entry. "Is this pid new to the map" answered NO, so
    // a later attempt with a different mask succeeded and was never journalled.
    CHECK_EQ(cd::NeedsRecoveryRecord(false, false), true);   // never seen
    CHECK_EQ(cd::NeedsRecoveryRecord(true, false), true);    // seen, record was taken away
    CHECK_EQ(cd::NeedsRecoveryRecord(true, true), false);    // seen and recorded

    Case("AE2 THE BLOCKER - nothing may reach the setter without a durable record");
    CHECK_EQ(cd::MayApplyAssignment(true, false), false);    // record needed, write failed
    CHECK_EQ(cd::MayApplyAssignment(true, true), true);      // record needed, write landed
    // A pid that is already recorded is not held hostage by an unrelated failed batch: its
    // recovery record is on disk from an earlier tick and is still valid.
    CHECK_EQ(cd::MayApplyAssignment(false, false), true);
    CHECK_EQ(cd::MayApplyAssignment(false, true), true);

    Case("AE3 a pid is cleared because something LANDED, not because the last try worked");
    // A process moved to Cache and then refused a move to Freq is still on Cache. The old
    // test - "was the last attempt blocked" - skipped its clear entirely.
    CHECK_EQ(cd::NeedsClearOnLeaving(true), true);
    CHECK_EQ(cd::NeedsClearOnLeaving(false), false);

    Case("AE4 a recovery record only leaves the file when there is nothing left to recover");
    CHECK_EQ(cd::MayDropRecoveryRecord(false, cd::ApplyResult::OtherError), true);
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::Ok), true);
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::Gone), true);
    // THE ONE THAT SHIPPED WRONG: an ordinary clear failure was logged and the record and
    // journal entry were removed anyway, leaving a live process masked with nothing that
    // could undo it.
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::AccessDenied), false);
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::InvalidParameter), false);
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::OtherError), false);
}

void Test_AE5_MissingIsNotUnreadable() {
    // CATCHES the third bullet of the blocker: applier.cpp turned ANY journal-read failure
    // into an EMPTY journal, so one unreadable read followed by an ordinary add rewrote the
    // file with nothing but the new entry - every existing recovery record dropped, by the
    // code that exists to preserve them. Nothing could tell the two failures apart because
    // the reader returned bool.
    Case("AE5 a missing file and an unreadable one are different answers");
    const std::wstring missing = TempFilePath(L"go_test_definitely_absent_8271.txt");
    ::DeleteFileW(missing.c_str());
    std::wstring text;
    CHECK_EQ(cd::ReadFileUtf8Checked(missing, text), cd::FileReadResult::Missing);

    // A DIRECTORY exists and cannot be opened as a file. Content is not there to be lost,
    // but the classification is the one that matters: never Missing.
    CHECK_EQ(cd::ReadFileUtf8Checked(TempDirNoSlash(), text), cd::FileReadResult::Unreadable);

    Case("AE5b a readable file still round-trips, so the classifier is not just refusing");
    const std::wstring good = TempFilePath(L"go_test_readable_8271.txt");
    CHECK_EQ(cd::WriteFileUtf8Atomic(good, L"hello\nworld\n"), true);
    CHECK_EQ(cd::ReadFileUtf8Checked(good, text), cd::FileReadResult::Ok);
    CHECK_EQ(text, L"hello\nworld\n");

    Case("AE5c POSITIVE CONTROL - the two-valued reader genuinely cannot tell them apart");
    // This is what every caller used to see, and it is why the distinction had to be made
    // at the reader rather than guessed at by the journal.
    CHECK_EQ(cd::ReadFileUtf8(missing, text), false);
    CHECK_EQ(cd::ReadFileUtf8(TempDirNoSlash(), text), false);

    Case("AE5d a write that cannot land returns false and creates nothing");
    const std::wstring nowhere =
        TempFilePath(L"go_test_no_such_dir_8271\\applied.journal");
    CHECK_EQ(cd::WriteFileUtf8Atomic(nowhere, L"x"), false);
    LONGLONG size = 0;
    CHECK_EQ(FileSizeOf(nowhere, size), false);
    // ...and the temp file it writes through is cleaned up rather than left behind.
    CHECK_EQ(FileSizeOf(nowhere + L".tmp", size), false);

    ::DeleteFileW(good.c_str());
}

void Test_AE6_AFailedJournalWriteIsReported() {
    // CATCHES THE BLOCKER ITSELF at the applier boundary. JournalAddMany returned void, so
    // the engine could not have obeyed a failure even if it had wanted to. The fault here is
    // real, not mocked: the journal is pointed at a path whose DIRECTORY does not exist, so
    // CreateFileW on the atomic write's temp file genuinely fails.
    Case("AE6 a journal write that cannot land is reported as a failure");
    const std::wstring nowhere =
        TempFilePath(L"go_test_no_such_dir_8271\\applied.journal");
    cd::JournalSetPathForTests(nowhere);

    std::vector<cd::JournalEntry> add;
    add.push_back(Entry(4242, 777777ull, L"probe.exe"));
    CHECK_EQ(cd::JournalAddMany(add), false);
    CHECK_EQ(cd::JournalAdd(4243, 777778ull, L"probe2.exe"), false);
    CHECK_EQ((int)cd::JournalRead().size(), 0);

    Case("AE6b a journal write that CAN land is reported as a success and round-trips");
    // POSITIVE CONTROL for AE6: without it a function that returned false unconditionally
    // would pass the case above.
    const std::wstring good = TempFilePath(L"go_test_journal_8271.txt");
    ::DeleteFileW(good.c_str());
    cd::JournalSetPathForTests(good);

    CHECK_EQ(cd::JournalAddMany(add), true);
    std::vector<cd::JournalEntry> back = cd::JournalRead();
    CHECK_EQ((int)back.size(), 1);
    CHECK_EQ(back[0].pid, (DWORD)4242);
    CHECK_EQ(back[0].creationTime, 777777ull);
    CHECK_EQ(back[0].name, std::wstring(L"probe.exe"));

    Case("AE6c re-adding a pid already on disk is a no-op that reports SUCCESS");
    // The claim is about the on-disk STATE, not about whether a write happened. A false here
    // would hold back an assignment whose record is already safe.
    CHECK_EQ(cd::JournalAddMany(add), true);
    CHECK_EQ(cd::JournalAdd(4242, 777777ull, L"probe.exe"), true);
    CHECK_EQ((int)cd::JournalRead().size(), 1);

    Case("AE6d removing reports success, and removing what is not there is not a failure");
    std::vector<DWORD> drop;
    drop.push_back(4242);
    CHECK_EQ(cd::JournalRemoveMany(drop), true);
    CHECK_EQ((int)cd::JournalRead().size(), 0);
    CHECK_EQ(cd::JournalRemoveMany(drop), true);
    CHECK_EQ(cd::JournalRemove(4242), true);

    ::DeleteFileW(good.c_str());
    cd::JournalSetPathForTests(L"");
}

void Test_AE7_AnUnreadableJournalIsNeverOverwritten() {
    // CATCHES the worst consequence of the missing/unreadable conflation, with a fault that
    // is real and reversible: a file LARGER than the reader will accept. It exists, it is
    // perfectly writable, and it cannot be read - which is precisely the shape that turned
    // one failed read into a rewrite that dropped every record on disk.
    //
    // SetEndOfFile allocates without writing, so the 65 MB costs no I/O.
    Case("AE7 a journal too large to read is left exactly as it is");
    const std::wstring big = TempFilePath(L"go_test_journal_toobig_8271.bin");
    ::DeleteFileW(big.c_str());
    HANDLE h = ::CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER want;
    want.QuadPart = 65ll * 1024ll * 1024ll;      // one megabyte past the reader's ceiling
    CHECK(::SetFilePointerEx(h, want, nullptr, FILE_BEGIN) != 0);
    CHECK(::SetEndOfFile(h) != 0);
    ::CloseHandle(h);

    std::wstring text;
    CHECK_EQ(cd::ReadFileUtf8Checked(big, text), cd::FileReadResult::Unreadable);

    cd::JournalSetPathForTests(big);
    std::vector<cd::JournalEntry> add;
    add.push_back(Entry(4242, 777777ull, L"probe.exe"));

    CHECK_EQ(cd::JournalAddMany(add), false);
    LONGLONG after = 0;
    CHECK_EQ(FileSizeOf(big, after), true);
    CHECK_EQ(after == want.QuadPart, true);      // v0.4.3 replaced it with ONE LINE

    Case("AE7b removal refuses too - the same rewrite, the same records lost");
    std::vector<DWORD> drop;
    drop.push_back(4242);
    CHECK_EQ(cd::JournalRemoveMany(drop), false);
    CHECK_EQ(FileSizeOf(big, after), true);
    CHECK_EQ(after == want.QuadPart, true);

    Case("AE7c and startup recovery does not truncate a journal it never read");
    // v0.4.3 read the file, got an empty vector, logged "journal empty, nothing to clear"
    // and truncated - which is how an unreadable journal became a lost one at the exact
    // moment recovery was supposed to be undoing the last run's assignments.
    // Nothing is cleared here because nothing is parsed: no live process is touched.
    CHECK_EQ(cd::RecoverFromJournal(), 0);
    CHECK_EQ(FileSizeOf(big, after), true);
    CHECK_EQ(after == want.QuadPart, true);

    cd::JournalSetPathForTests(L"");
    ::DeleteFileW(big.c_str());
}

void Test_AE8_TheSetterChecksWhoItIsWriting() {
    // CATCHES the review's HIGH finding, and it is the one that can move a process the user
    // never agreed to move: ApplyCpuSets opened a PID and set its CPU sets without ever
    // asking whether the process behind that pid was still the one the caller decided about.
    // Windows reuses pids, so a process that exits after the snapshot hands its number to a
    // replacement - which then receives the original's decision, exclusion result and all.
    //
    // THIS RUNS AGAINST A REAL PROCESS - THIS ONE - AND THE REAL SETTER. Our own pid and our
    // own creation time are the only pair a test can be certain about, and the readback is
    // what proves the refusal was a refusal rather than a silent write.
    Case("AE8 a creation time that does not match the live process is refused");
    const DWORD me = ::GetCurrentProcessId();
    ULONGLONG created = 0;
    CHECK(cd::GetProcessCreationTime(me, created));
    CHECK(created != 0ull);
    if (created == 0ull) return;

    std::vector<ULONG> before;
    CHECK(cd::ReadCpuSets(me, before));

    cd::Topology topo;
    std::wstring err;
    CHECK(cd::DetectTopology(topo, &err));
    CHECK(!topo.entries.empty());
    if (topo.entries.empty()) return;
    std::vector<ULONG> ids;
    ids.push_back(topo.entries[0].Id);

    // A pid that has been RECYCLED reads as Gone - our process really is gone - and the flag
    // is what lets a log say WHICH kind of gone.
    cd::ApplyOutcome wrong = cd::ApplyCpuSets(me, ids, created + 1ull);
    CHECK_EQ(wrong.result, cd::ApplyResult::Gone);
    CHECK_EQ(wrong.identityMismatch, true);
    std::vector<ULONG> afterWrong;
    CHECK(cd::ReadCpuSets(me, afterWrong));
    CHECK_EQ(afterWrong == before, true);       // NOTHING was written

    Case("AE8b no identity at all is a refusal, not a licence to write");
    cd::ApplyOutcome blind = cd::ApplyCpuSets(me, ids, 0ull);
    CHECK_EQ(blind.result, cd::ApplyResult::OtherError);
    CHECK_EQ(blind.identityMismatch, true);
    CHECK(cd::ReadCpuSets(me, afterWrong));
    CHECK_EQ(afterWrong == before, true);

    Case("AE8c POSITIVE CONTROL - the RIGHT identity is accepted and really does write");
    // Without this the two refusals above would pass against a setter that refuses
    // everything, which is the vacuous-check failure this project keeps guarding against.
    cd::ApplyOutcome ok = cd::ApplyCpuSets(me, ids, created);
    CHECK_EQ(ok.result, cd::ApplyResult::Ok);
    std::vector<ULONG> now;
    CHECK(cd::ReadCpuSets(me, now));
    CHECK_EQ((int)now.size(), 1);
    if (now.size() == 1) CHECK_EQ(now[0], ids[0]);

    Case("AE8d and the clear is guarded the same way, so the restore is identity-checked");
    cd::ApplyOutcome wrongClear = cd::ClearCpuSets(me, created + 1ull);
    CHECK_EQ(wrongClear.result, cd::ApplyResult::Gone);
    CHECK(cd::ReadCpuSets(me, now));
    CHECK_EQ((int)now.size(), 1);               // still ours, still assigned

    // Put this process back exactly as it was found.
    cd::ApplyOutcome restore = cd::ApplyCpuSets(me, before, created);
    CHECK_EQ(restore.result, cd::ApplyResult::Ok);
    CHECK(cd::ReadCpuSets(me, now));
    CHECK_EQ(now == before, true);
}

// ===========================================================================
// AF. The apply gate compares the mask's CONTENT, not its NAME.
//
// THE DEFECT. Engine::Impl::Tick skips any pid whose record already says what is wanted,
// so an unchanged mask is not re-issued four times a second. At commit 80fea8b that test
// was, in full:
//
//     if (haveRecord && a->second.maskName == want) continue;
//
// and AppliedRec carried a maskName and no ids at all - so the only thing the gate could
// ever compare was the NAME. Edit mask "Cache" on the Core map from LPs 0-15 down to 0-7
// and KEEP THE NAME, which is what editing a mask normally looks like, and every already
// governed process stays on the old processors until it exits or the app restarts. The UI
// says the profile is on Cache; Cache now means 0-7; the processes are on 0-15.
//
// Nothing in the product can catch it: the CPU-Set getters echo stored intent and never
// effective placement, so there is no reading anywhere that disagrees with the wrong one.
//
// HOW THESE TESTS ESTABLISH FAILURE-BEFORE-FIX. The old gate is not described here, it is
// TRANSCRIBED - OldNameOnlyGate_Skips below is that one line and nothing else. Every case
// runs against BOTH gates and the disagreements are counted, so the suite does not assert
// that a fix was made, it exhibits the answers the shipped code gave. AF1 counts them as
// single decisions; AF2 replays them as a sequence of ticks, which is the form the user
// actually meets.
// ===========================================================================

// engine.cpp:1013 at commit 80fea8b, verbatim, as a function. Returns "the gate skipped
// this pid", i.e. no re-issue. It can only see names, because that is all the shipped
// AppliedRec stored.
bool OldNameOnlyGate_Skips(bool haveRecord,
                           const std::wstring& recordedMask,
                           const std::wstring& wantMask) {
    return haveRecord && recordedMask == wantMask;
}

struct GateCase {
    const char* what;
    bool haveRecord;
    const wchar_t* recordedMask;
    std::vector<ULONG> recordedIds;
    const wchar_t* wantMask;
    std::vector<ULONG> wantIds;
    bool expectReissue;
};

// LPs 0-15 and LPs 0-7 on the reference machine, as CPU Set Ids. The +256 offset is real on
// that part and is deliberately never assumed by the product - see coremap.cpp.
std::vector<ULONG> Cache16() { return IdRange(256, 16, 1); }
std::vector<ULONG> Cache8()  { return IdRange(256, 8, 1); }

std::vector<GateCase> GateCases() {
    std::vector<GateCase> v;
    std::vector<ULONG> shuffled = Cache8();
    std::swap(shuffled[0], shuffled[7]);
    std::swap(shuffled[2], shuffled[5]);
    std::vector<ULONG> dupes = Cache8();
    dupes.push_back(256);
    dupes.push_back(259);
    std::vector<ULONG> other8 = IdRange(264, 8, 1);
    std::vector<ULONG> shrunk = Cache8();
    shrunk.pop_back();
    std::vector<ULONG> none;

    v.push_back({ "a pid with no record at all is always issued",
                  false, L"", none, L"Cache", Cache16(), true });
    v.push_back({ "nothing changed - the gate must still hold",
                  true, L"Cache", Cache16(), L"Cache", Cache16(), false });
    v.push_back({ "THE BUG: the mask was edited 0-15 -> 0-7 and kept its name",
                  true, L"Cache", Cache16(), L"Cache", Cache8(), true });
    v.push_back({ "THE BUG, the other way: the mask was widened and kept its name",
                  true, L"Cache", Cache8(), L"Cache", Cache16(), true });
    v.push_back({ "THE BUG, same size: the mask was moved onto the other CCD",
                  true, L"Cache", Cache8(), L"Cache", other8, true });
    v.push_back({ "THE BUG, one processor taken away",
                  true, L"Cache", Cache8(), L"Cache", shrunk, true });
    v.push_back({ "the same processors in a different order are not a change",
                  true, L"Cache", Cache8(), L"Cache", shuffled, false });
    v.push_back({ "the same processors listed twice are not a change",
                  true, L"Cache", Cache8(), L"Cache", dupes, false });
    v.push_back({ "a different mask name is a change, as it always was",
                  true, L"Cache", Cache16(), L"Freq", other8, true });
    v.push_back({ "a renamed mask with identical content is still a change",
                  true, L"Cache", Cache8(), L"CacheV2", Cache8(), true });
    v.push_back({ "a pid recorded as cleared, now wanted on a real mask",
                  true, L"", none, L"Cache", Cache8(), true });
    v.push_back({ "a pid recorded as cleared and still wanted cleared",
                  true, L"", none, L"", none, false });
    return v;
}

void Test_AF1_TheGateComparesContentNotName() {
    Case("AF1 the gate's answer for every shape of change, name-only vs name-and-content");
    const std::vector<GateCase> cases = GateCases();
    int oldWrong = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const GateCase& c = cases[i];
        // THE RULE UNDER TEST. Both id lists arrive normalised, exactly as engine.cpp
        // normalises them at the one place a mask is resolved.
        const bool reissue = cd::NeedsReissue(c.haveRecord,
                                              c.recordedMask,
                                              cd::NormalizedMaskIds(c.recordedIds),
                                              c.wantMask,
                                              cd::NormalizedMaskIds(c.wantIds));
        if (reissue != c.expectReissue) std::printf("       case: %s\n", c.what);
        CHECK_EQ(reissue, c.expectReissue);

        // THE SHIPPED GATE, on the same row. Not a paraphrase - the transcribed line.
        const bool oldReissue = !OldNameOnlyGate_Skips(c.haveRecord, c.recordedMask, c.wantMask);
        if (oldReissue != c.expectReissue) {
            ++oldWrong;
            std::printf("       80fea8b answered %s, the right answer is %s: %s\n",
                        oldReissue ? "re-issue" : "skip",
                        c.expectReissue ? "re-issue" : "skip",
                        c.what);
        }
    }

    Case("AF1b FAILURE-BEFORE-FIX - the shipped gate gets exactly four of these twelve wrong");
    // Every one of the four is the same shape: the name stayed, the processors moved, and
    // the process was left where it was. The number is pinned so a later edit that quietly
    // reintroduces a name-only comparison cannot pass this file.
    CHECK_EQ((int)cases.size(), 12);
    CHECK_EQ(oldWrong, 4);

    Case("AF1c POSITIVE CONTROL - the new rule is not simply answering 're-issue' every time");
    // Four of the twelve rows expect a SKIP, and a rule that always re-issued would fail
    // them. Counted here as well so the control cannot be lost inside the table.
    int expectSkip = 0;
    for (size_t i = 0; i < cases.size(); ++i) if (!cases[i].expectReissue) ++expectSkip;
    CHECK_EQ(expectSkip, 4);
}

// The gate AS THE TICK RUNS IT: resolve the mask, normalise it, ask the rule, and on a
// re-issue write BOTH halves back into the record. Mirrors engine.cpp's apply loop; the
// only thing left out is the applier itself.
struct GateSim {
    bool haveRecord = false;
    std::wstring maskName;
    std::vector<ULONG> maskIds;
    int reissues = 0;

    void Tick(const std::wstring& want, const std::vector<ULONG>& rawIds) {
        const std::vector<ULONG> ids = cd::NormalizedMaskIds(rawIds);
        if (!cd::NeedsReissue(haveRecord, maskName, maskIds, want, ids)) return;
        ++reissues;
        haveRecord = true;
        maskName = want;
        maskIds = ids;
    }
};

// The same loop with the shipped gate in it, and a record with nowhere to put the ids.
struct OldGateSim {
    bool haveRecord = false;
    std::wstring maskName;
    int reissues = 0;

    void Tick(const std::wstring& want, const std::vector<ULONG>&) {
        if (OldNameOnlyGate_Skips(haveRecord, maskName, want)) return;
        ++reissues;
        haveRecord = true;
        maskName = want;
    }
};

void Test_AF2_AnEditedMaskReachesTheMachine() {
    Case("AF2 forty ticks, one edit at the halfway mark - the new gate issues twice");
    // Ten seconds of watcher at the default 250 ms poll. The mask is edited in place at
    // tick 20: same name, half the processors.
    GateSim sim;
    for (int t = 0; t < 40; ++t) {
        sim.Tick(L"Cache", t < 20 ? Cache16() : Cache8());
    }
    CHECK_EQ(sim.reissues, 2);                       // the first tick, and the edit
    CHECK_EQ(sim.maskIds, Cache8());                 // and the record carries the edit

    Case("AF2b FAILURE-BEFORE-FIX - the shipped gate issues ONCE and never sees the edit");
    OldGateSim shipped;
    for (int t = 0; t < 40; ++t) {
        shipped.Tick(L"Cache", t < 20 ? Cache16() : Cache8());
    }
    CHECK_EQ(shipped.reissues, 1);                   // twenty ticks of a stale mask, silently

    Case("AF2c THE CONVERSE, with the storm it prevents made explicit");
    // Losing the gate is not a smaller bug than the one being fixed: at ~200 processes under
    // extreme game mode and a 250 ms poll it is 800 setter calls a second, for ever.
    GateSim quiet;
    for (int t = 0; t < 40; ++t) quiet.Tick(L"Cache", Cache16());
    CHECK_EQ(quiet.reissues, 1);
    // ...and forty ticks whose ids are merely REORDERED each time, which is what a
    // hand-written config.ini can produce, still issue once.
    GateSim jumbled;
    std::vector<ULONG> a = Cache8();
    std::vector<ULONG> b = Cache8();
    std::swap(b[0], b[7]);
    for (int t = 0; t < 40; ++t) jumbled.Tick(L"Cache", (t % 2) ? a : b);
    CHECK_EQ(jumbled.reissues, 1);

    Case("AF2d a real edit still gets through after a run of reordered no-ops");
    jumbled.Tick(L"Cache", Cache16());
    CHECK_EQ(jumbled.reissues, 2);
}

void Test_AF3_OrderComesOffDiskAndIsNotAChange() {
    Case("AF3 config.ini keeps a mask's ids in FILE ORDER, which is why the gate normalises");
    // The claim the normalisation rests on, measured against the real parser rather than
    // assumed: ParseMaskValue appends ids as it meets them and nothing sorts them
    // afterwards, so a hand-edited file really can hand ResolveMask "258,256,257".
    cd::Config c;
    std::wstring err;
    CHECK(cd::ParseConfig(L"[masks]\nCache=258,256,257\n", c, &err));
    const cd::Mask* m = c.FindMask(L"Cache");
    CHECK(m != nullptr);
    if (!m) return;
    std::vector<ULONG> fileOrder;
    fileOrder.push_back(258);
    fileOrder.push_back(256);
    fileOrder.push_back(257);
    CHECK_EQ(m->ids, fileOrder);                     // NOT sorted on the way in

    Case("AF3b so an order-only difference is not a change, and normalising says so");
    std::vector<ULONG> ascending;
    ascending.push_back(256);
    ascending.push_back(257);
    ascending.push_back(258);
    CHECK_EQ(cd::NormalizedMaskIds(fileOrder), ascending);
    CHECK_EQ(cd::SameMaskIds(fileOrder, ascending), true);
    CHECK_EQ(cd::NeedsReissue(true, L"Cache", ascending, L"Cache", fileOrder), false);

    Case("AF3c POSITIVE CONTROL - SameMaskIds is not answering true for everything");
    std::vector<ULONG> missingOne;
    missingOne.push_back(256);
    missingOne.push_back(257);
    CHECK_EQ(cd::SameMaskIds(fileOrder, missingOne), false);
    CHECK_EQ(cd::NeedsReissue(true, L"Cache", ascending, L"Cache", missingOne), true);
    // A duplicate is not a difference either - the setter takes these ids as a SET.
    std::vector<ULONG> withDupe = ascending;
    withDupe.push_back(257);
    CHECK_EQ(cd::SameMaskIds(ascending, withDupe), true);
}

void Test_AF4_AnEditedMaskAfterABlockedAttemptIsStillJournalled() {
    // THE INTERACTION WITH v0.4.4's JOURNAL RULES. A re-issue caused by changed ids is a new
    // assignment attempt on a pid that ALREADY HAS A RECORD, which is the exact shape rule 1
    // was rewritten for. These cases compose rule 5 with rules 1 and 2 in the order a tick
    // meets them, so a re-issue can neither drop nor duplicate a journal entry.
    Case("AF4 an edited mask retries a pid whose first attempt was REFUSED, and journals it");
    // Tick 1: the assignment was refused (AccessDenied, an elevated process). engine.cpp
    // keeps the record - blocked - and, because nothing of ours ever landed, takes the
    // journal entry back out: everApplied = false, journalled = false.
    const bool haveRecord = true;
    const bool journalled = false;

    // Ticks 2..n: same mask, so the gate holds and the refusal is NOT retried at 4 Hz.
    CHECK_EQ(cd::NeedsReissue(haveRecord, L"Cache", Cache16(), L"Cache", Cache16()), false);

    // The operator now edits Cache. The gate opens...
    CHECK_EQ(cd::NeedsReissue(haveRecord, L"Cache", Cache16(), L"Cache", Cache8()), true);
    // ...rule 1 sees a pid with a record but NO entry on disk, and writes one...
    CHECK_EQ(cd::NeedsRecoveryRecord(haveRecord, haveRecord && journalled), true);
    // ...and rule 2 lets the retry reach the setter only once that write has landed.
    CHECK_EQ(cd::MayApplyAssignment(true, true), true);
    CHECK_EQ(cd::MayApplyAssignment(true, false), false);

    Case("AF4b a held-back re-issue leaves the record alone, so the next tick tries again");
    // Rule 2 said no, so engine.cpp writes NOTHING into `applied` - the record still names
    // the OLD ids, and the gate must still be open on the next tick. A fix that updated the
    // record before the apply would close it and lose the edit for ever.
    CHECK_EQ(cd::NeedsReissue(haveRecord, L"Cache", Cache16(), L"Cache", Cache8()), true);

    Case("AF4c a re-issue on an ALREADY JOURNALLED pid adds no second entry");
    // The journal entry is keyed on pid + creation time + name and says nothing about which
    // mask is on the process, so the entry already on disk still describes this assignment.
    // Rule 1 answers no, and no duplicate is written.
    CHECK_EQ(cd::NeedsRecoveryRecord(true, true), false);
    // ...and rule 2 does not hold a re-issue hostage to an unrelated failed batch write.
    CHECK_EQ(cd::MayApplyAssignment(false, false), true);

    Case("AF4d a re-issue never drops an entry, because it is not a clear");
    // Rules 3 and 4 are reached only by a pid that LEFT the desired set. A re-issued pid is
    // still in it, so nothing on this path can remove its record - and if the retry succeeds
    // everApplied becomes true, which is what keeps the entry when the pid does leave.
    CHECK_EQ(cd::NeedsClearOnLeaving(true), true);
    CHECK_EQ(cd::MayDropRecoveryRecord(true, cd::ApplyResult::AccessDenied), false);
}

// ===========================================================================
// AA. Rule 1 - the game mask follows the game that is actually being played.
//
// THE DEFECT, in the operator's words: "I usually have 2 games open at once. For example, I
// play Overwatch, it swap Overwatch as default. Then I open another game, the another game
// probably was pinned to heavy mask at the background during Overwatch."
//
// Every fixture below puts the WRONG game first in config order, because that is the shape
// of the operator's own config.ini (Palworld above Overwatch 2): a test whose two profiles
// are already in the desired order would pass against the broken implementation.
// ===========================================================================

// pid 500  explorer.exe
// pid 1000 Palworld.exe            <- game of the FIRST profile in config order
// pid 1001 PalworldLauncher.exe    <- descendant of 1000; its window counts as Palworld's
// pid 2000 Overwatch.exe           <- game of the SECOND profile in config order
// pid 2001 OverwatchChild.exe      <- descendant of 2000
// pid 3500 Discord.exe             <- never a candidate; alt-tabbing here must change nothing
cd::ProcessSnapshot MakeTwoGameSnapshot() {
    cd::ProcessSnapshot s;
    AddProc(s, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s, 3500, 500, L"Discord.exe", 150, 0, 0.0);
    AddProc(s, 1000, 500, L"Palworld.exe", 200, 0, 0.0);
    AddProc(s, 1001, 1000, L"PalworldLauncher.exe", 300, 0, 0.0);
    AddProc(s, 2000, 500, L"Overwatch.exe", 400, 0, 0.0);
    AddProc(s, 2001, 2000, L"OverwatchChild.exe", 500, 0, 0.0);
    return s;
}

// Palworld FIRST, exactly as the operator's file has it.
cd::Config MakeTwoGameConfig(const cd::Topology& t) {
    cd::Config c;
    c.version = 1;
    c.pollMs = 250;
    c.masks = cd::DeriveMasks(t);

    cd::Profile pal;
    pal.name = L"Palworld";
    pal.enabled = true;
    pal.game = L"Palworld.exe";
    pal.gameMask = L"Cache no SMT";
    pal.heavyMask = L"Freq";
    pal.autoPin = false;
    c.profiles.push_back(pal);

    cd::Profile ow;
    ow.name = L"Overwatch 2";
    ow.enabled = true;
    ow.game = L"Overwatch.exe";
    ow.gameMask = L"Cache no SMT";
    ow.heavyMask = L"Freq";
    ow.autoPin = false;
    c.profiles.push_back(ow);
    return c;
}

cd::SelectionCandidate Cand(const wchar_t* name, bool fg) {
    cd::SelectionCandidate c;
    c.name = name;
    c.ownsForeground = fg;
    return c;
}

// Runs one tick of rule 1 and reports who won. Everything below drives ComputeDesired rather
// than ChooseProfile alone wherever the point is END TO END: the pure chooser agreeing with
// itself proves nothing about whether the engine consults it.
std::wstring TickWinner(const cd::ProcessSnapshot& s, const cd::Config& c, DWORD fg,
                        cd::ProfileSelection& sel, std::map<DWORD, std::wstring>* out) {
    std::vector<std::wstring> sticky;
    const cd::Profile* matched = reinterpret_cast<const cd::Profile*>(0x1);
    std::map<DWORD, std::wstring> res =
        cd::ComputeDesired(s, c, fg, sticky, &matched, 0, nullptr, nullptr, &sel);
    if (out) *out = res;
    return matched ? matched->name : std::wstring();
}

void Test_AA1_DwellIsDerivedFromThePollInterval() {
    Case("AA1 the dwell is DERIVED from poll_ms, never a hardcoded tick count");
    // 3 s at the shipped default. This is the number the operator was quoted.
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(250), 12);
    // Both ends of the clamp config.h enforces on poll_ms.
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(100), 30);
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(2000), 2);
    // Rounds UP, so the dwell is never SHORTER than 3 s.
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(400), 8);
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(3000), 1);
    // Absurd input still yields a real dwell: a zero-tick dwell is not a dwell.
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(0), 1);
    CHECK_EQ(cd::ForegroundSwitchDwellTicks(-5), 1);
}

void Test_AA2_ChooseProfileWithNothingToChoose() {
    Case("AA2 no candidates clears the state, and says so exactly once");
    std::vector<cd::SelectionCandidate> none;
    cd::ProfileSelection sel;
    CHECK_EQ(cd::ChooseProfile(none, 12, sel), -1);
    CHECK_EQ(sel.reason, cd::SelectReason::Unchanged);   // nothing was governing to clear

    sel.selected = L"Overwatch 2";
    CHECK_EQ(cd::ChooseProfile(none, 12, sel), -1);
    CHECK_EQ(sel.reason, cd::SelectReason::Cleared);
    CHECK(sel.selected.empty());
    CHECK_EQ(sel.challengerTicks, 0);
    // ...and it does not keep announcing it.
    CHECK_EQ(cd::ChooseProfile(none, 12, sel), -1);
    CHECK_EQ(sel.reason, cd::SelectReason::Unchanged);
}

void Test_AA3_FirstSelectionIsImmediateAndPrefersTheForeground() {
    Case("AA3 with nothing selected the FOREGROUND candidate wins at once - no dwell");
    std::vector<cd::SelectionCandidate> v;
    v.push_back(Cand(L"Palworld", false));
    v.push_back(Cand(L"Overwatch 2", true));

    cd::ProfileSelection sel;
    CHECK_EQ(cd::ChooseProfile(v, 12, sel), 1);
    CHECK_EQ(sel.reason, cd::SelectReason::First);
    CHECK_EQ(sel.selected, std::wstring(L"Overwatch 2"));

    Case("AA3b with nothing selected and no foreground, the first candidate wins at once");
    std::vector<cd::SelectionCandidate> w;
    w.push_back(Cand(L"Palworld", false));
    w.push_back(Cand(L"Overwatch 2", false));
    cd::ProfileSelection sel2;
    CHECK_EQ(cd::ChooseProfile(w, 12, sel2), 0);
    CHECK_EQ(sel2.reason, cd::SelectReason::First);
}

void Test_AA4_AnInterruptedDwellNeverSwitches() {
    Case("AA4 the dwell is CONTINUOUS: one tick away from the challenger resets the count");
    std::vector<cd::SelectionCandidate> chal;      // Palworld in front, Overwatch governing
    chal.push_back(Cand(L"Palworld", true));
    chal.push_back(Cand(L"Overwatch 2", false));
    std::vector<cd::SelectionCandidate> quiet;     // nobody in front
    quiet.push_back(Cand(L"Palworld", false));
    quiet.push_back(Cand(L"Overwatch 2", false));

    cd::ProfileSelection sel;
    sel.selected = L"Overwatch 2";

    // Eleven ticks of challenge - one short - then a single tick away.
    for (int i = 0; i < 11; ++i) CHECK_EQ(cd::ChooseProfile(chal, 12, sel), 1);
    CHECK_EQ(sel.challengerTicks, 11);
    CHECK_EQ(cd::ChooseProfile(quiet, 12, sel), 1);
    CHECK_EQ(sel.challengerTicks, 0);

    // Eleven more must STILL not be enough: the count restarted from zero.
    for (int i = 0; i < 11; ++i) CHECK_EQ(cd::ChooseProfile(chal, 12, sel), 1);
    CHECK_EQ(sel.selected, std::wstring(L"Overwatch 2"));
    CHECK_EQ(sel.reason, cd::SelectReason::Unchanged);
    // The twelfth consecutive one is.
    CHECK_EQ(cd::ChooseProfile(chal, 12, sel), 0);
    CHECK_EQ(sel.reason, cd::SelectReason::Foreground);

    Case("AA4b a DIFFERENT challenger restarts the count rather than inheriting it");
    std::vector<cd::SelectionCandidate> three;
    three.push_back(Cand(L"Palworld", false));
    three.push_back(Cand(L"Overwatch 2", false));
    three.push_back(Cand(L"StarRail", true));
    cd::ProfileSelection s2;
    s2.selected = L"Overwatch 2";
    for (int i = 0; i < 11; ++i) CHECK_EQ(cd::ChooseProfile(chal, 12, s2), 1);
    CHECK_EQ(s2.challengerTicks, 11);
    CHECK_EQ(cd::ChooseProfile(three, 12, s2), 1);      // StarRail takes over the challenge
    CHECK_EQ(s2.challengerTicks, 1);
    CHECK_EQ(s2.challenger, std::wstring(L"StarRail"));
}

void Test_AA5_TwoLiveGamesTheForegroundOneWins() {
    Case("AA5 two live games, foreground on the SECOND: the second one is governed");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);
    cd::ProcessSnapshot s = MakeTwoGameSnapshot();

    // Fixture sanity: config order really does put the wrong game first, so a pass here
    // cannot be the old "first enabled profile" rule getting lucky.
    CHECK_EQ(c.profiles[0].name, std::wstring(L"Palworld"));

    cd::ProfileSelection sel;
    std::map<DWORD, std::wstring> res;
    CHECK_EQ(TickWinner(s, c, 2000, sel, &res), std::wstring(L"Overwatch 2"));
    CHECK_EQ(sel.reason, cd::SelectReason::First);

    // The masks followed. Overwatch and its child are on the game mask...
    CHECK_EQ(MaskOf(res, 2000), std::wstring(L"Cache no SMT"));
    CHECK_EQ(MaskOf(res, 2001), std::wstring(L"Cache no SMT"));
    // ...and the background game is simply NOT GOVERNED. It is deliberately not forced onto
    // the heavy mask: the operator asked for the right game to be pinned, not for the other
    // one to be punished.
    CHECK(!Has(res, 1000));
    CHECK(!Has(res, 1001));

    Case("AA5b and the same tick with the foreground on the FIRST game picks the first");
    cd::ProfileSelection sel2;
    CHECK_EQ(TickWinner(s, c, 1000, sel2, nullptr), std::wstring(L"Palworld"));
}

void Test_AA6_ForegroundOnADescendantStillCounts() {
    Case("AA6 the foreground window may belong to a DESCENDANT - launcher, child, shim");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);
    cd::ProcessSnapshot s = MakeTwoGameSnapshot();

    cd::ProfileSelection sel;
    CHECK_EQ(TickWinner(s, c, 2001, sel, nullptr), std::wstring(L"Overwatch 2"));

    // And the mirror image, so this is not passing because Overwatch always wins.
    cd::ProfileSelection sel2;
    CHECK_EQ(TickWinner(s, c, 1001, sel2, nullptr), std::wstring(L"Palworld"));
}

void Test_AA7_AltTabbingToSomethingElseNeverUnpinsTheGame() {
    Case("AA7 THE MOST IMPORTANT RULE: a foreground that is no game holds the selection");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);
    cd::ProcessSnapshot s = MakeTwoGameSnapshot();

    cd::ProfileSelection sel;
    CHECK_EQ(TickWinner(s, c, 2000, sel, nullptr), std::wstring(L"Overwatch 2"));

    // Discord, a browser, this app's own settings window, or a foreground we could not read
    // at all. NONE of them may hand the mask back to the game further up config.ini.
    const DWORD notAGame[] = { 3500, 500, 0, 987654 };
    for (size_t i = 0; i < sizeof(notAGame) / sizeof(notAGame[0]); ++i) {
        std::map<DWORD, std::wstring> res;
        CHECK_EQ(TickWinner(s, c, notAGame[i], sel, &res), std::wstring(L"Overwatch 2"));
        CHECK_EQ(sel.reason, cd::SelectReason::Unchanged);
        CHECK(!Has(res, 1000));
    }
    // Thirty ticks of it - far past any dwell - still nothing.
    for (int i = 0; i < 30; ++i)
        CHECK_EQ(TickWinner(s, c, 3500, sel, nullptr), std::wstring(L"Overwatch 2"));
}

void Test_AA8_TheDwellThroughComputeDesired() {
    Case("AA8 a challenger holds the foreground: 11 ticks change nothing, the 12th switches");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);
    cd::ProcessSnapshot s = MakeTwoGameSnapshot();
    const int dwell = cd::ForegroundSwitchDwellTicks(c.pollMs);
    CHECK_EQ(dwell, 12);

    cd::ProfileSelection sel;
    CHECK_EQ(TickWinner(s, c, 2000, sel, nullptr), std::wstring(L"Overwatch 2"));

    // Alt-tab to Palworld and hold it. Every tick short of the dwell keeps Overwatch, and
    // keeps Overwatch's family on the game mask - this is what stops a rapid alt-tab from
    // re-pinning ~150 processes twice a second under extreme game mode.
    for (int i = 1; i < dwell; ++i) {
        std::map<DWORD, std::wstring> res;
        CHECK_EQ(TickWinner(s, c, 1000, sel, &res), std::wstring(L"Overwatch 2"));
        CHECK_EQ(MaskOf(res, 2000), std::wstring(L"Cache no SMT"));
        CHECK(!Has(res, 1000));
    }

    Case("AA8b the dwell elapses and the mask moves with it");
    std::map<DWORD, std::wstring> res;
    CHECK_EQ(TickWinner(s, c, 1000, sel, &res), std::wstring(L"Palworld"));
    CHECK_EQ(sel.reason, cd::SelectReason::Foreground);
    CHECK_EQ(MaskOf(res, 1000), std::wstring(L"Cache no SMT"));
    CHECK_EQ(MaskOf(res, 1001), std::wstring(L"Cache no SMT"));
    CHECK(!Has(res, 2000));
    CHECK(!Has(res, 2001));

    Case("AA8c and the switch is announced ONCE, not on every tick afterwards");
    CHECK_EQ(TickWinner(s, c, 1000, sel, nullptr), std::wstring(L"Palworld"));
    CHECK_EQ(sel.reason, cd::SelectReason::Unchanged);
}

void Test_AA9_TheSelectedGameExitingReleasesAtOnce() {
    Case("AA9 the governing game exits: the survivor takes over IMMEDIATELY, no dwell");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);
    cd::ProcessSnapshot both = MakeTwoGameSnapshot();

    cd::ProfileSelection sel;
    CHECK_EQ(TickWinner(both, c, 2000, sel, nullptr), std::wstring(L"Overwatch 2"));

    // Overwatch closes. Palworld is still running and nothing is in the foreground that we
    // recognise - the desktop, say. One tick, not twelve.
    cd::ProcessSnapshot alone;
    AddProc(alone, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(alone, 1000, 500, L"Palworld.exe", 200, 0, 0.0);
    AddProc(alone, 1001, 1000, L"PalworldLauncher.exe", 300, 0, 0.0);

    std::map<DWORD, std::wstring> res;
    CHECK_EQ(TickWinner(alone, c, 500, sel, &res), std::wstring(L"Palworld"));
    CHECK_EQ(sel.reason, cd::SelectReason::Released);
    CHECK_EQ(MaskOf(res, 1000), std::wstring(L"Cache no SMT"));

    Case("AA9b every game exits: nothing is governed and the state is wiped");
    cd::ProcessSnapshot empty;
    AddProc(empty, 500, 400, L"explorer.exe", 100, 0, 0.0);
    std::map<DWORD, std::wstring> res2;
    CHECK_EQ(TickWinner(empty, c, 500, sel, &res2), std::wstring());
    CHECK_EQ((int)res2.size(), 0);
    CHECK(sel.selected.empty());

    Case("AA9c so the NEXT game to appear is pinned at once rather than after a dwell");
    std::map<DWORD, std::wstring> res3;
    CHECK_EQ(TickWinner(alone, c, 500, sel, &res3), std::wstring(L"Palworld"));
    CHECK_EQ(sel.reason, cd::SelectReason::First);
    CHECK_EQ(MaskOf(res3, 1000), std::wstring(L"Cache no SMT"));
}

void Test_AA10_OneGameRunningIsByteIdenticalToBefore() {
    Case("AA10 with ONE game running the answer does not depend on the new state at all");
    cd::Topology t = MakeReference(false);
    cd::Config c = MakeTwoGameConfig(t);

    // Only the SECOND profile's game is live, so the old first-match rule and the new one
    // must agree - and they must agree whatever the foreground is, including a foreground
    // that belongs to nothing.
    cd::ProcessSnapshot s;
    AddProc(s, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(s, 3500, 500, L"Discord.exe", 150, 0, 0.0);
    AddProc(s, 2000, 500, L"Overwatch.exe", 400, 0, 0.0);
    AddProc(s, 2001, 2000, L"OverwatchChild.exe", 500, 0, 0.0);

    const DWORD fgs[] = { 2000, 2001, 3500, 500, 0 };
    // ONE state object across all five, so this is a session and not five first ticks.
    cd::ProfileSelection sel;
    for (size_t i = 0; i < sizeof(fgs) / sizeof(fgs[0]); ++i) {
        // The stateless call - exactly what every caller that predates this feature makes.
        std::vector<std::wstring> stickyA;
        const cd::Profile* matchedA = nullptr;
        std::map<DWORD, std::wstring> a =
            cd::ComputeDesired(s, c, fgs[i], stickyA, &matchedA);

        // The stateful one.
        std::map<DWORD, std::wstring> b;
        const std::wstring nameB = TickWinner(s, c, fgs[i], sel, &b);

        CHECK(matchedA != nullptr);
        CHECK_EQ(matchedA->name, std::wstring(L"Overwatch 2"));
        CHECK_EQ(nameB, std::wstring(L"Overwatch 2"));
        CHECK(a == b);      // the whole desired map, not just the winner
    }
}

void Test_AA11_AMigratedLegacyProfileGovernsNothing() {
    // 🔴 WHAT THIS SLOT USED TO HOLD, AND WHY IT COULD NOT BE SAVED. AA11 asserted that an
    // All Games incumbent yielded to a specific profile IMMEDIATELY, with no dwell. That was
    // a property of Rule 1b and ONLY of Rule 1b: an All Games profile joined the candidate
    // list only while the list was empty, so the instant a specific game matched, the
    // incumbent vanished from the candidates and ChooseProfile reported Released.
    //
    // v0.5.4 deleted Rule 1b. Two ordinary profiles are BOTH candidates, so the incumbent
    // stays in the list and the three-second dwell applies exactly as it should. Rewriting
    // the old assertion in terms of ordinary profiles produced a test that FAILED, and it
    // deserved to - it was asserting a rule this build does not have. The half of the claim
    // that survives, "a profile whose game exited releases at once", is already covered by
    // AA9 and again at the ChooseProfile level by AA13, so restating it here would buy
    // nothing.
    //
    // WHAT REPLACES IT IS THE END-TO-END HALF OF THE MIGRATION, which nothing else tests:
    // config.cpp rule 6b clears a legacy profile's stale `game`, and the point of clearing
    // it is that the profile must not start governing an executable the user never chose for
    // it. B12b proves the field is cleared; this proves the ENGINE agrees.
    Case("AA11 a migrated legacy All Games profile governs nothing, even with its old game up");
    cd::Topology t = MakeReference(false);
    cd::Config c;
    c.version = 1;
    c.pollMs = 250;
    c.masks = cd::DeriveMasks(t);

    cd::Profile ow;
    ow.name = L"Overwatch 2";
    ow.enabled = true;
    ow.game = L"Overwatch.exe";
    ow.gameMask = L"Cache no SMT";
    ow.heavyMask = L"Freq";
    c.profiles.push_back(ow);

    // As an OLD config would load it: the retired flag set, and a stale executable in the
    // field the engine used to overwrite on every tick. ENABLED, so nothing but the cleared
    // game can be what stops it.
    cd::Profile legacy;
    legacy.name = L"All Games";
    legacy.enabled = true;
    legacy.legacyAllGames = true;
    legacy.game = L"Palworld.exe";
    legacy.gameMask = L"Cache no SMT";
    legacy.heavyMask = L"Freq";
    c.profiles.push_back(legacy);

    // FAILURE-BEFORE-FIX CONTROL. Before the repair runs, that stale field IS live - this is
    // precisely the hazard rule 6b exists to close, and asserting it here is what stops the
    // test below from passing vacuously on a build that never set the game at all.
    cd::ProcessSnapshot pal;
    AddProc(pal, 500, 400, L"explorer.exe", 100, 0, 0.0);
    AddProc(pal, 1000, 500, L"Palworld.exe", 200, 0, 0.0);
    {
        cd::ProfileSelection sel;
        CHECK_EQ(TickWinner(pal, c, 1000, sel, nullptr), std::wstring(L"All Games"));
    }

    // Now load it the way the app does.
    const std::vector<std::wstring> rep = cd::ValidateAndRepair(c, t);
    CHECK(AnyContains(rep, L"All Games"));

    // Palworld is still running. The migrated profile must govern NOTHING.
    {
        cd::ProfileSelection sel;
        std::map<DWORD, std::wstring> res;
        CHECK_EQ(TickWinner(pal, c, 1000, sel, &res), std::wstring());
        CHECK(res.empty());
    }

    // And the ordinary profile beside it is untouched: Overwatch still governs when it runs.
    {
        cd::ProcessSnapshot ow2;
        AddProc(ow2, 500, 400, L"explorer.exe", 100, 0, 0.0);
        AddProc(ow2, 2000, 500, L"Overwatch.exe", 200, 0, 0.0);
        cd::ProfileSelection sel;
        CHECK_EQ(TickWinner(ow2, c, 2000, sel, nullptr), std::wstring(L"Overwatch 2"));
    }
}

void Test_AA12_EveryReasonHasItsOwnWords() {
    Case("AA12 every reason prints a distinct, non-empty phrase for the log line");
    const cd::SelectReason all[] = {
        cd::SelectReason::Unchanged, cd::SelectReason::First, cd::SelectReason::Foreground,
        cd::SelectReason::Released, cd::SelectReason::Cleared,
    };
    std::set<std::wstring> seen;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const std::wstring w = cd::SelectReasonText(all[i]);
        CHECK(!w.empty());
        seen.insert(w);
    }
    CHECK_EQ((int)seen.size(), (int)(sizeof(all) / sizeof(all[0])));
}

// ===========================================================================
// AB. The version label in the corner of the settings window.
//
// Only the PURE half is testable here, and it is the half that can be wrong in a way nobody
// notices: the Win32 half either reads the resource or does not, and its failure mode is an
// absent label. See src\version_label.h. The number itself is checked against the BUILT
// BINARY in the gate, not here - a test that reads the .rc would be checking the input.
// ===========================================================================

void Test_AA13_OneCandidateIgnoresTheForegroundEntirely() {
    Case("AA13 with ONE candidate the foreground flag cannot change the answer");
    // This is not a curiosity. ComputeDesired SKIPS the whole descendant walk below two
    // candidates on the strength of exactly this invariant, so if it ever stopped holding,
    // the single-game path would silently start ignoring a foreground that mattered.
    const bool fgs[] = { false, true };
    for (size_t i = 0; i < 2; ++i) {
        std::vector<cd::SelectionCandidate> one;
        one.push_back(Cand(L"Overwatch 2", fgs[i]));

        cd::ProfileSelection fresh;                       // no incumbent
        CHECK_EQ(cd::ChooseProfile(one, 12, fresh), 0);
        CHECK_EQ(fresh.reason, cd::SelectReason::First);

        cd::ProfileSelection sitting;                     // it IS the incumbent
        sitting.selected = L"Overwatch 2";
        CHECK_EQ(cd::ChooseProfile(one, 12, sitting), 0);
        CHECK_EQ(sitting.reason, cd::SelectReason::Unchanged);

        cd::ProfileSelection other;                       // someone else was, and has gone
        other.selected = L"Palworld";
        CHECK_EQ(cd::ChooseProfile(one, 12, other), 0);
        CHECK_EQ(other.reason, cd::SelectReason::Released);
    }
}

void Test_AB1_TheShippedVersionFormatsAsTheOperatorNamesIt() {
    Case("AB1 0,5,8,0 in the resource reads 'v0.5.8' on screen");
    // ms = (major<<16)|minor, ls = (patch<<16)|build - the VS_FIXEDFILEINFO packing.
    // Tracks src\GameOptimizer.rc: a version bump that leaves this vector behind makes the
    // case NAME a lie while the assertion still passes, which is the quiet half of a stale test.
    CHECK_EQ(cd::FormatVersionLabel(0x00000005u, 0x00080000u), std::wstring(L"v0.5.8"));
}

void Test_AB2_AFourthFieldIsShownOnlyWhenItSaysSomething() {
    Case("AB2 the build field appears only when it is non-zero");
    CHECK_EQ(cd::FormatVersionLabel(0x00000004u, 0x00030007u), std::wstring(L"v0.4.3.7"));
    CHECK_EQ(cd::FormatVersionLabel(0x00010014u, 0x012C0000u), std::wstring(L"v1.20.300"));
}

void Test_AB3_AnUnreadableVersionDrawsNothing() {
    Case("AB3 0.0.0.0 is as useless as a failed read, so it hides the label");
    CHECK(cd::FormatVersionLabel(0u, 0u).empty());
    // ...but a genuine 0.0.0.1 is not nothing.
    CHECK_EQ(cd::FormatVersionLabel(0u, 1u), std::wstring(L"v0.0.0.1"));
}

void Test_AB4_TheLabelIsNeverSomethingElse() {
    Case("AB4 tripwire: the label starts with 'v', carries no spaces and no stray suffix");
    const std::wstring v = cd::FormatVersionLabel(0x00000004u, 0x00030000u);
    CHECK(!v.empty() && v[0] == L'v');
    CHECK(v.find(L' ') == std::wstring::npos);
    CHECK(v.find(L"..") == std::wstring::npos);
    CHECK(v.size() < 32);   // it shares a row with the OK button; it is not a paragraph
}

// ===========================================================================
// == AG. Auto-Isolate GPU - pure decision logic ==
// ===========================================================================

void Test_AG1_AdapterKeyFromPnpId() {
    Case("AG1 PnP ID parsing extracts VEN&DEV&SUBSYS, uppercase");
    std::wstring input = L"PCI\\VEN_10DE&DEV_2684&SUBSYS_40BF1458&REV_A1\\4&15A5C264&0&000B";
    CHECK_EQ(cd::AdapterKeyFromPnpId(input), std::wstring(L"10DE&2684&40BF1458"));

    // Lowercase input should give uppercase output
    std::wstring lowercaseInput = L"pci\\ven_10de&dev_2684&subsys_40bf1458&rev_a1\\4&15a5c264&0&000b";
    CHECK_EQ(cd::AdapterKeyFromPnpId(lowercaseInput), std::wstring(L"10DE&2684&40BF1458"));

    // Invalid input gives empty
    CHECK_EQ(cd::AdapterKeyFromPnpId(std::wstring(L"not a pnp id")), std::wstring());
    CHECK_EQ(cd::AdapterKeyFromPnpId(std::wstring()), std::wstring());
}

void Test_AG2_FormatAndParsePreferenceValue() {
    Case("AG2 FormatPreferenceValue and AdapterKeyFromPreferenceValue round-trip");
    std::wstring key = L"10DE&2684&40BF1458";
    std::wstring formatted = cd::FormatPreferenceValue(key);
    CHECK_EQ(formatted, std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;"));
    CHECK_EQ(cd::AdapterKeyFromPreferenceValue(formatted), key);

    Case("AG2b reversed key order still parses");
    std::wstring reversed = L"GpuPreference=1073741824;SpecificAdapter=10DE&2684&40BF1458;";
    CHECK_EQ(cd::AdapterKeyFromPreferenceValue(reversed), key);

    Case("AG2c missing trailing semicolon parses");
    std::wstring noTrailingSemi = L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824";
    CHECK_EQ(cd::AdapterKeyFromPreferenceValue(noTrailingSemi), key);

    Case("AG2d non-SpecificAdapter value gives empty");
    CHECK_EQ(cd::AdapterKeyFromPreferenceValue(std::wstring(L"GpuPreference=2;")), std::wstring());
}

void Test_AG3_PlanGpuIsolation() {
    Case("AG3a empty adapter list gives both empty");
    std::vector<cd::GpuAdapter> empty;
    cd::GpuPlan plan = cd::PlanGpuIsolation(empty);
    CHECK(plan.gameKey.empty());
    CHECK(plan.backgroundKey.empty());

    Case("AG3b one adapter gives both empty");
    std::vector<cd::GpuAdapter> one;
    cd::GpuAdapter a1;
    a1.name = L"RTX 4090";
    a1.adapterKey = L"10DE&2684&40BF1458";
    a1.hasDisplay = true;
    a1.vram = 24ull * 1024 * 1024 * 1024;
    one.push_back(a1);
    plan = cd::PlanGpuIsolation(one);
    CHECK(plan.gameKey.empty());
    CHECK(plan.backgroundKey.empty());

    Case("AG3c two adapters, one with display: game on display, background on other");
    std::vector<cd::GpuAdapter> two;
    cd::GpuAdapter a_display;
    a_display.name = L"RTX 4090";
    a_display.adapterKey = L"10DE&2684&40BF1458";
    a_display.hasDisplay = true;
    a_display.vram = 24ull * 1024 * 1024 * 1024;
    two.push_back(a_display);

    cd::GpuAdapter a_noDisplay;
    a_noDisplay.name = L"RTX 3060";
    a_noDisplay.adapterKey = L"10DE&2060&40BF1458";
    a_noDisplay.hasDisplay = false;
    a_noDisplay.vram = 12ull * 1024 * 1024 * 1024;
    two.push_back(a_noDisplay);

    plan = cd::PlanGpuIsolation(two);
    CHECK_EQ(plan.gameKey, std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(plan.backgroundKey, std::wstring(L"10DE&2060&40BF1458"));

    Case("AG3d two adapters both with display: distinct non-empty");
    std::vector<cd::GpuAdapter> bothDisplay;
    a_display.vram = 24ull * 1024 * 1024 * 1024;
    bothDisplay.push_back(a_display);

    cd::GpuAdapter a2_display;
    a2_display.name = L"RTX 5090";
    a2_display.adapterKey = L"10DE&2B85&53021462";
    a2_display.hasDisplay = true;
    a2_display.vram = 32ull * 1024 * 1024 * 1024;
    bothDisplay.push_back(a2_display);

    plan = cd::PlanGpuIsolation(bothDisplay);
    CHECK(!plan.gameKey.empty());
    CHECK(!plan.backgroundKey.empty());
    CHECK_NE(plan.gameKey, plan.backgroundKey);

    Case("AG3e two adapters neither with display: both empty");
    std::vector<cd::GpuAdapter> noneDisplay;
    cd::GpuAdapter no1;
    no1.name = L"GPU1";
    no1.adapterKey = L"1234&5678&9ABC";
    no1.hasDisplay = false;
    no1.vram = 8ull * 1024 * 1024 * 1024;
    noneDisplay.push_back(no1);

    cd::GpuAdapter no2;
    no2.name = L"GPU2";
    no2.adapterKey = L"ABCD&EF01&2345";
    no2.hasDisplay = false;
    no2.vram = 12ull * 1024 * 1024 * 1024;
    noneDisplay.push_back(no2);

    plan = cd::PlanGpuIsolation(noneDisplay);
    CHECK(plan.gameKey.empty());
    CHECK(plan.backgroundKey.empty());

    Case("AG3f two adapters both with display, different vram: larger vram wins game");
    std::vector<cd::GpuAdapter> twoWithDiffVram;
    cd::GpuAdapter small;
    small.name = L"Small VRAM";
    small.adapterKey = L"AAAA&BBBB&CCCC";
    small.hasDisplay = true;
    small.vram = 8ull * 1024 * 1024 * 1024;
    twoWithDiffVram.push_back(small);

    cd::GpuAdapter large;
    large.name = L"Large VRAM";
    large.adapterKey = L"DDDD&EEEE&FFFF";
    large.hasDisplay = true;
    large.vram = 24ull * 1024 * 1024 * 1024;
    twoWithDiffVram.push_back(large);

    plan = cd::PlanGpuIsolation(twoWithDiffVram);
    CHECK_EQ(plan.gameKey, std::wstring(L"DDDD&EEEE&FFFF"));  // larger vram adapter
    CHECK_EQ(plan.backgroundKey, std::wstring(L"AAAA&BBBB&CCCC"));

    Case("AG3g TWO IDENTICAL CARDS SHARE ONE ADAPTER KEY, and the plan must REFUSE");
    {
        // 🔴 THE CASE AG3d ONLY LOOKED LIKE IT COVERED. That case asserts
        // CHECK_NE(gameKey, backgroundKey) on two adapters the TEST gave different keys, so it
        // passes for a reason unrelated to the code and could never have caught this.
        //
        // The adapter key is built from VendorId, DeviceId and SubSysId. Two identical cards -
        // a dual-4090 box, exactly the machine this feature is for - match on all three and
        // therefore produce ONE key. PlanGpuIsolation separates adapters by POINTER, so before
        // the guard it returned gameKey == backgroundKey, every row ticked, every registry
        // write succeeded, and the dialog reported N apps moved onto the card the game was
        // already on. Nothing anywhere disagreed.
        std::vector<cd::GpuAdapter> twins;
        cd::GpuAdapter t1;
        t1.name = L"RTX 4090";
        t1.adapterKey = L"10DE&2684&40BF1458";
        t1.hasDisplay = true;
        t1.vram = 24ull * 1024 * 1024 * 1024;
        twins.push_back(t1);

        cd::GpuAdapter t2 = t1;          // a SECOND physical card, same model, same key
        t2.hasDisplay = false;           // and no display, so it looks like an ideal background
        twins.push_back(t2);

        cd::GpuPlan twinPlan = cd::PlanGpuIsolation(twins);
        CHECK(twinPlan.gameKey.empty());
        CHECK(twinPlan.backgroundKey.empty());
        // and it SAYS why, because an empty plan with no reason is indistinguishable from a
        // machine that simply has one GPU.
        CHECK(!twinPlan.why.empty());
        CHECK(twinPlan.why.find(L"same adapter id") != std::wstring::npos);

        // CONTROL: change ONE character of the second key and the plan must succeed again,
        // so the guard cannot be passing by refusing everything.
        twins[1].adapterKey = L"10DE&2684&40BF1459";
        cd::GpuPlan okPlan = cd::PlanGpuIsolation(twins);
        CHECK_EQ(okPlan.gameKey, std::wstring(L"10DE&2684&40BF1458"));
        CHECK_EQ(okPlan.backgroundKey, std::wstring(L"10DE&2684&40BF1459"));
    }
}

void Test_AG4_ClassifyGpuPref() {
    Case("AG4a Correct: want=stored=running");
    cd::GpuPrefState state = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;"),
        std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Correct));

    Case("AG4b Missing: no stored value");
    state = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(),
        std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Missing));

    Case("AG4c WrongAdapter: stored value is incorrect");
    state = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(L"SpecificAdapter=AAAA&BBBB&CCCC;GpuPreference=1073741824;"),
        std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::WrongAdapter));

    Case("AG4d StaleNotApplied: stored is correct but process on different adapter");
    state = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;"),
        std::wstring(L"DDDD&EEEE&FFFF"));  // different from want
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::StaleNotApplied));

    Case("AG4e Unknown: want or running is empty");
    state = cd::ClassifyGpuPref(
        std::wstring(),  // empty want
        std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;"),
        std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Unknown));

    Case("AG4f StaleNotApplied vs WrongAdapter are distinct");
    // StaleNotApplied: stored value IS correct, but process is elsewhere
    cd::GpuPrefState stale = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;"),
        std::wstring(L"OTHER&ADAPTER&KEY"));
    // WrongAdapter: stored value itself is wrong
    cd::GpuPrefState wrong = cd::ClassifyGpuPref(
        std::wstring(L"10DE&2684&40BF1458"),
        std::wstring(L"SpecificAdapter=WRONG&ADAPT&KEY;GpuPreference=1073741824;"),
        std::wstring(L"10DE&2684&40BF1458"));
    CHECK_EQ(static_cast<int>(stale), static_cast<int>(cd::GpuPrefState::StaleNotApplied));
    CHECK_EQ(static_cast<int>(wrong), static_cast<int>(cd::GpuPrefState::WrongAdapter));
    CHECK_NE(static_cast<int>(stale), static_cast<int>(wrong));
}

void Test_AG5_ConfigRoundTripPreservesUnknownKey() {
    Case("AG5 config round-trip preserves unknown keys in [gpus] section");
    std::wstring configText = L"[general]\r\n"
        L"version=1\r\n"
        L"start_with_windows=0\r\n"
        L"poll_ms=250\r\n"
        L"notifications=0\r\n"
        L"paused=0\r\n"
        L"vcache_original_start=-1\r\n"
        L"first_run_done=0\r\n"
        L"show_vcache_warning=1\r\n"
        L"\r\n"
        L"[masks]\r\n"
        L"\r\n"
        L"[topology]\r\n"
        L"signature=\r\n"
        L"\r\n"
        L"[exclusions]\r\n"
        L"names=\r\n"
        L"\r\n"
        L"[gpus]\r\n"
        L"auto_isolate=1\r\n"
        L"game_gpu=\r\n"
        L"background_gpu=\r\n"
        L"unknown_future_key=future_value\r\n";

    cd::Config c;
    std::wstring error;
    CHECK(cd::ParseConfig(configText, c, &error));

    std::wstring serialized = cd::SerializeConfig(c);
    CHECK(serialized.find(L"unknown_future_key=future_value") != std::wstring::npos);
}

void Test_AH1_FormatRowGpu() {
    Case("AH1a FormatRowGpu: assigned == running, show one name");
    auto nameFor = [](const std::wstring& key) {
        if (key == L"10DE&2B85&53021462") return std::wstring(L"RTX 5090");
        if (key == L"10DE&2684&40BF1458") return std::wstring(L"RTX 4090");
        return std::wstring(L"unknown GPU");
    };

    cd::GpuRow row;
    row.exeName = L"game.exe";
    row.exePath = L"C:\\game\\game.exe";
    row.assignedKey = L"10DE&2B85&53021462";
    row.runningKey = L"10DE&2B85&53021462";
    row.isProfileGame = false;
    row.selected = false;

    std::wstring result = cd::FormatRowGpu(row, nameFor);
    CHECK_EQ(result, std::wstring(L"RTX 5090"));

    Case("AH1b FormatRowGpu: assigned != running, show both with arrow");
    row.assignedKey = L"10DE&2B85&53021462";
    row.runningKey = L"10DE&2684&40BF1458";
    result = cd::FormatRowGpu(row, nameFor);
    CHECK(result.find(L"RTX 5090") != std::wstring::npos);
    CHECK(result.find(L"RTX 4090") != std::wstring::npos);
    CHECK(result.find(L"->") != std::wstring::npos);

    Case("AH1c FormatRowGpu: empty assignedKey shows 'not assigned'");
    row.assignedKey = L"";
    row.runningKey = L"10DE&2684&40BF1458";
    result = cd::FormatRowGpu(row, nameFor);
    CHECK(result.find(L"not assigned") != std::wstring::npos);

    Case("AH1d FormatRowGpu: unknown key shows 'unknown GPU'");
    row.assignedKey = L"UNKNOWN&KEY&HERE";
    row.runningKey = L"10DE&2684&40BF1458";
    result = cd::FormatRowGpu(row, nameFor);
    CHECK(result.find(L"unknown GPU") != std::wstring::npos);

    Case("AH1e FormatRowGpu: an UNMEASURED runningKey is not a mismatch - no arrow");
    // Nothing in the product fills runningKey. Before the fix every assigned row in the
    // Isolate GPU window read "RTX 4090 -> running on unknown GPU" - a disagreement nobody
    // measured, on every row. The earlier cases all set runningKey, so none of them saw it.
    row.assignedKey = L"10DE&2684&40BF1458";
    row.runningKey = L"";
    result = cd::FormatRowGpu(row, nameFor);
    CHECK_EQ(result, std::wstring(L"RTX 4090"));
    CHECK(result.find(L"->") == std::wstring::npos);
    CHECK(result.find(L"unknown GPU") == std::wstring::npos);
    // and with nothing assigned either, it still says so plainly
    row.assignedKey = L"";
    result = cd::FormatRowGpu(row, nameFor);
    CHECK_EQ(result, std::wstring(L"not assigned"));
}

void Test_AH2_RowState() {
    Case("AH2a RowState: Correct - want==stored==running");
    cd::GpuRow row;
    row.exeName = L"app.exe";
    row.exePath = L"C:\\app\\app.exe";
    row.assignedKey = L"10DE&2B85&53021462";
    row.runningKey = L"10DE&2B85&53021462";
    row.isProfileGame = false;
    row.selected = false;

    cd::GpuPrefState state = cd::RowState(row, L"10DE&2B85&53021462");
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Correct));

    Case("AH2b RowState: Missing - no assignedKey");
    row.assignedKey = L"";
    row.runningKey = L"10DE&2B85&53021462";
    state = cd::RowState(row, L"10DE&2B85&53021462");
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Missing));

    Case("AH2c RowState: WrongAdapter - assignedKey is wrong");
    row.assignedKey = L"AAAA&BBBB&CCCC";
    row.runningKey = L"10DE&2B85&53021462";
    state = cd::RowState(row, L"10DE&2B85&53021462");
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::WrongAdapter));

    Case("AH2d RowState: StaleNotApplied - stored correct but running elsewhere");
    row.assignedKey = L"10DE&2B85&53021462";
    row.runningKey = L"DIFFERENT&KEY&HERE";
    state = cd::RowState(row, L"10DE&2B85&53021462");
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::StaleNotApplied));

    Case("AH2e RowState: Unknown - runningKey is empty");
    row.assignedKey = L"10DE&2B85&53021462";
    row.runningKey = L"";
    state = cd::RowState(row, L"10DE&2B85&53021462");
    CHECK_EQ(static_cast<int>(state), static_cast<int>(cd::GpuPrefState::Unknown));

    Case("AH2f RowState: StaleNotApplied vs WrongAdapter are distinct");
    cd::GpuRow staleRow;
    staleRow.assignedKey = L"10DE&2B85&53021462";
    staleRow.runningKey = L"OTHER&ADAPTER&KEY";
    cd::GpuPrefState stale = cd::RowState(staleRow, L"10DE&2B85&53021462");

    cd::GpuRow wrongRow;
    wrongRow.assignedKey = L"WRONG&ADAPT&KEY";
    wrongRow.runningKey = L"10DE&2B85&53021462";
    cd::GpuPrefState wrong = cd::RowState(wrongRow, L"10DE&2B85&53021462");

    CHECK_EQ(static_cast<int>(stale), static_cast<int>(cd::GpuPrefState::StaleNotApplied));
    CHECK_EQ(static_cast<int>(wrong), static_cast<int>(cd::GpuPrefState::WrongAdapter));
    CHECK_NE(static_cast<int>(stale), static_cast<int>(wrong));
}

void Test_AH3_SelectForBackground() {
    Case("AH3a SelectForBackground: isProfileGame=true is NEVER selected");
    std::vector<cd::GpuRow> rows;
    cd::GpuRow row1;
    row1.exeName = L"profile_game.exe";
    row1.assignedKey = L"10DE&2684&40BF1458";
    row1.runningKey = L"10DE&2684&40BF1458";
    row1.isProfileGame = true;
    row1.selected = false;
    rows.push_back(row1);

    std::vector<cd::GpuRow> result = cd::SelectForBackground(rows, L"10DE&2B85&53021462");
    CHECK_EQ(result[0].selected, false);

    Case("AH3b SelectForBackground: already correct on both keys is not selected");
    rows.clear();
    cd::GpuRow row2;
    row2.exeName = L"background_app.exe";
    row2.assignedKey = L"10DE&2B85&53021462";
    row2.runningKey = L"10DE&2B85&53021462";
    row2.isProfileGame = false;
    row2.selected = false;
    rows.push_back(row2);

    result = cd::SelectForBackground(rows, L"10DE&2B85&53021462");
    CHECK_EQ(result[0].selected, false);

    Case("AH3c SelectForBackground: wrong row IS selected");
    rows.clear();
    cd::GpuRow row3;
    row3.exeName = L"wrong_app.exe";
    row3.assignedKey = L"10DE&2684&40BF1458";
    row3.runningKey = L"10DE&2684&40BF1458";
    row3.isProfileGame = false;
    row3.selected = false;
    rows.push_back(row3);

    result = cd::SelectForBackground(rows, L"10DE&2B85&53021462");
    CHECK_EQ(result[0].selected, true);

    Case("AH3d SelectForBackground: empty backgroundKey selects nothing");
    rows.clear();
    row3.exeName = L"any_app.exe";
    row3.assignedKey = L"10DE&2684&40BF1458";
    row3.runningKey = L"10DE&2684&40BF1458";
    row3.isProfileGame = false;
    row3.selected = false;
    rows.push_back(row3);

    result = cd::SelectForBackground(rows, L"");
    CHECK_EQ(result[0].selected, false);

    Case("AH3e runningKey UNKNOWN and already assigned to the background GPU: NOT re-selected");
    // 🔴 The skip used to require runningKey == backgroundKey too, and runningKey has no
    // producer in this product, so the bulk action re-selected - and Apply rewrote - every app
    // that was already where it should be. Every earlier case here set runningKey.
    const std::wstring bg = L"10DE&2B85&53021462";
    rows.clear();
    cd::GpuRow onBg;
    onBg.exeName = L"already_moved.exe";
    onBg.assignedKey = bg;
    onBg.runningKey = L"";
    rows.push_back(onBg);
    result = cd::SelectForBackground(rows, bg);
    CHECK_EQ(result[0].selected, false);

    Case("AH3f runningKey unknown, assigned ELSEWHERE: selected");
    rows.clear();
    cd::GpuRow elsewhere = onBg;
    elsewhere.exeName = L"on_game_gpu.exe";
    elsewhere.assignedKey = L"10DE&2684&40BF1458";
    rows.push_back(elsewhere);
    result = cd::SelectForBackground(rows, bg);
    CHECK_EQ(result[0].selected, true);

    Case("AH3g runningKey unknown, NOTHING assigned: selected");
    rows.clear();
    cd::GpuRow bare = onBg;
    bare.exeName = L"never_assigned.exe";
    bare.assignedKey = L"";
    rows.push_back(bare);
    result = cd::SelectForBackground(rows, bg);
    CHECK_EQ(result[0].selected, true);

    Case("AH3h CONTROL - runningKey KNOWN and different: selected even though assigned is right");
    // Without this, AH3e would pass on a rule that ignored runningKey altogether. When the
    // running adapter IS a measured fact, registry and process must both agree, as before.
    rows.clear();
    cd::GpuRow stale = onBg;
    stale.exeName = L"stale.exe";
    stale.assignedKey = bg;
    stale.runningKey = L"10DE&2684&40BF1458";
    rows.push_back(stale);
    result = cd::SelectForBackground(rows, bg);
    CHECK_EQ(result[0].selected, true);
}

// "Select all" (v0.5.9): the SAFE bulk tick. gpuwindow.cpp is not in this build, so the rule lives in gpu_rows.h
// where these can reach it - deleting any one of its three exceptions there must turn one of these red.
void Test_AH4_SelectAllTicks() {
    const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
    // The sibling pair: same install root, same version-folder stem, so AnotherVersionMayHoldMainGpuPin can see them.
    const std::wstring oldPath = L"C:\\SelectAllTest\\Chat\\app-1.0\\chat.exe";
    const std::wstring newPath = L"C:\\SelectAllTest\\Chat\\app-2.0\\chat.exe";
    typedef std::vector<std::pair<std::wstring, std::wstring> > Pairs;
    const Pairs none;

    Case("AH4a Select all: a listed, ordinary application IS ticked");
    cd::GpuRow plain;
    plain.exeName = L"editor.exe";
    plain.exePath = L"C:\\SelectAllTest\\Editor\\editor.exe";
    CHECK_EQ(cd::SelectAllTicks(plain, true, false, mainKey, none), true);

    Case("AH4b Select all: a Windows or excluded image is NEVER ticked");
    CHECK_EQ(cd::SelectAllTicks(plain, true, true, mainKey, none), false);

    Case("AH4c Select all: a game the user has a profile for is NEVER ticked");
    cd::GpuRow game = plain;
    game.isProfileGame = true;
    CHECK_EQ(cd::SelectAllTicks(game, true, false, mainKey, none), false);

    Case("AH4d Select all: a LIVE main-GPU pin is NEVER ticked, and the same row on the background GPU is");
    cd::GpuRow pinned = plain;
    pinned.assignedKey = mainKey;
    CHECK_EQ(cd::SelectAllTicks(pinned, true, false, mainKey, none), false);
    // CONTROL: without it AH4d would pass on a rule that refused every assigned row, pin or not.
    cd::GpuRow onBackground = plain;
    onBackground.assignedKey = bgKey;
    CHECK_EQ(cd::SelectAllTicks(onBackground, true, false, mainKey, none), true);
    // A pin lost to an auto-update is still the user's pin: no value of its own, lostKey names the main GPU.
    cd::GpuRow lost = plain;
    lost.lostKey = mainKey;
    CHECK_EQ(cd::SelectAllTicks(lost, true, false, mainKey, none), false);

    Case("AH4e Select all: an application a SIBLING install of which may hold a main-GPU pin is NEVER ticked");
    cd::GpuRow sibling;
    sibling.exeName = L"chat.exe";
    sibling.exePath = newPath;          // no value of its own
    Pairs reg;
    reg.push_back(std::make_pair(oldPath, mainKey));
    CHECK_EQ(cd::SelectAllTicks(sibling, true, false, mainKey, reg), false);
    // CONTROL: the same sibling pinned to the BACKGROUND GPU is no reason to skip this row.
    Pairs bgReg;
    bgReg.push_back(std::make_pair(oldPath, bgKey));
    CHECK_EQ(cd::SelectAllTicks(sibling, true, false, mainKey, bgReg), true);

    Case("AH4f Select all: a row that is not in the list box is NEVER ticked");
    CHECK_EQ(cd::SelectAllTicks(plain, false, false, mainKey, none), false);

    Case("AH4g Select all: no target is needed, and the main GPU being chosen changes nothing");
    // It is NOT Auto assign: SelectForAutoAssign refuses everything with no target and while the main GPU is the
    // target (AutoAssignAllowed). Select all takes no target at all, so there is nothing to refuse on.
    CHECK_EQ(cd::SelectAllTicks(plain, true, false, std::wstring(), none), true);
    // ...and it does not ask "would this row actually move": a row ALREADY on the GPU it would be sent to is still
    // safe to tick, which is exactly where SelectForBackground says no.
    std::vector<cd::GpuRow> one(1, onBackground);
    CHECK_EQ(cd::SelectForBackground(one, bgKey, mainKey)[0].selected, false);
    CHECK_EQ(cd::SelectAllTicks(onBackground, true, false, mainKey, none), true);

    Case("AH4h SelectAllPendingCount: an already-ticked row does not count, an empty list counts nothing");
    std::vector<cd::GpuRow> rows;
    rows.push_back(plain);          // eligible, unticked  -> counts
    rows.push_back(game);           // profile game        -> never
    rows.push_back(pinned);         // main-GPU pin        -> never
    cd::GpuRow already = plain;
    already.exeName = L"ticked.exe";
    already.selected = true;        // eligible but ALREADY ticked -> does not count
    rows.push_back(already);
    std::vector<bool> listed(4, true), system(4, false);
    CHECK_EQ(cd::SelectAllPendingCount(rows, listed, system, mainKey, none), static_cast<size_t>(1));
    system[0] = true;               // the one that counted is now a Windows image
    CHECK_EQ(cd::SelectAllPendingCount(rows, listed, system, mainKey, none), static_cast<size_t>(0));
    const std::vector<cd::GpuRow> empty;
    const std::vector<bool> noFlags;
    CHECK_EQ(cd::SelectAllPendingCount(empty, noFlags, noFlags, mainKey, none), static_cast<size_t>(0));
    // FAIL CLOSED: flags shorter than the rows read as "not listed" and "system", so a caller that cannot supply
    // them ticks nothing rather than everything.
    CHECK_EQ(cd::SelectAllPendingCount(rows, noFlags, noFlags, mainKey, none), static_cast<size_t>(0));
}

// ===========================================================================
// == AJ. GPU assignment policy - every case here was FOUND on the operator's real machine ==
// ===========================================================================
//
// A read-only probe of the feature's data path, run on 2026-09-12 before the Isolate GPU window
// existed, returned four adapters (RTX 5090 with display, AMD Radeon iGPU 2 GB, RTX 4090, and a
// SECOND RTX 5090 entry with the same key), 16 registry preferences all pointing at the 4090, and
// three "orphans" of which one was a different application. None of the 2800 tests that existed
// at the time could see any of it, because every one of them used adapters and paths the test
// author invented. These use the machine's real shapes.

std::vector<cd::GpuAdapter> RealMachineAdapters() {
    // 🔴 `a` IS REUSED, SO EVERY FIELD IS SET ON EVERY ENTRY - kind included, or an entry silently inherits the
    // previous entry's. Kinds as measured by igpuprobe (DXCore IsIntegrated by LUID): the second RTX 5090 entry is
    // not known to DXCore at all.
    std::vector<cd::GpuAdapter> v;
    cd::GpuAdapter a;
    a.name = L"NVIDIA GeForce RTX 5090";   a.adapterKey = L"10DE&2B85&53021462";
    a.hasDisplay = true;  a.vram = 31ull << 30;  a.kind = cd::GpuKind::Discrete;    v.push_back(a);
    a.name = L"AMD Radeon(TM) Graphics";   a.adapterKey = L"1002&13C0&88771043";
    a.hasDisplay = false; a.vram = 2ull << 30;   a.kind = cd::GpuKind::Integrated;  v.push_back(a);
    a.name = L"NVIDIA GeForce RTX 4090";   a.adapterKey = L"10DE&2684&40BF1458";
    a.hasDisplay = false; a.vram = 22ull << 30;  a.kind = cd::GpuKind::Discrete;    v.push_back(a);
    a.name = L"NVIDIA GeForce RTX 5090";   a.adapterKey = L"10DE&2B85&53021462";
    a.hasDisplay = false; a.vram = 31ull << 30;  a.kind = cd::GpuKind::Unknown;     v.push_back(a);
    return v;
}

// A registry and a restore file for RunGpuEdits, held in memory. Every call is logged in the order it was made,
// so a test can see that a row is recorded before the next row is written.
struct FakeGpuEdits {
    std::map<std::wstring, std::wstring> reg;
    std::set<std::wstring> unreadableNow;                                // every read of these fails
    std::set<std::wstring> unreadableAfterWrite;                         // reads fail once written
    std::map<std::wstring, cd::GuardedWriteResult> result;               // a forced write result
    std::map<std::wstring, unsigned long> error;                         // ...and its error code
    std::map<std::wstring, std::wstring> otherProgram;                   // written by "another program" as the forced result happens
    std::set<std::wstring> recordFails;
    std::map<std::wstring, std::wstring> afterWrite;                     // what reads back after a write lands
    std::vector<std::wstring> log;
    std::vector<cd::GpuPreferenceBefore> recorded;
    std::set<std::wstring> written;
    std::set<std::wstring> automaticWrites;                              // written with GpuEditItem::automatic set

    cd::GpuEditOps Ops() {
        cd::GpuEditOps ops;
        ops.write = [this](const std::wstring& path, bool expectPresent, const std::wstring& expectValue, bool del,
                           const std::wstring& value, unsigned long& err, bool automatic) {
            log.push_back(L"write " + path + (del ? std::wstring(L" delete") : L" set " + value));
            if (automatic) automaticWrites.insert(path);
            err = 0;
            const std::map<std::wstring, cd::GuardedWriteResult>::const_iterator forced = result.find(path);
            if (forced != result.end()) {
                const std::map<std::wstring, std::wstring>::const_iterator o = otherProgram.find(path);
                if (o != otherProgram.end()) reg[path] = o->second;
                const std::map<std::wstring, unsigned long>::const_iterator e = error.find(path);
                if (e != error.end()) err = e->second;
                return forced->second;
            }
            // otherwise the guard's own rule: nothing is written unless the value is still as read
            const std::map<std::wstring, std::wstring>::const_iterator cur = reg.find(path);
            const bool present = cur != reg.end();
            if (present != expectPresent || (present && cur->second != expectValue)) return cd::GuardedWriteResult::Changed;
            if (del) reg.erase(path); else reg[path] = value;
            const std::map<std::wstring, std::wstring>::const_iterator ov = afterWrite.find(path);
            if (ov != afterWrite.end()) reg[path] = ov->second;   // another program, the same instant
            written.insert(path);
            return cd::GuardedWriteResult::Written;
        };
        ops.read = [this](const std::wstring& path, std::wstring& value, bool& unreadable) {
            log.push_back(L"read " + path);
            value.clear();
            unreadable = unreadableNow.count(path) != 0 ||
                         (written.count(path) != 0 && unreadableAfterWrite.count(path) != 0);
            if (unreadable) return false;
            const std::map<std::wstring, std::wstring>::const_iterator cur = reg.find(path);
            if (cur == reg.end()) return false;
            value = cur->second;
            return true;
        };
        ops.record = [this](const cd::GpuPreferenceBefore& row) {
            log.push_back(L"record " + row.exePath);
            if (recordFails.count(row.exePath)) return false;
            recorded.push_back(row);
            return true;
        };
        return ops;
    }

    std::vector<cd::GpuEditItem> Items(const std::vector<std::wstring>& paths) const {
        std::vector<cd::GpuEditItem> items;
        for (size_t i = 0; i < paths.size(); ++i) {
            cd::GpuEditItem it;
            it.exePath = paths[i];
            const std::map<std::wstring, std::wstring>::const_iterator cur = reg.find(paths[i]);
            it.present = cur != reg.end();
            if (it.present) it.existing = cur->second;
            items.push_back(it);
        }
        return items;
    }

    // Position of the first log line starting with `prefix`, or the log's size.
    size_t At(const std::wstring& prefix) const {
        for (size_t i = 0; i < log.size(); ++i)
            if (log[i].compare(0, prefix.size(), prefix) == 0) return i;
        return log.size();
    }
};

void Test_AJ_GpuPolicy() {
    Case("AJ1 the plan never picks a SECOND entry of the game GPU as background");
    {
        // DXGI lists the operator's 5090 twice. With the duplicate enumerated BEFORE any other
        // display-less adapter, a pointer-only skip picked it, and the plan either pinned apps
        // to the game's own card or refused outright. Order is DXGI's choice, not ours.
        std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        std::vector<cd::GpuAdapter> dupFirst;
        dupFirst.push_back(v[0]); dupFirst.push_back(v[3]);
        dupFirst.push_back(v[1]); dupFirst.push_back(v[2]);
        const cd::GpuPlan p = cd::PlanGpuIsolation(dupFirst);
        CHECK_EQ(p.gameKey, std::wstring(L"10DE&2B85&53021462"));
        CHECK(!p.backgroundKey.empty());
        CHECK_NE(p.backgroundKey, p.gameKey);

        // and with ONLY the game card plus its own duplicate, it refuses and names why
        std::vector<cd::GpuAdapter> onlyDup;
        onlyDup.push_back(v[0]); onlyDup.push_back(v[3]);
        const cd::GpuPlan q = cd::PlanGpuIsolation(onlyDup);
        CHECK(q.backgroundKey.empty());
        CHECK(q.why.find(L"same adapter id") != std::wstring::npos);
    }

    Case("AJ2 candidates exclude the game GPU by KEY, so its duplicate is never offered");
    {
        const std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        const cd::GpuPlan p = cd::PlanGpuIsolation(v);
        const std::vector<std::wstring> c = cd::CandidateBackgroundKeys(v, p);
        CHECK_EQ((int)c.size(), 2);
        if (c.size() == 2) {
            CHECK_EQ(c[0], std::wstring(L"1002&13C0&88771043"));
            CHECK_EQ(c[1], std::wstring(L"10DE&2684&40BF1458"));
        }
        cd::GpuPlan undecidable;
        CHECK(cd::CandidateBackgroundKeys(v, undecidable).empty());
    }

    Case("AJ3 the default background GPU follows the user's own pins, not DXGI order");
    {
        // The plan's first display-less adapter is the 2 GB iGPU. Every preference the operator
        // set by hand is on the 4090. The default must be the 4090.
        const std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        const cd::GpuPlan p = cd::PlanGpuIsolation(v);
        CHECK_EQ(p.backgroundKey, std::wstring(L"1002&13C0&88771043"));   // the plan alone picks the iGPU
        std::vector<std::pair<std::wstring, std::wstring> > reg;
        reg.push_back(std::make_pair(std::wstring(L"C:\\a\\claude.exe"), std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(std::wstring(L"C:\\b\\obs64.exe"), std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(std::wstring(L"C:\\c\\x.exe"), std::wstring(L"")));
        CHECK_EQ(cd::ChooseBackgroundKey(v, p, reg), std::wstring(L"10DE&2684&40BF1458"));

        // CONTROL: no history at all keeps the plan's own pick
        std::vector<std::pair<std::wstring, std::wstring> > none;
        CHECK_EQ(cd::ChooseBackgroundKey(v, p, none), std::wstring(L"1002&13C0&88771043"));

        // CONTROL: a pin to the GAME gpu is not a vote for a background gpu
        std::vector<std::pair<std::wstring, std::wstring> > gamePins;
        gamePins.push_back(std::make_pair(std::wstring(L"C:\\g\\game.exe"), std::wstring(L"10DE&2B85&53021462")));
        CHECK_EQ(cd::ChooseBackgroundKey(v, p, gamePins), std::wstring(L"1002&13C0&88771043"));

        // CONTROL: a TIE keeps the plan's pick rather than flipping on noise
        std::vector<std::pair<std::wstring, std::wstring> > tie;
        tie.push_back(std::make_pair(std::wstring(L"C:\\1\\a.exe"), std::wstring(L"10DE&2684&40BF1458")));
        tie.push_back(std::make_pair(std::wstring(L"C:\\2\\b.exe"), std::wstring(L"1002&13C0&88771043")));
        CHECK_EQ(cd::ChooseBackgroundKey(v, p, tie), std::wstring(L"1002&13C0&88771043"));

        // and an undecidable plan stays undecidable whatever the registry says
        cd::GpuPlan undecidable;
        CHECK(cd::ChooseBackgroundKey(v, undecidable, reg).empty());
    }

    Case("AJ4 Windows binaries are recognised, and look-alike folders are not");
    {
        CHECK(cd::IsWindowsImagePath(L"C:\\Windows\\System32\\dwm.exe"));
        CHECK(cd::IsWindowsImagePath(L"c:\\windows\\explorer.exe"));
        CHECK(cd::IsWindowsImagePath(L"D:\\WINDOWS\\SystemApps\\x.exe"));
        CHECK(!cd::IsWindowsImagePath(L"C:\\WindowsApps\\x.exe"));
        CHECK(!cd::IsWindowsImagePath(L"C:\\Users\\u\\AppData\\Local\\AnthropicClaude\\app-1.52386.3\\claude.exe"));
        CHECK(!cd::IsWindowsImagePath(L"C:\\Program Files\\OBS\\obs64.exe"));
        CHECK(!cd::IsWindowsImagePath(L""));
    }

    Case("AJ5 same install root: two versions of one app yes, two apps sharing a file name no");
    {
        const std::wstring stale = L"C:\\Users\\u\\AppData\\Local\\AnthropicClaude\\app-1.49585.0\\claude.exe";
        const std::wstring desktop = L"C:\\Users\\u\\AppData\\Local\\AnthropicClaude\\app-1.52386.3\\claude.exe";
        const std::wstring cli = L"C:\\Users\\u\\AppData\\Roaming\\Claude\\claude-code\\2.1.266\\claude.exe";
        CHECK(cd::SameInstallRoot(stale, desktop));
        CHECK(!cd::SameInstallRoot(stale, cli));
        CHECK(cd::SameInstallRoot(L"C:\\P\\EdgeWebView\\Application\\152.0.4191.62\\msedgewebview2.exe",
                                  L"C:\\P\\EdgeWebView\\Application\\152.0.4191.66\\msedgewebview2.exe"));
        CHECK(!cd::SameInstallRoot(L"x.exe", L"x.exe"));   // too shallow to have a root
    }

    Case("AJ6 the orphan detector on the machine's real data: Claude Code is NOT an orphan");
    {
        // The exact shape the probe returned. Before the fix the detector reported THREE
        // orphans, one of them pairing Claude Desktop's old folder with Claude Code's binary.
        const std::wstring root = L"C:\\Users\\u\\AppData\\Local\\AnthropicClaude\\";
        const std::wstring cli = L"C:\\Users\\u\\AppData\\Roaming\\Claude\\claude-code\\2.1.266\\claude.exe";
        const std::wstring edgeOld = L"C:\\P\\EdgeWebView\\Application\\152.0.4191.62\\msedgewebview2.exe";
        const std::wstring edgeNew = L"C:\\P\\EdgeWebView\\Application\\152.0.4191.66\\msedgewebview2.exe";
        std::vector<std::pair<std::wstring, std::wstring> > reg;
        reg.push_back(std::make_pair(root + L"claude.exe", std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(root + L"app-1.24012.9\\claude.exe", std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(root + L"app-1.26832.0\\claude.exe", std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(root + L"app-1.49585.0\\claude.exe", std::wstring(L"10DE&2684&40BF1458")));
        reg.push_back(std::make_pair(edgeOld, std::wstring(L"10DE&2684&40BF1458")));
        std::vector<std::wstring> running;
        running.push_back(root + L"app-1.52386.3\\claude.exe");
        running.push_back(cli);
        running.push_back(edgeNew);
        const std::wstring launcher = root + L"claude.exe";
        const std::vector<cd::OrphanedAssignment> o = cd::FindOrphanedAssignments(
            reg, running,
            [&](const std::wstring& p) {
                return cd::WcsIcmp(p, launcher) || cd::WcsIcmp(p, running[0]) ||
                       cd::WcsIcmp(p, cli) || cd::WcsIcmp(p, edgeNew);
            });
        CHECK_EQ((int)o.size(), 2);
        bool desktop = false, edge = false, cliFlagged = false;
        for (size_t i = 0; i < o.size(); ++i) {
            if (cd::WcsIcmp(o[i].livePath, running[0])) desktop = true;
            if (cd::WcsIcmp(o[i].livePath, edgeNew)) edge = true;
            if (cd::WcsIcmp(o[i].livePath, cli)) cliFlagged = true;
        }
        CHECK(desktop);
        CHECK(edge);
        CHECK(!cliFlagged);
    }

    Case("AJ7 driver components are protected by the SHIPPED exclusions, not only the user's list");
    {
        // FAILURE-BEFORE-FIX CONTROL, built from the operator's real list. [M] Their config.ini names
        // 23 exclusions and neither amdow.exe nor AMDRSSrcExt.exe - both joined the defaults after
        // that config was written - so "Game Optimize for GPU" ticked two AMD driver components.
        cd::Config theirs;
        const wchar_t* names[] = { L"EasyAntiCheat.exe", L"audiodg.exe", L"dwm.exe", L"csrss.exe",
                                   L"NVDisplay.Container.exe", L"nvcontainer.exe", L"AMDRSServ.exe",
                                   L"RadeonSoftware.exe", L"GameOptimizer.exe" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) theirs.exclusions.push_back(names[i]);
        CHECK(!theirs.IsExcluded(L"amdow.exe"));        // the user's own list misses it...
        CHECK(!theirs.IsExcluded(L"AMDRSSrcExt.exe"));
        CHECK(cd::IsDefaultExcluded(L"amdow.exe"));      // ...the shipped list does not
        CHECK(cd::IsDefaultExcluded(L"AMDRSSrcExt.exe"));
        CHECK(cd::IsDefaultExcluded(L"AMDRSSRCEXT.EXE"));      // case-insensitive, as Windows is
        CHECK(cd::IsDefaultExcluded(L"amd3dvcacheSvc.exe"));   // a trailing-* prefix entry
        // CONTROL: ordinary applications are not protected, so the check is not refusing everything
        CHECK(!cd::IsDefaultExcluded(L"claude.exe"));
        CHECK(!cd::IsDefaultExcluded(L"firefox.exe"));
    }

    Case("AJ48 two Microsoft components a hand test found are protected by the SHIPPED exclusions too");
    {
        // FAILURE-BEFORE-FIX CONTROL, built from the operator's own hand test of the previous
        // build. [M] They applied the GPU feature to eight applications and two were Microsoft
        // system components - Defender's session helper under ProgramData, GameInput's
        // redistributable service under Program Files. IsWindowsImagePath matches only an image
        // under "X:\windows\", so neither was ever classified as a Windows component, and a
        // config.ini written before these names joined the defaults cannot protect them either.
        cd::Config theirs;
        const wchar_t* names[] = { L"EasyAntiCheat.exe", L"audiodg.exe", L"dwm.exe", L"csrss.exe",
                                   L"NVDisplay.Container.exe", L"nvcontainer.exe", L"AMDRSServ.exe",
                                   L"RadeonSoftware.exe", L"MsMpEng.exe", L"GameOptimizer.exe" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) theirs.exclusions.push_back(names[i]);
        CHECK(!theirs.IsExcluded(L"DefenderSessionHelper.exe"));    // the user's older list misses both...
        CHECK(!theirs.IsExcluded(L"GameInputRedistService.exe"));
        CHECK(cd::IsDefaultExcluded(L"DefenderSessionHelper.exe"));  // ...the shipped list does not
        CHECK(cd::IsDefaultExcluded(L"GameInputRedistService.exe"));
        // The list matches a BARE FILE NAME, case-insensitively, so whatever casing a process
        // reports and the full path a row carries both reach the same entry.
        CHECK(cd::IsDefaultExcluded(L"defendersessionhelper.exe"));
        CHECK(cd::IsDefaultExcluded(L"GAMEINPUTREDISTSERVICE.EXE"));
        CHECK(cd::IsDefaultExcluded(
            L"C:\\ProgramData\\Microsoft\\Windows Defender\\DefenderSessionHelper.exe"));
        CHECK(cd::IsDefaultExcluded(
            L"C:\\Program Files\\Microsoft GameInput\\x64\\GameInputRedistService.exe"));
        // ...and THE REASON the name list has to be the mechanism: neither real path is under
        // the Windows directory, so the path rule cannot see them however it is read.
        CHECK(!cd::IsWindowsImagePath(
            L"C:\\ProgramData\\Microsoft\\Windows Defender\\DefenderSessionHelper.exe"));
        CHECK(!cd::IsWindowsImagePath(
            L"C:\\Program Files\\Microsoft GameInput\\x64\\GameInputRedistService.exe"));
        // CONTROL: both entries are exact names, not prefixes, and ordinary applications stay free.
        CHECK(!cd::IsDefaultExcluded(L"DefenderSessionHelper-helper.exe"));
        CHECK(!cd::IsDefaultExcluded(L"GameInputRedist.exe"));
        CHECK(!cd::IsDefaultExcluded(L"claude.exe"));
    }

    Case("AJ8 two rows with one file name are told apart by their folders");
    {
        // The window showed claude.exe twice - Claude Desktop and Claude Code - as identical lines.
        // A backslash is BUILT here rather than typed, because every tool that has edited this file
        // today has mangled a typed one at least once.
        const std::wstring B(1, wchar_t(92));
        const std::wstring desktop = L"C:" + B + L"Users" + B + L"u" + B + L"AppData" + B + L"Local" +
                                     B + L"AnthropicClaude" + B + L"app-1.52386.3" + B + L"claude.exe";
        const std::wstring cli = L"C:" + B + L"Users" + B + L"u" + B + L"AppData" + B + L"Roaming" +
                                 B + L"Claude" + B + L"claude-code" + B + L"2.1.266" + B + L"claude.exe";
        CHECK_EQ(cd::ParentFoldersOf(desktop, 2), L"AnthropicClaude" + B + L"app-1.52386.3");
        CHECK_EQ(cd::ParentFoldersOf(cli, 2), L"claude-code" + B + L"2.1.266");
        CHECK(cd::ParentFoldersOf(desktop, 2) != cd::ParentFoldersOf(cli, 2));   // the whole point
        CHECK_EQ(cd::ParentFoldersOf(L"C:" + B + L"a" + B + L"x.exe", 1), std::wstring(L"a"));
        // too shallow for the levels asked: empty, never a truncated or wrong fragment
        CHECK_EQ(cd::ParentFoldersOf(L"C:" + B + L"x.exe", 2), std::wstring());
        CHECK_EQ(cd::ParentFoldersOf(L"x.exe", 1), std::wstring());
    }

    Case("AJ9 Apply keeps every field of the existing value it does not own");
    {
        // Found by adversarial review: Apply wrote over the WHOLE value, destroying any other field
        // Windows keeps for that application in the same string.
        const std::wstring key = L"10DE&2684&40BF1458";
        const std::wstring existing = L"SwapEffectUpgradeEnable=1;GpuPreference=2;AutoHDREnable=2097;";
        const std::wstring merged = cd::MergeGpuPreferenceValue(existing, key);
        CHECK(merged.find(L"SpecificAdapter=10DE&2684&40BF1458;") == 0);
        CHECK(merged.find(L"GpuPreference=1073741824;") != std::wstring::npos);
        CHECK(merged.find(L"SwapEffectUpgradeEnable=1;") != std::wstring::npos);
        CHECK(merged.find(L"AutoHDREnable=2097;") != std::wstring::npos);
        // the old choice is REPLACED, not left beside the new one
        CHECK(merged.find(L"GpuPreference=2;") == std::wstring::npos);
        // an absent value gives exactly what the feature always wrote
        CHECK_EQ(cd::MergeGpuPreferenceValue(L"", key), cd::FormatPreferenceValue(key));
        // re-applying is idempotent - no second SpecificAdapter
        CHECK_EQ(cd::MergeGpuPreferenceValue(merged, key), merged);
        // a different card replaces the first rather than adding to it
        const std::wstring moved = cd::MergeGpuPreferenceValue(merged, L"1002&13C0&88771043");
        CHECK(moved.find(L"10DE&2684&40BF1458") == std::wstring::npos);
        CHECK(moved.find(L"AutoHDREnable=2097;") != std::wstring::npos);
    }

    Case("AJ10 Remove strips only the GPU fields, and empties only when nothing else is there");
    {
        CHECK_EQ(cd::StripGpuAssignment(
                     L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;AutoHDREnable=2097;"),
                 std::wstring(L"AutoHDREnable=2097;"));
        // the value the feature itself writes strips to nothing, so Remove deletes it
        CHECK_EQ(cd::StripGpuAssignment(cd::FormatPreferenceValue(L"10DE&2684&40BF1458")), std::wstring());
        CHECK_EQ(cd::StripGpuAssignment(L"specificadapter=X;GPUPREFERENCE=1;"), std::wstring());
        CHECK_EQ(cd::StripGpuAssignment(L""), std::wstring());
        // a last field with no trailing ';' still survives, written back terminated
        CHECK_EQ(cd::StripGpuAssignment(L"GpuPreference=2;AutoHDREnable=2097"),
                 std::wstring(L"AutoHDREnable=2097;"));
        // the helper drops fragments that are not name=value - which is exactly why Apply and Remove
        // never hand it such a value: IsEditablePreferenceValue refuses it first (AJ16)
        CHECK_EQ(cd::StripGpuAssignment(L";;=5;junk;GpuPreference=1;"), std::wstring());
    }

    Case("AJ11 a failed display query refuses the plan instead of guessing");
    {
        // Found by adversarial review: every EnumOutputs error used to count as a display, which
        // could make the wrong card the game GPU.
        std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        CHECK(!cd::PlanGpuIsolation(v).backgroundKey.empty());      // control: answers normally
        v[2].displayUnknown = true;                                   // the 4090 could not answer
        const cd::GpuPlan p = cd::PlanGpuIsolation(v);
        CHECK(p.gameKey.empty());
        CHECK(p.backgroundKey.empty());
        CHECK(p.why.find(L"NVIDIA GeForce RTX 4090") != std::wstring::npos);
    }

    Case("AJ12 sibling installs sharing a root and an exe name are not one app");
    {
        // Found by adversarial review: a shared install root alone was taken as "same app".
        CHECK(!cd::SameInstallRoot(L"C:\\Program Files\\OldApp\\helper.exe",
                                   L"C:\\Program Files\\OtherApp\\helper.exe"));
        CHECK(!cd::SameInstallRoot(L"C:\\Users\\U\\AppData\\Local\\Discord\\Update.exe",
                                   L"C:\\Users\\U\\AppData\\Local\\Slack\\Update.exe"));
        CHECK(!cd::SameInstallRoot(L"C:\\A\\x.exe", L"C:\\B\\x.exe"));            // drive-root installs
        CHECK(!cd::SameInstallRoot(L"C:\\P\\app-1.2\\x.exe", L"C:\\P\\tools\\x.exe"));  // version vs plain
        CHECK(!cd::SameInstallRoot(L"C:\\P\\app-1.2\\x.exe", L"C:\\P\\beta-1.3\\x.exe")); // two products
        // controls: real version moves still match
        CHECK(cd::SameInstallRoot(L"C:\\P\\app-1.9\\x.exe", L"C:\\P\\App-1.10\\x.exe"));
        CHECK(cd::SameInstallRoot(L"C:\\P\\2.1.266\\x.exe", L"C:\\P\\2.1.270\\x.exe"));

        const auto missing = [](const std::wstring&) { return false; };
        std::vector<std::pair<std::wstring, std::wstring> > reg;
        reg.push_back({L"C:\\Program Files\\OldApp\\helper.exe", L"10DE&2684&40BF1458"});
        const std::vector<std::wstring> live = {L"C:\\Program Files\\OtherApp\\helper.exe"};
        CHECK(cd::FindOrphanedAssignments(reg, live, missing).empty());
    }

    Case("AJ13 the newest stale version is chosen by number, and ruled-out entries touch no disk");
    {
        const auto missing = [](const std::wstring&) { return false; };
        std::vector<std::pair<std::wstring, std::wstring> > reg;
        reg.push_back({L"C:\\P\\app-10\\x.exe", L"10DE&2684&40BF1458"});
        reg.push_back({L"C:\\P\\app-9\\x.exe", L"1002&13C0&88771043"});
        const std::vector<std::wstring> live = {L"C:\\P\\app-11\\x.exe"};
        const std::vector<cd::OrphanedAssignment> o = cd::FindOrphanedAssignments(reg, live, missing);
        CHECK_EQ((int)o.size(), 1);
        if (o.size() == 1) {
            // plain string order would have picked app-9, and with it the WRONG lost GPU
            CHECK_EQ(o[0].stalePath, std::wstring(L"C:\\P\\app-10\\x.exe"));
            CHECK_EQ(o[0].lostKey, std::wstring(L"10DE&2684&40BF1458"));
        }
        CHECK(cd::NaturalPathLess(L"app-9", L"app-10"));
        CHECK(!cd::NaturalPathLess(L"app-10", L"app-9"));
        CHECK(!cd::NaturalPathLess(L"APP-1", L"app-1"));
        CHECK(!cd::NaturalPathLess(L"app-1", L"APP-1"));

        // A disk probe can block for seconds on an offline share, so none is made for an entry
        // whose exe name or install root already rules it out.
        int probes = 0;
        const auto counting = [&probes](const std::wstring&) { ++probes; return false; };
        std::vector<std::pair<std::wstring, std::wstring> > many;
        for (int i = 0; i < 20; ++i)
            many.push_back({L"\\\\server\\share\\tool" + std::to_wstring(i) + L"\\other.exe",
                            L"10DE&2684&40BF1458"});
        many.push_back({L"\\\\server\\share\\app-3\\x.exe", L"10DE&2684&40BF1458"});  // same name, other root
        const std::vector<std::wstring> running = {L"C:\\P\\app-11\\x.exe"};
        CHECK(cd::FindOrphanedAssignments(many, running, counting).empty());
        CHECK_EQ(probes, 0);
        // control: a genuine candidate IS probed, exactly once
        many.push_back({L"C:\\P\\app-10\\x.exe", L"10DE&2684&40BF1458"});
        CHECK_EQ((int)cd::FindOrphanedAssignments(many, running, counting).size(), 1);
        CHECK_EQ(probes, 1);
    }

    Case("AJ14 the adapter key comes from the whole SpecificAdapter field only, and is validated");
    {
        // Found by adversarial review: a substring search let NotSpecificAdapter= count, refused a
        // last field with no ';', and accepted any text with two '&' in it.
        const std::wstring k = L"10DE&2684&40BF1458";
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"AutoHDREnable=1;SpecificAdapter=10DE&2684&40BF1458"), k);
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"NotSpecificAdapter=10DE&2684&40BF1458;"), std::wstring());
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"SpecificAdapter=10de&2684&40bf1458;"), k);
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"SpecificAdapter=10DE&2684;"), std::wstring());
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"SpecificAdapter=ZZZZ&2684&40BF1458;"), std::wstring());
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"SpecificAdapter=10DE&&2684&40BF145;"), std::wstring());
        CHECK_EQ(cd::AdapterKeyFromPreferenceValue(L"SpecificAdapter=a&b&c;"), std::wstring());
    }

    Case("AJ15 Windows' own GPU settings are seen and named, not shown as 'not assigned'");
    {
        CHECK_EQ(cd::PreferenceChoiceKey(L"GpuPreference=2;"), cd::WindowsHighPerformanceKey());
        CHECK_EQ(cd::PreferenceChoiceKey(L"SwapEffectUpgradeEnable=1;GpuPreference=1;"), cd::WindowsPowerSavingKey());
        CHECK_EQ(cd::PreferenceChoiceKey(L"SwapEffectUpgradeEnable=0;GpuPreference=0;"), std::wstring());
        CHECK_EQ(cd::PreferenceChoiceKey(L"AppStatus=1;AutoHDREnable=2097;"), std::wstring());
        CHECK_EQ(cd::PreferenceChoiceKey(cd::FormatPreferenceValue(L"10DE&2684&40BF1458")),
                 std::wstring(L"10DE&2684&40BF1458"));
        CHECK(cd::IsWindowsModeKey(cd::WindowsHighPerformanceKey()));
        CHECK(cd::IsWindowsModeKey(cd::WindowsPowerSavingKey()));
        CHECK(!cd::IsWindowsModeKey(L"10DE&2684&40BF1458"));
        CHECK(!cd::IsWindowsModeKey(L""));
        // a row on a Windows setting is not on the target, so the selector still offers to move it...
        std::vector<cd::GpuRow> rows(1);
        rows[0].exeName = L"x.exe";
        rows[0].exePath = L"C:\\P\\app-1\\x.exe";
        rows[0].assignedKey = cd::WindowsHighPerformanceKey();
        const std::vector<cd::GpuRow> picked = cd::SelectForBackground(rows, L"10DE&2684&40BF1458");
        CHECK(picked.size() == 1 && picked[0].selected);
        // ...and Remove takes the setting away while keeping Windows' other fields
        CHECK_EQ(cd::StripGpuAssignment(L"SwapEffectUpgradeEnable=1;GpuPreference=2;"),
                 std::wstring(L"SwapEffectUpgradeEnable=1;"));
    }

    Case("AJ16 only plain name=value; fields are edited; anything else is left exactly as it is");
    {
        CHECK(cd::IsEditablePreferenceValue(L""));
        CHECK(cd::IsEditablePreferenceValue(L"AppStatus=1;AutoHDREnable=2097;"));
        CHECK(cd::IsEditablePreferenceValue(
            L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;SwapEffectUpgradeEnable=1;AutoHDREnable=4147;"));
        CHECK(cd::IsEditablePreferenceValue(L"Odd=a=b;"));
        CHECK(!cd::IsEditablePreferenceValue(L"AppStatus=1"));
        CHECK(!cd::IsEditablePreferenceValue(L"FutureFlag;GpuPreference=2;"));
        CHECK(!cd::IsEditablePreferenceValue(L";;=5;junk;GpuPreference=1;"));
        CHECK(!cd::IsEditablePreferenceValue(L"=5;"));
        CHECK(!cd::IsEditablePreferenceValue(L";"));
        // for an editable value, merge and strip keep every other field byte for byte and in order
        const std::wstring v = L"AppStatus=1;Odd=a=b;GpuPreference=2;AutoHDREnable=2097;";
        CHECK(cd::IsEditablePreferenceValue(v));
        CHECK_EQ(cd::StripGpuAssignment(v), std::wstring(L"AppStatus=1;Odd=a=b;AutoHDREnable=2097;"));
        CHECK_EQ(cd::MergeGpuPreferenceValue(v, L"10DE&2684&40BF1458"),
                 std::wstring(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=1073741824;AppStatus=1;Odd=a=b;AutoHDREnable=2097;"));
    }

    Case("AJ17 the restore file puts every value back, and deletes the ones that were absent");
    {
        std::vector<cd::GpuPreferenceBefore> before(2);
        before[0].exePath = L"C:\\P\\app-1\\x.exe";
        before[0].present = true;
        before[0].value = L"AppStatus=1;Q=\"q\";";
        before[1].exePath = L"C:\\P\\y.exe";
        before[1].present = false;
        const std::wstring expected =
            cd::FormatRegRestoreHeader() +
            L"\"C:\\\\P\\\\app-1\\\\x.exe\"=\"AppStatus=1;Q=\\\"q\\\";\"\r\n"
            L"\"C:\\\\P\\\\y.exe\"=-\r\n";
        CHECK_EQ(cd::FormatRegRestoreFile(before), expected);
        CHECK_EQ(cd::FormatRegRestoreFile(std::vector<cd::GpuPreferenceBefore>()), cd::FormatRegRestoreHeader());
        // regedit's own two lines, unchanged and in their places: the first line of the file, and the key
        // line immediately above the first row.
        CHECK_EQ(cd::RegRestoreVersionLine(), std::wstring(L"Windows Registry Editor Version 5.00\r\n"));
        CHECK_EQ(cd::RegRestoreKeyLine(),
                 std::wstring(L"[HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences]\r\n"));
        CHECK_EQ(expected.compare(0, cd::RegRestoreVersionLine().size(), cd::RegRestoreVersionLine()), 0);
        CHECK(expected.find(cd::RegRestoreKeyLine() + L"\"C:") != std::wstring::npos);
    }

    Case("AJ17b the restore file says CUDA is NOT in it, in lines regedit treats as comments (F12)");
    {
        // 🔴 THE .REG IS THE WHOLE UNDO FOR WINDOWS' GPU PREFERENCE AND NONE OF THE UNDO FOR CUDA. NVIDIA
        // keeps which GPU CUDA uses in its own settings, so no registry file can put it back - and a user
        // who opens this one and stops there is left with the CUDA exclusion still on.
        const std::wstring head = cd::FormatRegRestoreHeader();
        CHECK(head.find(L"WHICH GPU CUDA USES IS NOT IN THIS FILE.") != std::wstring::npos);
        CHECK(head.find(L"NVIDIA keeps that in its own settings, not in the") != std::wstring::npos);
        CHECK(head.find(L"Use \"Remove assignment\" on Game Optimizer's") != std::wstring::npos);
        CHECK(head.find(L"GPU Assignment tab for that") != std::wstring::npos);
        // It names the ONE record this version keeps (D1), not the per-run files rounds 1 and 2 wrote.
        CHECK(head.find(L"gpu-cuda-record.txt file kept in this same folder") != std::wstring::npos);
        CHECK(head.find(cd::CudaRecordFileName()) != std::wstring::npos);
        CHECK(head.find(L"gpu-cuda-before") == std::wstring::npos);

        // 🔴 AND IT IS STILL A .REG FILE REGEDIT WILL IMPORT. The version line is first, the key line is
        // last, and EVERY line between them is blank or starts with ';' - regedit's comment marker - so
        // the note changes nothing the file writes.
        CHECK_EQ(head.compare(0, cd::RegRestoreVersionLine().size(), cd::RegRestoreVersionLine()), 0);
        const size_t key = head.find(cd::RegRestoreKeyLine());
        CHECK(key != std::wstring::npos);
        CHECK_EQ(key + cd::RegRestoreKeyLine().size(), head.size());
        size_t at = cd::RegRestoreVersionLine().size();
        size_t comments = 0;
        bool everyLineIsAComment = true;
        while (at < key) {
            const size_t nl = head.find(L"\r\n", at);
            if (nl == std::wstring::npos) { everyLineIsAComment = false; break; }
            const std::wstring line = head.substr(at, nl - at);
            if (!line.empty()) {
                if (line[0] != L';') everyLineIsAComment = false;
                else ++comments;
            }
            at = nl + 2;
        }
        CHECK(everyLineIsAComment);
        CHECK(comments >= 5);

        // A complete file is still recognised as complete, and a .reg an EARLIER version wrote - the same
        // two lines with no comment block - still is too. Refusing that one would make the window tell a
        // user not to open the only way back they have (FormatUnfinishedNotice).
        std::vector<cd::GpuPreferenceBefore> one(1);
        one[0].exePath = L"C:\\x.exe";
        one[0].present = false;
        CHECK(cd::IsCompleteRegRestoreText(cd::FormatRegRestoreFile(one)));
        CHECK(cd::IsCompleteRegRestoreText(cd::RegRestoreVersionLine() + L"\r\n" + cd::RegRestoreKeyLine() +
                                           L"\"C:\\\\x.exe\"=-\r\n"));
        // and nothing became complete that was not: no row, a comment with no line break, another key.
        CHECK(!cd::IsCompleteRegRestoreText(head));
        CHECK(!cd::IsCompleteRegRestoreText(cd::RegRestoreVersionLine() + L"; cut off here"));
        CHECK(!cd::IsCompleteRegRestoreText(cd::RegRestoreVersionLine() + L"\r\n[HKEY_CURRENT_USER\\Other]\r\n"
                                            L"\"C:\\\\x.exe\"=-\r\n"));
    }

    Case("AJ18 a card counts only beside its own mode, and control characters are never edited");
    {
        // Found by adversarial review, round 3.
        CHECK_EQ(cd::PreferenceChoiceKey(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=2;"),
                 cd::WindowsHighPerformanceKey());
        CHECK_EQ(cd::PreferenceChoiceKey(L"SpecificAdapter=10DE&2684&40BF1458;"), std::wstring());
        CHECK_EQ(cd::PreferenceChoiceKey(L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=0;"), std::wstring());
        CHECK_EQ(cd::PreferenceChoiceKey(L"GpuPreference=1073741824;SpecificAdapter=10DE&2684&40BF1458;"),
                 std::wstring(L"10DE&2684&40BF1458"));
        CHECK_EQ(cd::PreferenceChoiceKey(L"GpuPreference=1073741824;"), std::wstring());
        CHECK(!cd::IsEditablePreferenceValue(std::wstring(L"AppStatus=1;") + wchar_t(10) + L"X=1;"));
        CHECK(!cd::IsEditablePreferenceValue(std::wstring(L"A=") + wchar_t(9) + L";"));
        CHECK(!cd::IsEditablePreferenceValue(std::wstring(L"A=1") + wchar_t(0) + L";"));
        CHECK(!cd::IsEditablePreferenceValue(std::wstring(L"A=1") + wchar_t(0x7F) + L";"));
        // quotes and backslashes are escaped in the restore file, so they stay editable
        CHECK(cd::IsEditablePreferenceValue(L"Q=\"a\\b\";"));
    }

    Case("AJ19 a lost assignment is told to take the GPU chosen in the window, never promised its old one");
    {
        // Found by adversarial review, round 3: "assign them again" read as a promise of the old GPU,
        // while Apply writes whatever the picker shows.
        const std::wstring one = cd::FormatGpuIsolateStatusLine(7, 2, 1);
        const std::wstring many = cd::FormatGpuIsolateStatusLine(7, 2, 2);
        CHECK(one.find(L"tick it and Apply to put it on the GPU chosen above") != std::wstring::npos);
        CHECK(many.find(L"tick them and Apply to put them on the GPU chosen above") != std::wstring::npos);
        CHECK(one.find(L"again") == std::wstring::npos);
        CHECK(many.find(L"again") == std::wstring::npos);
    }

    Case("AJ20 the classifier reads a stored value exactly as the window does");
    {
        // Found by adversarial review, round 4: the classifier still took a card without its mode.
        const std::wstring k = L"10DE&2684&40BF1458";
        CHECK(cd::ClassifyGpuPref(k, L"SpecificAdapter=10DE&2684&40BF1458;GpuPreference=2;", k) ==
              cd::GpuPrefState::WrongAdapter);
        CHECK(cd::ClassifyGpuPref(k, L"SpecificAdapter=10DE&2684&40BF1458;", k) == cd::GpuPrefState::WrongAdapter);
        CHECK(cd::ClassifyGpuPref(k, cd::FormatPreferenceValue(k), k) == cd::GpuPrefState::Correct);
        CHECK(!cd::IsWindowsModeKey(cd::UnreadableChoiceKey()));
        CHECK(cd::UnreadableChoiceKey() != cd::PreferenceChoiceKey(cd::FormatPreferenceValue(k)));
    }

    Case("AJ21 Apply: a row is recorded right after its change, and only a changed row is ever recorded");
    {
        // Found by adversarial review, round 5: the restore file listed every planned row until the last write,
        // a refused row kept a stale label, and a no-op was counted as a change.
        const std::wstring gpu = L"10DE&2684&40BF1458";
        const std::wstring own = L"AppStatus=1;AutoHDREnable=2097;";
        const std::wstring a = L"C:\\A\\a.exe", b = L"C:\\B\\b.exe", c = L"C:\\C\\c.exe", dd = L"C:\\D\\d.exe",
                           e = L"C:\\E\\e.exe";
        FakeGpuEdits f;
        f.reg[a] = own;                                     // another program changes it: refused
        f.reg[b] = own;                                     // an ordinary change
        f.reg[c] = cd::MergeGpuPreferenceValue(own, gpu);   // already exactly so: nothing to do
        f.unreadableAfterWrite.insert(dd);                  // absent; changed, but the read-back fails
        f.reg[e] = own;                                     // Windows cancels the commit
        f.result[a] = cd::GuardedWriteResult::Changed;
        f.otherProgram[a] = L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;";
        f.result[e] = cd::GuardedWriteResult::NotCommitted;
        f.error[e] = 6704;
        std::vector<std::wstring> paths;
        paths.push_back(a); paths.push_back(b); paths.push_back(c); paths.push_back(dd); paths.push_back(e);
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(f.Items(paths), false, gpu, f.Ops());
        CHECK(r.size() == 5);

        CHECK(r[0].outcome == cd::GpuEditOutcome::Refused);
        CHECK(r[0].reason.find(L"changed after it was read") != std::wstring::npos);
        CHECK_EQ(r[0].choiceKey, std::wstring(L"1002&13C0&88771043"));   // what the other program wrote, not the old label
        CHECK(!r[0].recorded);
        CHECK_EQ(f.reg[a], std::wstring(L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;"));

        CHECK(r[1].outcome == cd::GpuEditOutcome::Done);
        CHECK_EQ(r[1].choiceKey, gpu);
        CHECK(r[1].recorded);
        CHECK_EQ(f.reg[b], cd::MergeGpuPreferenceValue(own, gpu));

        CHECK(r[2].outcome == cd::GpuEditOutcome::AlreadyDone);
        CHECK(f.At(L"write " + c) == f.log.size());   // never written
        CHECK(!r[2].recorded);
        CHECK_EQ(r[2].choiceKey, gpu);

        CHECK(r[3].outcome == cd::GpuEditOutcome::Unconfirmed);
        CHECK(r[3].reason.find(L"could not be read back") != std::wstring::npos);
        CHECK_EQ(r[3].choiceKey, cd::UnreadableChoiceKey());
        CHECK(r[3].recorded);   // it was committed, so it can be put back

        CHECK(r[4].outcome == cd::GpuEditOutcome::Refused);
        CHECK(r[4].reason.find(L"6704") != std::wstring::npos);
        CHECK(r[4].reason.find(L"most likely") != std::wstring::npos);
        CHECK_EQ(r[4].choiceKey, std::wstring());
        CHECK(!r[4].recorded);

        // exactly the two committed rows are in the restore file, with the values pass one read
        CHECK(f.recorded.size() == 2);
        CHECK(f.recorded.size() == 2 && f.recorded[0].exePath == b && f.recorded[0].present && f.recorded[0].value == own);
        CHECK(f.recorded.size() == 2 && f.recorded[1].exePath == dd && !f.recorded[1].present);
        // and each is recorded BEFORE the next row is written
        CHECK(f.At(L"record " + b) < f.At(L"write " + dd));
        CHECK(f.At(L"record " + dd) < f.At(L"write " + e));
        CHECK(f.At(L"write " + b) < f.At(L"record " + b));
    }

    Case("AJ22 a change the restore file cannot record stops every change after it");
    {
        const std::wstring gpu = L"10DE&2684&40BF1458";
        const std::wstring own = L"AppStatus=1;";
        const std::wstring x = L"C:\\X\\x.exe", y = L"C:\\Y\\y.exe", z = L"C:\\Z\\z.exe";
        FakeGpuEdits f;
        f.reg[y] = own;
        f.reg[z] = own;
        f.recordFails.insert(x);
        std::vector<std::wstring> paths;
        paths.push_back(x); paths.push_back(y); paths.push_back(z);
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(f.Items(paths), false, gpu, f.Ops());
        CHECK(r.size() == 3);
        CHECK(r[0].outcome == cd::GpuEditOutcome::Done && !r[0].recorded);   // it changed; the caller must say it is missing
        CHECK(r[1].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK(r[2].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK(r[1].reason.find(L"not tried") != std::wstring::npos);
        CHECK(f.At(L"write " + y) == f.log.size());
        CHECK(f.At(L"write " + z) == f.log.size());
        CHECK_EQ(f.reg[y], own);
        CHECK_EQ(f.reg[z], own);
    }

    Case("AJ23 Remove: nothing to remove is not a removal, and a value created meanwhile is left alone");
    {
        const std::wstring gpu = L"10DE&2684&40BF1458";
        const std::wstring own = L"AppStatus=1;AutoHDREnable=2097;";
        const std::wstring r1 = L"C:\\R1\\r1.exe", r2 = L"C:\\R2\\r2.exe", r3 = L"C:\\R3\\r3.exe", r4 = L"C:\\R4\\r4.exe",
                           r5 = L"C:\\R5\\r5.exe";
        FakeGpuEdits f;
        f.reg[r3] = own;                                     // no GPU fields at all
        f.reg[r4] = cd::FormatPreferenceValue(gpu);          // only this feature's fields: deleted
        f.reg[r5] = cd::MergeGpuPreferenceValue(own, gpu);   // stripped back to Windows' own fields
        std::vector<std::wstring> paths;
        paths.push_back(r1); paths.push_back(r2); paths.push_back(r3); paths.push_back(r4); paths.push_back(r5);
        std::vector<cd::GpuEditItem> items = f.Items(paths);
        f.reg[r2] = L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;";   // created after pass one read r2 as absent
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, true, std::wstring(), f.Ops());
        CHECK(r.size() == 5);
        CHECK(r[0].outcome == cd::GpuEditOutcome::AlreadyDone);
        CHECK(r[0].reason.find(L"no GPU assignment to remove") != std::wstring::npos);
        CHECK(r[1].outcome == cd::GpuEditOutcome::Refused);
        CHECK_EQ(r[1].choiceKey, std::wstring(L"1002&13C0&88771043"));
        CHECK(r[2].outcome == cd::GpuEditOutcome::AlreadyDone);
        CHECK(f.At(L"write " + r1) == f.log.size());
        CHECK(f.At(L"write " + r2) == f.log.size());
        CHECK(f.At(L"write " + r3) == f.log.size());
        CHECK(r[3].outcome == cd::GpuEditOutcome::Done);
        CHECK(f.At(L"write " + r4 + L" delete") < f.log.size());
        CHECK(f.reg.find(r4) == f.reg.end());
        CHECK(r[4].outcome == cd::GpuEditOutcome::Done);
        CHECK_EQ(f.reg[r5], own);
        CHECK_EQ(r[4].choiceKey, std::wstring());
        CHECK(f.recorded.size() == 2);
        CHECK(f.recorded.size() == 2 && f.recorded[0].exePath == r4 && f.recorded[1].exePath == r5);
        CHECK_EQ(f.reg[r2], std::wstring(L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;"));
    }

    Case("AJ24 a guarded write that did not happen is described by what was observed, not by a guess");
    {
        using G = cd::GuardedWriteResult;
        CHECK(cd::GuardedReason(G::Changed, 0) != cd::GuardedReason(G::CheckUnreadable, 0));
        CHECK(cd::GuardedReason(G::CheckUnreadable, 0).find(L"could not be read again") != std::wstring::npos);
        CHECK(cd::GuardedReason(G::NotCommitted, 6704).find(L"(error 6704)") != std::wstring::npos);
        CHECK(cd::GuardedReason(G::NotCommitted, 5).find(L"(error 5)") != std::wstring::npos);
        CHECK(cd::GuardedReason(G::NotCommitted, 5).find(L"another program") == std::wstring::npos);
        CHECK(cd::GuardedReason(G::Unavailable, 0).find(L"(error") == std::wstring::npos);
        CHECK(cd::GuardedReason(G::Failed, 87).find(L"(error 87)") != std::wstring::npos);
        CHECK(cd::GuardedReason(G::Written, 0).empty());
        // v0.5.7: the caller's own check said no - its own words, never "Windows refused" (the default case)
        CHECK_EQ(cd::GuardedReason(G::NoLongerAllowed, 0),
                 std::wstring(L"another version of it may now be pinned to the main GPU, or Windows' GPU settings could "
                              L"not all be read again, so it was left alone; tick it by hand to change it anyway"));
        CHECK(cd::GuardedReason(G::NoLongerAllowed, 0) != cd::GuardedReason(G::Failed, 0));
    }

    Case("AJ47 a row Auto assign ticked reaches the guarded write marked as such; a hand tick never does");
    {
        // v0.5.7, the sibling-pin race: the window's write op asks Auto assign's rule again inside the guarded write only
        // for an item marked automatic, so RunGpuEdits must hand each item's own flag through, unchanged, in order.
        FakeGpuEdits f;
        const std::wstring handPath = L"C:\\Apps\\Hand\\hand.exe", autoPath = L"C:\\Apps\\Auto\\auto.exe";
        std::vector<std::wstring> paths;
        paths.push_back(handPath);
        paths.push_back(autoPath);
        std::vector<cd::GpuEditItem> items = f.Items(paths);
        items[1].automatic = true;
        CHECK(!items[0].automatic);   // the default is a hand tick
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, false, L"10DE&2684&40BF1458", f.Ops());
        CHECK(r.size() == 2 && r[0].outcome == cd::GpuEditOutcome::Done && r[1].outcome == cd::GpuEditOutcome::Done);
        CHECK(f.automaticWrites.size() == 1 && f.automaticWrites.count(autoPath) == 1);
        // a refusal from the check is reported in its own words, and nothing is recorded or counted as done
        FakeGpuEdits g;
        g.result[autoPath] = cd::GuardedWriteResult::NoLongerAllowed;
        std::vector<cd::GpuEditItem> one = g.Items(std::vector<std::wstring>(1, autoPath));
        one[0].automatic = true;
        const std::vector<cd::GpuEditResult> refused = cd::RunGpuEdits(one, false, L"10DE&2684&40BF1458", g.Ops());
        CHECK(refused.size() == 1 && refused[0].outcome == cd::GpuEditOutcome::Refused);
        CHECK(refused.size() == 1 &&
              refused[0].reason == cd::GuardedReason(cd::GuardedWriteResult::NoLongerAllowed, 0));
        CHECK(g.recorded.empty() && g.reg.find(autoPath) == g.reg.end());
    }

    Case("AJ25 the record kept while a change runs cannot be imported; the .reg is its heading plus its rows");
    {
        std::vector<cd::GpuPreferenceBefore> rows(2);
        rows[0].exePath = L"C:\\A\\a.exe";
        rows[0].present = true;
        rows[0].value = L"Q=\"x\\y\";";
        rows[1].exePath = L"C:\\B\\b.exe";
        rows[1].present = false;
        const std::wstring reg = cd::FormatRegRestoreFile(rows);
        CHECK_EQ(reg, cd::FormatRegRestoreHeader() + cd::FormatRegRestoreRow(rows[0]) + cd::FormatRegRestoreRow(rows[1]));
        CHECK(reg.compare(0, 36, L"Windows Registry Editor Version 5.00") == 0);
        CHECK_EQ(cd::FormatRegRestoreRow(rows[0]), std::wstring(L"\"C:\\\\A\\\\a.exe\"=\"Q=\\\"x\\\\y\\\";\"\r\n"));
        CHECK_EQ(cd::FormatRegRestoreRow(rows[1]), std::wstring(L"\"C:\\\\B\\\\b.exe\"=-\r\n"));
        const std::wstring pending = cd::FormatPendingRestoreFile(rows);
        CHECK(pending.compare(0, 23, L"Windows Registry Editor") != 0);
        CHECK(pending.compare(0, 8, L"REGEDIT4") != 0);
        CHECK(pending.find(L"Windows Registry Editor Version 5.00") == std::wstring::npos);
        CHECK(pending.find(L"NOT A RESTORE FILE") != std::wstring::npos);
        CHECK(pending.find(cd::FormatRegRestoreRow(rows[0])) != std::wstring::npos);
        CHECK(pending.find(cd::FormatRegRestoreRow(rows[1])) != std::wstring::npos);
        // round 6: every value line is a comment, so even a copy with regedit's heading pasted on imports nothing
        CHECK(pending.find(L"; " + cd::FormatRegRestoreRow(rows[0])) != std::wstring::npos);
        CHECK(pending.find(L"; " + cd::FormatRegRestoreRow(rows[1])) != std::wstring::npos);
        CHECK(pending.find(L"\r\n\"") == std::wstring::npos);
        CHECK(pending.compare(0, 1, L"\"") != 0);
    }

    Case("AJ26 a list item that cannot be tied to its row, and cannot be taken back, breaks the control");
    {
        std::vector<long long> added;
        std::vector<long long> removed;
        const auto add = [&added](size_t i) { added.push_back(static_cast<long long>(i)); return static_cast<long long>(added.size() - 1); };
        const cd::PopulateResult ok = cd::PopulateControl(3, add, [](long long, size_t) { return true; },
                                                          [](long long) { return true; });
        CHECK(!ok.broken && ok.shown.size() == 3 && ok.shown[0] && ok.shown[1] && ok.shown[2]);

        const cd::PopulateResult noInsert = cd::PopulateControl(
            3, [](size_t i) { return i == 1 ? -1LL : static_cast<long long>(i); },
            [](long long, size_t) { return true; }, [](long long) { return true; });
        CHECK(!noInsert.broken && noInsert.shown[0] && !noInsert.shown[1] && noInsert.shown[2]);

        const cd::PopulateResult untied = cd::PopulateControl(
            3, [](size_t i) { return static_cast<long long>(i); },
            [](long long, size_t i) { return i != 2; },
            [&removed](long long at) { removed.push_back(at); return true; });
        CHECK(!untied.broken && untied.shown[0] && untied.shown[1] && !untied.shown[2]);
        CHECK(removed.size() == 1 && removed[0] == 2);

        const cd::PopulateResult stuck = cd::PopulateControl(
            3, [](size_t i) { return static_cast<long long>(i); },
            [](long long, size_t i) { return i != 0; }, [](long long) { return false; });
        CHECK(stuck.broken && !stuck.shown[0] && !stuck.shown[1] && !stuck.shown[2]);

        size_t index = 99;
        CHECK(!cd::VisibleCandidate(-1, 0, 3, index));
        CHECK(!cd::VisibleCandidate(0, -1, 3, index));
        CHECK(!cd::VisibleCandidate(0, 3, 3, index));
        CHECK(cd::VisibleCandidate(1, 2, 3, index) && index == 2);
    }

    Case("AJ27 two rows with one file name never read the same, however shallow or alike their folders");
    {
        std::vector<std::pair<std::wstring, std::wstring> > rows;
        rows.push_back(std::make_pair(std::wstring(L"x.exe"), std::wstring(L"C:\\Alpha\\x.exe")));
        rows.push_back(std::make_pair(std::wstring(L"X.EXE"), std::wstring(L"D:\\Beta\\x.exe")));
        rows.push_back(std::make_pair(std::wstring(L"claude.exe"),
                                      std::wstring(L"C:\\Users\\u\\AppData\\Local\\AnthropicClaude\\app-1.2\\claude.exe")));
        rows.push_back(std::make_pair(std::wstring(L"claude.exe"),
                                      std::wstring(L"C:\\Users\\u\\AppData\\Roaming\\Claude\\claude-code\\2.1\\claude.exe")));
        rows.push_back(std::make_pair(std::wstring(L"y.exe"), std::wstring(L"C:\\One\\tools\\bin\\y.exe")));
        rows.push_back(std::make_pair(std::wstring(L"y.exe"), std::wstring(L"D:\\Two\\tools\\bin\\y.exe")));
        rows.push_back(std::make_pair(std::wstring(L"solo.exe"), std::wstring(L"C:\\Solo\\solo.exe")));
        const std::vector<std::wstring> w = cd::DisambiguatingFolders(rows);
        CHECK(w.size() == 7);
        CHECK_EQ(w[0], std::wstring(L"C:\\Alpha"));
        CHECK_EQ(w[1], std::wstring(L"D:\\Beta"));
        CHECK_EQ(w[2], std::wstring(L"AnthropicClaude\\app-1.2"));
        CHECK_EQ(w[3], std::wstring(L"claude-code\\2.1"));
        CHECK_EQ(w[4], std::wstring(L"C:\\One\\tools\\bin"));
        CHECK_EQ(w[5], std::wstring(L"D:\\Two\\tools\\bin"));
        CHECK_EQ(w[6], std::wstring());
    }

    Case("AJ28 a preference that could not be read is never reported as a lost assignment");
    {
        std::vector<std::wstring> running(1, L"C:\\Apps\\Tool\\app-1.1\\tool.exe");
        const auto missing = [](const std::wstring&) { return false; };
        std::vector<std::pair<std::wstring, std::wstring> > reg(
            1, std::make_pair(std::wstring(L"C:\\Apps\\Tool\\app-1.0\\tool.exe"), cd::UnreadableChoiceKey()));
        CHECK(cd::FindOrphanedAssignments(reg, running, missing).empty());
        // control: the same shape with a real card IS reported, so the check above can fail
        reg[0].second = L"10DE&2684&40BF1458";
        CHECK(cd::FindOrphanedAssignments(reg, running, missing).size() == 1);
    }

    Case("AJ29 the notice for a change that did not finish names only files that exist");
    {
        // Found by adversarial review, round 6: it always spoke of "the .reg restore file it saved".
        CHECK(cd::FormatUnfinishedNotice(std::vector<cd::UnfinishedRecord>()).empty());

        std::vector<cd::UnfinishedRecord> noReg(1);
        noReg[0].pendingPath = L"C:\\D\\gpu-preferences-before-20260912-154408-546.pending";
        const std::wstring a = cd::FormatUnfinishedNotice(noReg);
        CHECK(a.find(L"An earlier GPU change left its record behind") == 0);
        CHECK(a.find(L"It wrote no restore file") != std::wstring::npos);
        CHECK(a.find(noReg[0].pendingPath) != std::wstring::npos);
        CHECK(a.find(L".reg") == std::wstring::npos);

        std::vector<cd::UnfinishedRecord> withReg(1);
        withReg[0].pendingPath = L"C:\\D\\gpu-preferences-before-20260912-154642-024.pending";
        withReg[0].regExists = true;
        withReg[0].regValid = true;
        const std::wstring b = cd::FormatUnfinishedNotice(withReg);
        CHECK(b.find(L"C:\\D\\gpu-preferences-before-20260912-154642-024.reg") != std::wstring::npos);
        CHECK(b.find(withReg[0].pendingPath) != std::wstring::npos);
        CHECK(b.find(L"may be missing from that file") != std::wstring::npos);
        CHECK(b.find(L"It wrote no restore file") == std::wstring::npos);

        std::vector<cd::UnfinishedRecord> five(5);
        for (size_t i = 0; i < five.size(); ++i)
            five[i].pendingPath = L"C:\\D\\gpu-preferences-before-2026091" + std::to_wstring(i) + L".pending";
        const std::wstring c = cd::FormatUnfinishedNotice(five);
        CHECK(c.find(L"5 earlier GPU changes left their records behind") == 0);
        CHECK(c.find(L"and 2 more in the same folder") != std::wstring::npos);
        CHECK(c.find(five[4].pendingPath) != std::wstring::npos);   // the newest is named
        CHECK(c.find(five[0].pendingPath) == std::wstring::npos);   // the oldest is counted, not named
    }

    Case("AJ30 a restore file that does not read as complete is never recommended");
    {
        // Found by adversarial review, round 6: existence was the only check.
        std::vector<cd::UnfinishedRecord> torn(1);
        torn[0].pendingPath = L"C:\\D\\gpu-preferences-before-20260912-160915-401.pending";
        torn[0].regExists = true;
        const std::wstring n = cd::FormatUnfinishedNotice(torn);
        CHECK(n.find(L"could not be checked as complete, so do not open it") != std::wstring::npos);
        CHECK(n.find(L"C:\\D\\gpu-preferences-before-20260912-160915-401.reg") != std::wstring::npos);
        CHECK(n.find(L"open it to put those values back") == std::wstring::npos);
        CHECK(n.find(L"do not rename it") != std::wstring::npos);

        std::vector<cd::GpuPreferenceBefore> rows(2);
        rows[0].exePath = L"C:\\A\\a.exe";
        rows[0].present = true;
        rows[0].value = L"Q=\"x\\y\";";
        rows[1].exePath = L"C:\\B\\b.exe";
        rows[1].present = false;
        const std::wstring whole = cd::FormatRegRestoreFile(rows);
        CHECK(cd::IsCompleteRegRestoreText(whole));
        CHECK(!cd::IsCompleteRegRestoreText(cd::FormatRegRestoreHeader()));             // no row at all
        CHECK(!cd::IsCompleteRegRestoreText(whole.substr(0, whole.size() - 2)));        // no final line break
        CHECK(!cd::IsCompleteRegRestoreText(whole.substr(0, whole.size() - 9)));        // cut inside the last name
        const std::wstring first = cd::FormatRegRestoreHeader() + cd::FormatRegRestoreRow(rows[0]);
        CHECK(cd::IsCompleteRegRestoreText(first));                                     // quotes and backslashes escaped
        CHECK(!cd::IsCompleteRegRestoreText(first.substr(0, first.size() - 5)));        // cut after an escape
        CHECK(!cd::IsCompleteRegRestoreText(whole + L"junk\r\n"));
        CHECK(!cd::IsCompleteRegRestoreText(cd::FormatPendingRestoreFile(rows)));
    }

    Case("AJ31 Apply through every failure the guarded write, the read-back and the record can report");
    {
        const std::wstring gpu = L"10DE&2684&40BF1458";
        const std::wstring amd = L"SpecificAdapter=1002&13C0&88771043;GpuPreference=1073741824;";
        const std::wstring own = L"AppStatus=1;";
        const std::wstring u = L"C:\\U\\u.exe", fl = L"C:\\F\\f.exe", dv = L"C:\\V\\v.exe", g1 = L"C:\\G1\\g1.exe",
                           g2 = L"C:\\G2\\g2.exe", g3 = L"C:\\G3\\g3.exe";
        FakeGpuEdits f;
        f.reg[u] = own; f.reg[fl] = own; f.reg[dv] = own; f.reg[g1] = own; f.reg[g2] = own; f.reg[g3] = own;
        f.result[u] = cd::GuardedWriteResult::Unavailable;
        f.error[u] = 1234;
        f.result[fl] = cd::GuardedWriteResult::Failed;
        f.error[fl] = 87;
        f.afterWrite[dv] = amd + own;          // another program's value is what reads back
        f.recordFails.insert(g2);
        std::vector<std::wstring> paths;
        paths.push_back(u); paths.push_back(fl); paths.push_back(dv); paths.push_back(g1); paths.push_back(g2); paths.push_back(g3);
        const std::vector<cd::GpuEditItem> items = f.Items(paths);
        f.reg[g3] = amd;                        // changed after pass one read it
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, false, gpu, f.Ops());
        CHECK(r.size() == 6);
        CHECK(r[0].outcome == cd::GpuEditOutcome::Refused && !r[0].recorded);
        CHECK(r[0].reason.find(L"could not start a protected change (error 1234)") != std::wstring::npos);
        CHECK(r[1].outcome == cd::GpuEditOutcome::Refused && !r[1].recorded);
        CHECK(r[1].reason.find(L"(error 87)") != std::wstring::npos);
        CHECK(f.At(L"record " + u) == f.log.size() && f.At(L"record " + fl) == f.log.size());
        CHECK(r[2].outcome == cd::GpuEditOutcome::Unconfirmed && r[2].recorded);
        CHECK(r[2].reason.find(L"different value reads back") != std::wstring::npos);
        CHECK_EQ(r[2].choiceKey, std::wstring(L"1002&13C0&88771043"));
        CHECK(r[3].outcome == cd::GpuEditOutcome::Done && r[3].recorded);
        CHECK(r[4].outcome == cd::GpuEditOutcome::Done && !r[4].recorded);   // a LATER record fails
        CHECK(r[5].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK(f.At(L"write " + g3) == f.log.size());
        CHECK(f.At(L"read " + g3) < f.log.size());
        CHECK_EQ(r[5].choiceKey, std::wstring(L"1002&13C0&88771043"));      // what is there now, not the snapshot
        CHECK(f.recorded.size() == 2);
    }

    Case("AJ32 Remove through a failed record, an unreadable read-back and a cancelled commit");
    {
        const std::wstring gpu = L"10DE&2684&40BF1458";
        const std::wstring own = L"AppStatus=1;";
        const std::wstring a = L"C:\\RA\\a.exe", b = L"C:\\RB\\b.exe", c = L"C:\\RC\\c.exe", dd = L"C:\\RD\\d.exe";
        FakeGpuEdits f;
        f.reg[a] = cd::MergeGpuPreferenceValue(own, gpu);
        f.reg[b] = cd::MergeGpuPreferenceValue(own, gpu);
        f.recordFails.insert(a);
        std::vector<std::wstring> p1;
        p1.push_back(a); p1.push_back(b);
        const std::vector<cd::GpuEditResult> r1 = cd::RunGpuEdits(f.Items(p1), true, std::wstring(), f.Ops());
        CHECK(r1.size() == 2);
        CHECK(r1[0].outcome == cd::GpuEditOutcome::Done && !r1[0].recorded);
        CHECK(r1[1].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK(f.At(L"write " + b) == f.log.size());
        CHECK_EQ(f.reg[b], cd::MergeGpuPreferenceValue(own, gpu));
        CHECK_EQ(r1[1].choiceKey, gpu);

        FakeGpuEdits g;
        g.reg[c] = cd::FormatPreferenceValue(gpu);
        g.reg[dd] = cd::MergeGpuPreferenceValue(own, gpu);
        g.unreadableAfterWrite.insert(c);
        g.result[dd] = cd::GuardedWriteResult::NotCommitted;
        g.error[dd] = 6704;
        std::vector<std::wstring> p2;
        p2.push_back(c); p2.push_back(dd);
        const std::vector<cd::GpuEditResult> r2 = cd::RunGpuEdits(g.Items(p2), true, std::wstring(), g.Ops());
        CHECK(r2.size() == 2);
        CHECK(r2[0].outcome == cd::GpuEditOutcome::Unconfirmed && r2[0].recorded);
        CHECK_EQ(r2[0].choiceKey, cd::UnreadableChoiceKey());
        CHECK(r2[1].outcome == cd::GpuEditOutcome::Refused && !r2[1].recorded);
        CHECK(r2[1].reason.find(L"6704") != std::wstring::npos);
        CHECK_EQ(r2[1].choiceKey, gpu);
    }

    Case("AJ33 a result names a restore file only when one was written");
    {
        const std::wstring reg = L"C:\\D\\x.reg", pend = L"C:\\D\\x.pending";
        const std::wstring done = cd::FormatRestoreLines(true, reg, pend, false, std::wstring(), true);
        CHECK(done.find(reg) != std::wstring::npos && done.find(pend) == std::wstring::npos);
        const std::wstring noFile = cd::FormatRestoreLines(false, reg, pend, true, L"\r\n    a.exe", false);
        CHECK(noFile.find(L"No restore file could be written") != std::wstring::npos);
        CHECK(noFile.find(reg) == std::wstring::npos);
        CHECK(noFile.find(L"Missing from that file") == std::wstring::npos);
        CHECK(noFile.find(pend) != std::wstring::npos && noFile.find(L"named just above is in it") != std::wstring::npos);
        const std::wstring missing = cd::FormatRestoreLines(true, reg, pend, true, L"\r\n    b.exe", false);
        CHECK(missing.find(L"Missing from that file") != std::wstring::npos && missing.find(reg) != std::wstring::npos);
        const std::wstring leftover = cd::FormatRestoreLines(true, reg, pend, false, std::wstring(), false);
        CHECK(leftover.find(L"Delete it once you no longer need it") != std::wstring::npos);
        CHECK(cd::FormatRestoreLines(false, reg, pend, false, std::wstring(), true).empty());
    }

    Case("AJ34 the picker offers the main GPU first and once, and nothing on an undecidable plan");
    {
        // v0.5.6: removing an assignment does not put an application on the main GPU, so the picker must offer
        // it. DXGI lists the operator's RTX 5090 twice, and the second entry must never add a second row.
        const std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        const cd::GpuPlan p = cd::PlanGpuIsolation(v);
        std::vector<std::wstring> want;
        want.push_back(L"10DE&2B85&53021462");
        want.push_back(L"1002&13C0&88771043");
        want.push_back(L"10DE&2684&40BF1458");
        CHECK_EQ(cd::PickerTargetKeys(v, p), want);

        // the same three when the duplicate is enumerated before every other adapter
        std::vector<cd::GpuAdapter> dupFirst;
        dupFirst.push_back(v[0]); dupFirst.push_back(v[3]);
        dupFirst.push_back(v[1]); dupFirst.push_back(v[2]);
        CHECK_EQ(cd::PickerTargetKeys(dupFirst, cd::PlanGpuIsolation(dupFirst)), want);

        // the main GPU still FIRST when another card is enumerated before it - both fixtures above list it first, so
        // "every distinct key in enumeration order" passed them
        std::vector<cd::GpuAdapter> mainLater;
        mainLater.push_back(v[1]); mainLater.push_back(v[2]);
        mainLater.push_back(v[0]); mainLater.push_back(v[3]);
        CHECK_EQ(cd::PickerTargetKeys(mainLater, cd::PlanGpuIsolation(mainLater)), want);

        // twin cards plus a third GPU: the plan is decidable and the shared main-GPU key is offered first and once
        // (gpu_policy.h's second ceiling - which twin Windows picks for that key is unmeasured)
        std::vector<cd::GpuAdapter> twinsPlus(2, v[2]);
        twinsPlus[0].hasDisplay = true;
        twinsPlus.push_back(v[1]);
        std::vector<std::wstring> twinWant;
        twinWant.push_back(L"10DE&2684&40BF1458");
        twinWant.push_back(L"1002&13C0&88771043");
        CHECK_EQ(cd::PickerTargetKeys(twinsPlus, cd::PlanGpuIsolation(twinsPlus)), twinWant);

        // nothing for an undecidable plan, for the game card plus only its own duplicate, or for two identical cards
        CHECK(cd::PickerTargetKeys(v, cd::GpuPlan()).empty());
        std::vector<cd::GpuAdapter> onlyDup;
        onlyDup.push_back(v[0]); onlyDup.push_back(v[3]);
        CHECK(cd::PickerTargetKeys(onlyDup, cd::PlanGpuIsolation(onlyDup)).empty());
        std::vector<cd::GpuAdapter> twins(2, v[2]);   // two RTX 4090s, one key - the plan refuses (AG3g)
        twins[0].hasDisplay = true;
        CHECK(cd::PickerTargetKeys(twins, cd::PlanGpuIsolation(twins)).empty());

        CHECK(cd::IsMainGpuKey(p, L"10DE&2B85&53021462"));
        CHECK(!cd::IsMainGpuKey(p, L"1002&13C0&88771043"));
        CHECK(!cd::IsMainGpuKey(p, L"10DE&2684&40BF1458"));
        CHECK(!cd::IsMainGpuKey(p, L""));
        CHECK(!cd::IsMainGpuKey(cd::GpuPlan(), L""));   // an undecidable plan names no main GPU
    }

    Case("AJ35 Auto assign never moves apps onto the main GPU, and never sweeps a main-GPU pin back");
    {
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        std::vector<cd::GpuRow> rows(2);
        rows[0].exeName = L"browser.exe";  rows[0].exePath = L"C:\\B\\browser.exe";   // no assignment
        rows[1].exeName = L"editor.exe";   rows[1].exePath = L"C:\\E\\editor.exe";
        rows[1].assignedKey = mainKey;                                                // pinned to the main GPU by hand

        // the main GPU as the target: nothing is ticked, not even the unassigned row
        const std::vector<cd::GpuRow> ontoMain = cd::SelectForBackground(rows, mainKey, mainKey);
        CHECK(ontoMain.size() == 2 && !ontoMain[0].selected && !ontoMain[1].selected);

        // a background target: the unassigned row is ticked, the main-GPU pin is not
        const std::vector<cd::GpuRow> toBg = cd::SelectForBackground(rows, bgKey, mainKey);
        CHECK(toBg.size() == 2 && toBg[0].selected && !toBg[1].selected);

        // CONTROL: without the main GPU's key - every caller before v0.5.6 - the same pin IS ticked
        const std::vector<cd::GpuRow> before = cd::SelectForBackground(rows, bgKey);
        CHECK(before.size() == 2 && before[0].selected && before[1].selected);

        // decision 10 switched off: the pin is ticked again, and the main GPU as target is still refused
        const std::vector<cd::GpuRow> flipped = cd::SelectForBackground(rows, bgKey, mainKey, false);
        CHECK(flipped.size() == 2 && flipped[0].selected && flipped[1].selected);
        const std::vector<cd::GpuRow> flippedOntoMain = cd::SelectForBackground(rows, mainKey, mainKey, false);
        CHECK(flippedOntoMain.size() == 2 && !flippedOntoMain[0].selected && !flippedOntoMain[1].selected);

        // A MAIN-GPU PIN LOST TO AN AUTO-UPDATE: the new path has no value, so assignedKey is empty and only lostKey
        // remembers the main GPU. It is left alone like a live pin; a pin lost from ANOTHER GPU is still moved.
        std::vector<cd::GpuRow> orphans(2);
        orphans[0].exeName = L"claude.exe";  orphans[0].exePath = L"C:\\A\\app-1.2\\claude.exe";
        orphans[0].lostKey = mainKey;
        orphans[1].exeName = L"slack.exe";   orphans[1].exePath = L"C:\\S\\app-4.5\\slack.exe";
        orphans[1].lostKey = L"1002&13C0&88771043";
        const std::vector<cd::GpuRow> lost = cd::SelectForBackground(orphans, bgKey, mainKey);
        CHECK(lost.size() == 2 && !lost[0].selected && lost[1].selected);
        // CONTROLS: decision 10 switched off, and no main-GPU key, both tick the lost main-GPU pin again
        const std::vector<cd::GpuRow> lostFlipped = cd::SelectForBackground(orphans, bgKey, mainKey, false);
        CHECK(lostFlipped.size() == 2 && lostFlipped[0].selected && lostFlipped[1].selected);
        const std::vector<cd::GpuRow> lostBefore = cd::SelectForBackground(orphans, bgKey);
        CHECK(lostBefore.size() == 2 && lostBefore[0].selected && lostBefore[1].selected);
    }

    Case("AJ36 a GPU is integrated, discrete or unknown by every entry that shares its key");
    {
        const std::vector<cd::GpuAdapter> v = RealMachineAdapters();
        CHECK(cd::KindForKey(v, L"1002&13C0&88771043") == cd::GpuKind::Integrated);
        // DXCore does not know the second RTX 5090 entry; the card still reads discrete from the first
        CHECK(cd::KindForKey(v, L"10DE&2B85&53021462") == cd::GpuKind::Discrete);
        CHECK(cd::KindForKey(v, L"10DE&2684&40BF1458") == cd::GpuKind::Discrete);
        // a key whose only entry DXCore could not answer for, and a key no entry has
        const std::vector<cd::GpuAdapter> onlyUnknown(1, v[3]);
        CHECK(cd::KindForKey(onlyUnknown, L"10DE&2B85&53021462") == cd::GpuKind::Unknown);
        CHECK(cd::KindForKey(v, L"8086&A780&00000000") == cd::GpuKind::Unknown);
        // integrated outranks discrete when two entries with one key disagree - IN BOTH ORDERS, or "the last known kind
        // wins" would pass on the Discrete-then-Integrated order alone
        std::vector<cd::GpuAdapter> disagree(2, v[1]);
        disagree[0].kind = cd::GpuKind::Discrete;
        CHECK(cd::KindForKey(disagree, L"1002&13C0&88771043") == cd::GpuKind::Integrated);
        std::vector<cd::GpuAdapter> reversed(2, v[1]);
        reversed[1].kind = cd::GpuKind::Discrete;
        CHECK(cd::KindForKey(reversed, L"1002&13C0&88771043") == cd::GpuKind::Integrated);
        // CONTROL on the fixture: it reuses one struct, so a later entry must not inherit an earlier kind
        CHECK(v[2].kind == cd::GpuKind::Discrete && v[3].kind == cd::GpuKind::Unknown);

        // THE LOG COUNTS KEYS, NOT ENTRIES (Council review, v0.5.6): the second RTX 5090 entry is Unknown but its key is
        // not, so this machine writes no "Unknown" line
        size_t keys = 99;
        CHECK_EQ(cd::UnknownKindKeyCount(v, &keys), static_cast<size_t>(0));
        CHECK_EQ(keys, static_cast<size_t>(3));
        CHECK_EQ(cd::UnknownKindKeyCount(onlyUnknown, &keys), static_cast<size_t>(1));
        CHECK_EQ(keys, static_cast<size_t>(1));
        std::vector<cd::GpuAdapter> amdUnknown = v;
        amdUnknown[1].kind = cd::GpuKind::Unknown;   // an integrated GPU DXCore could not answer for
        CHECK_EQ(cd::UnknownKindKeyCount(amdUnknown), static_cast<size_t>(1));
        CHECK_EQ(cd::UnknownKindKeyCount(std::vector<cd::GpuAdapter>(), &keys), static_cast<size_t>(0));
        CHECK_EQ(keys, static_cast<size_t>(0));
    }

    Case("AJ37 Apply's question keeps v0.5.5's words and warns only about what is true");
    {
        // Copied from v0.5.5's DoApply, byte for byte: the first line and both replacing sentences.
        // 🔴 R6-4: THE EXPLAINING PARAGRAPH IS NOT BYTE-IDENTICAL ANY MORE, AND IT MUST NOT BE. It promised
        // to write Windows' own GPU preference "for each one", and a ticked row that already carries
        // exactly that preference is AlreadyDone - nothing is written for it. On every machine upgrading
        // from v0.5.6 or v0.5.7 that is EVERY ticked row, so the question promised a write for all of them
        // and the result then reported none.
        const std::wstring gap = L"\r\n\r\n";
        const std::wstring explain = L"This writes Windows' own per-application GPU preference for each one that does "
                                     L"not already have it, and keeps "
                                     L"the previous value of each one it changes in a restore file. It takes effect the "
                                     L"next time each application starts. Remove assignment later returns an application "
                                     L"to Windows' default GPU choice.";
        const std::wstring one = L"1 of them already has another GPU setting; it is replaced.";
        const std::wstring many = L"2 of them already have another GPU setting; those are replaced.";
        const std::wstring mainLine = L"NVIDIA GeForce RTX 5090 is the main GPU, where your games run: these "
                                      L"applications will share it with them.";
        const std::wstring igpuLine = L"NVIDIA GeForce RTX 5090 is an integrated GPU. Some applications can overload "
                                      L"it - for example a browser showing a 3D model - and run slowly.";

        cd::AssignConfirm c;
        c.count = 3;
        c.targetName = L"NVIDIA GeForce RTX 5090";
        // neither warning by default
        CHECK_EQ(cd::FormatAssignConfirm(c), L"Assign 3 applications to NVIDIA GeForce RTX 5090?" + gap + explain);
        c.count = 1;
        c.replacing = 1;
        CHECK_EQ(cd::FormatAssignConfirm(c),
                 L"Assign 1 application to NVIDIA GeForce RTX 5090?" + gap + explain + gap + one);
        c.count = 3;
        c.replacing = 2;
        CHECK_EQ(cd::FormatAssignConfirm(c),
                 L"Assign 3 applications to NVIDIA GeForce RTX 5090?" + gap + explain + gap + many);

        // each warning only when its flag is set
        c.replacing = 0;
        c.mainGpu = true;
        CHECK_EQ(cd::FormatAssignConfirm(c),
                 L"Assign 3 applications to NVIDIA GeForce RTX 5090?" + gap + explain + gap + mainLine);
        c.mainGpu = false;
        c.integrated = true;
        CHECK_EQ(cd::FormatAssignConfirm(c),
                 L"Assign 3 applications to NVIDIA GeForce RTX 5090?" + gap + explain + gap + igpuLine);

        // both together, with a replacing count: main, integrated, replacing - in that order
        c.mainGpu = true;
        c.replacing = 2;
        CHECK_EQ(cd::FormatAssignConfirm(c), L"Assign 3 applications to NVIDIA GeForce RTX 5090?" + gap + explain +
                                                 gap + mainLine + gap + igpuLine + gap + many);
    }

    Case("AJ37b Remove's question keeps v0.5.7's words and gains a CUDA line only when one is true");
    {
        // Copied from v0.5.7's DoRemove, byte for byte: the question and its one explaining paragraph.
        const std::wstring gap = L"\r\n\r\n";
        const std::wstring explain = L"Each returns to Windows' default GPU choice the next time it starts, and the "
                                     L"previous value of each one it changes is kept in a restore file. "
                                     L"Ticked applications with no assignment are left alone.";
        cd::RemoveConfirm c;
        c.count = 3;
        CHECK_EQ(cd::FormatRemoveConfirm(c), L"Remove the GPU assignment from 3 applications?" + gap + explain);
        c.count = 1;
        const std::wstring without = cd::FormatRemoveConfirm(c);
        CHECK_EQ(without, L"Remove the GPU assignment from 1 application?" + gap + explain);
        // the CUDA line is last, and a question without one is byte-identical to the one v0.5.7 asked
        c.cudaLine = L"This also asks NVIDIA to put back which GPU CUDA uses for 1 of them.";
        const std::wstring with = cd::FormatRemoveConfirm(c);
        CHECK_EQ(with.compare(0, without.size(), without), 0);
        CHECK_EQ(with, without + gap + c.cudaLine);
    }

    Case("AJ38 fixed v0.5.6 wording reads exactly as shipped");
    {
        // the status line that says why Auto assign is off while the main GPU is the target
        CHECK_EQ(cd::FormatMainGpuStatusLine(),
                 std::wstring(L"Auto assign GPU for Gaming is off while the main GPU is chosen - it only moves "
                              L"background apps off that GPU. Tick applications and Apply to put them on the main GPU."));

        // the Settings tabs, in page order. settings.cpp is not in this build, so settings_pages.h is the only
        // place a test can see them; the tab bar, its fallback buttons and the page headings all read that array.
        CHECK_EQ(sizeof(cd::kSettingsPageLabels) / sizeof(cd::kSettingsPageLabels[0]), static_cast<size_t>(4));
        CHECK_EQ(std::wstring(cd::kSettingsPageLabels[0]), std::wstring(L"Profiles"));
        CHECK_EQ(std::wstring(cd::kSettingsPageLabels[1]), std::wstring(L"CPU Core Map"));
        CHECK_EQ(std::wstring(cd::kSettingsPageLabels[2]), std::wstring(L"GPU Assignment"));
        CHECK_EQ(std::wstring(cd::kSettingsPageLabels[3]), std::wstring(L"Setting"));
    }

    Case("AJ39 a refresh of the tab keeps a tick only for the same program, GPU and row, and says a leftover record once");
    {
        // gpuwindow.cpp is not in this build, so these two rules live in gpu_rows.h; deleting either there used to pass.
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458", amdKey = L"1002&13C0&88771043";
        std::vector<std::wstring> offered;
        offered.push_back(mainKey); offered.push_back(amdKey); offered.push_back(bgKey);
        // what a row says, built the way LoadGpuData builds it: the listed choice is the GPU fields of the value it read
        const auto facts = [&mainKey](const std::wstring& path, const std::wstring& assigned, const std::wstring& lost,
                                      bool profile) {
            cd::GpuRow r;
            r.exePath = path;
            r.assignedKey = assigned;
            r.lostKey = lost;
            r.isProfileGame = profile;
            return cd::FactsOfRow(r, cd::GpuChoiceText(!assigned.empty(), cd::FormatPreferenceValue(assigned), false),
                                  mainKey);
        };
        typedef std::vector<cd::TickedRowFacts> Facts;
        Facts ticked;
        ticked.push_back(facts(L"C:\\B\\Browser.exe", L"", L"", false));
        ticked.push_back(facts(L"C:\\gone\\exited.exe", L"", L"", false));   // no longer running: no row to keep it on
        Facts rebuilt;                                                        // a new order, and Windows' casing this time
        rebuilt.push_back(facts(L"C:\\E\\editor.exe", L"", L"", false));
        rebuilt.push_back(facts(L"c:\\b\\browser.EXE", L"", L"", false));

        // 🔴 THE ROW AS IT WAS TICKED, NOT ONLY ITS PATH (Council round 2, v0.5.6). (a) the same program and GPU, but pinned to
        // the main GPU in Windows Settings between the visits: the tick goes - kept, Apply compared the new pin with itself
        // Every call here reads two complete walks of Windows' GPU preferences (the last two arguments); AJ45 covers the rest.
        CHECK(!cd::KeptTicks(bgKey, offered, ticked, Facts(1, facts(L"C:\\B\\Browser.exe", mainKey, L"", false)), true,
                             true)[0]);
        // ...and so it does for a choice that moved from one card to another, or went
        const Facts onBg(1, facts(L"C:\\N\\notes.exe", bgKey, L"", false));
        CHECK(!cd::KeptTicks(bgKey, offered, onBg, Facts(1, facts(L"C:\\N\\notes.exe", amdKey, L"", false)), true, true)[0]);
        CHECK(!cd::KeptTicks(bgKey, offered, onBg, Facts(1, facts(L"C:\\N\\notes.exe", L"", L"", false)), true, true)[0]);
        // (b) a deliberate tick on a row pinned to the main GPU, unchanged: kept, whatever the casing
        const Facts onMain(1, facts(L"C:\\M\\chat.exe", mainKey, L"", false));
        CHECK(onMain[0].mainGpuPin && !onMain[0].listedChoice.empty());
        CHECK(cd::KeptTicks(bgKey, offered, onMain, Facts(1, facts(L"c:\\m\\CHAT.exe", mainKey, L"", false)), true, true)[0]);
        // (c) the program became a profile game between the visits: dropped
        CHECK(!cd::KeptTicks(bgKey, offered, ticked, Facts(1, facts(L"C:\\B\\Browser.exe", L"", L"", true)), true, true)[0]);
        // (d) its main-GPU pin was newly found lost to an update: no GPU choice on either read, and still dropped
        const cd::TickedRowFacts orphaned = facts(L"C:\\B\\Browser.exe", L"", mainKey, false);
        CHECK(orphaned.listedChoice.empty() && orphaned.mainGpuPin);
        CHECK(!cd::KeptTicks(bgKey, offered, ticked, Facts(1, orphaned), true, true)[0]);
        // CONTROL for (a)-(d): the same row, unchanged, keeps its tick
        CHECK(cd::KeptTicks(bgKey, offered, ticked, Facts(1, facts(L"C:\\B\\Browser.exe", L"", L"", false)), true, true)[0]);

        // the GPU is still offered: the ticked program keeps its tick, whatever order or casing the rows come back in
        CHECK(cd::KeepsPickedTarget(bgKey, offered));
        const std::vector<bool> same = cd::KeptTicks(bgKey, offered, ticked, rebuilt, true, true);
        CHECK(same.size() == 2 && !same[0] && same[1]);

        // the GPU is no longer offered: no tick survives, and the picker goes back to its default
        std::vector<std::wstring> withoutBg;
        withoutBg.push_back(mainKey); withoutBg.push_back(amdKey);
        CHECK(!cd::KeepsPickedTarget(bgKey, withoutBg));
        const std::vector<bool> dropped = cd::KeptTicks(bgKey, withoutBg, ticked, rebuilt, true, true);
        CHECK(dropped.size() == 2 && !dropped[0] && !dropped[1]);
        // CONTROL: no GPU was picked before (the first visit) - nothing is kept either
        const std::vector<bool> first = cd::KeptTicks(std::wstring(), offered, ticked, rebuilt, true, true);
        CHECK(first.size() == 2 && !first[0] && !first[1]);

        // the notice: said for a new set of records...
        std::vector<std::wstring> shown;
        std::vector<std::wstring> records;
        records.push_back(L"C:\\Data\\gpu-preferences-before-2.pending");
        records.push_back(L"C:\\Data\\gpu-preferences-before-1.pending");
        CHECK(cd::ShouldPostUnfinished(shown, records));
        // ...and STILL said until it has really been shown: asking records nothing, so a post that failed, or a notice
        // that arrived while the tab was hidden, is said on the next visit (Council review, v0.5.6)...
        CHECK(cd::ShouldPostUnfinished(shown, records));
        CHECK(shown.empty());
        cd::MarkUnfinishedShown(shown, records);
        // ...not again, once shown, for the same set in another order and casing (arrowing back onto the tab)...
        std::vector<std::wstring> sameSet;
        sameSet.push_back(L"c:\\data\\GPU-PREFERENCES-BEFORE-1.pending");
        sameSet.push_back(L"C:\\Data\\gpu-preferences-before-2.pending");
        CHECK(!cd::ShouldPostUnfinished(shown, sameSet));
        // ...again when a record is added, until that set is shown...
        sameSet.push_back(L"C:\\Data\\gpu-preferences-before-3.pending");
        CHECK(cd::ShouldPostUnfinished(shown, sameSet));
        cd::MarkUnfinishedShown(shown, sameSet);
        CHECK(!cd::ShouldPostUnfinished(shown, sameSet));
        // ...never for no records, and again once records come back after they were all gone
        CHECK(!cd::ShouldPostUnfinished(shown, std::vector<std::wstring>()));
        CHECK(shown.empty());
        CHECK(cd::ShouldPostUnfinished(shown, records));
    }

    Case("AJ40 with nothing to tick, the tab never calls a main-GPU pin already assigned to the chosen GPU");
    {
        // no pin: the tab's sentence. Any pin: the pins are named, and why they stay. "Assigned to", never "on": the tab
        // cannot tell where a running application renders (Council review, v0.5.6).
        CHECK_EQ(cd::FormatNothingToMoveLine(0),
                 std::wstring(L"Every application that can be moved is already assigned to that GPU."));
        const std::wstring pinned = L"Every application that can be moved is already assigned to that GPU or pinned to "
                                    L"the main GPU. Auto assign GPU for Gaming leaves applications pinned to the main GPU "
                                    L"where they are.";
        CHECK(cd::FormatNothingToMoveLine(0).find(L"already on") == std::wstring::npos);
        CHECK(pinned.find(L"already on") == std::wstring::npos);
        CHECK_EQ(cd::FormatNothingToMoveLine(1), pinned);
        CHECK_EQ(cd::FormatNothingToMoveLine(3), pinned);

        // which rows count as a pin - the rows decision 10 skips, and no others
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        cd::GpuRow onMain;     onMain.assignedKey = mainKey;
        cd::GpuRow lostMain;   lostMain.lostKey = mainKey;                                 // updated into a new folder
        cd::GpuRow lostButSet; lostButSet.lostKey = mainKey; lostButSet.assignedKey = bgKey;   // holds a value now
        cd::GpuRow onBg;       onBg.assignedKey = bgKey;
        cd::GpuRow none;
        CHECK(cd::IsMainGpuPin(onMain, mainKey));
        CHECK(cd::IsMainGpuPin(lostMain, mainKey));
        CHECK(!cd::IsMainGpuPin(lostButSet, mainKey));
        CHECK(!cd::IsMainGpuPin(onBg, mainKey));
        CHECK(!cd::IsMainGpuPin(none, mainKey));
        // CONTROL: with no main GPU known, nothing is a pin - not even a row with no assignment
        CHECK(!cd::IsMainGpuPin(none, std::wstring()));

        // and the selector reads the same rule: both pins stay unticked for the background GPU, the unassigned row is ticked
        std::vector<cd::GpuRow> rows;
        rows.push_back(onMain); rows.push_back(lostMain); rows.push_back(lostButSet); rows.push_back(none);
        const std::vector<cd::GpuRow> picked = cd::SelectForBackground(rows, bgKey, mainKey);
        CHECK(picked.size() == 4 && !picked[0].selected && !picked[1].selected && !picked[2].selected &&
              picked[3].selected);
    }

    Case("AJ41 Apply and Remove refuse a row whose GPU choice changed after the list was shown, and only that");
    {
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        const std::wstring mainPin = cd::FormatPreferenceValue(mainKey), bgPin = cd::FormatPreferenceValue(bgKey);
        typedef cd::GpuPrepareVerdict V;

        // APPLY. Listed with no value, then pinned to the main GPU in Windows Settings before Apply read it: refused, and
        // the reason says why. A tick Auto assign made and one made by hand are the same row here.
        const std::wstring listedNone = cd::GpuChoiceText(false, std::wstring(), false);
        CHECK(listedNone.empty());
        CHECK(cd::PrepareVerdict(listedNone, true, mainPin + L"AppStatus=1;", false) == V::ChangedSinceListed);
        CHECK(cd::PrepareRefusalReason(V::ChangedSinceListed).find(L"changed after the list was shown") !=
              std::wstring::npos);
        // only Windows' own fields changed - a value with no GPU field appeared, or the fields around a pin changed: proceeds
        CHECK(cd::PrepareVerdict(listedNone, true, L"AppStatus=1;AutoHDREnable=2097;", false) == V::Ready);
        const std::wstring listedMain = cd::GpuChoiceText(true, L"AppStatus=1;" + mainPin, false);
        CHECK_EQ(listedMain, mainPin);
        CHECK(cd::PrepareVerdict(listedMain, true, mainPin + L"AppStatus=2;AutoHDREnable=2097;", false) == V::Ready);
        // the same choice: proceeds
        CHECK(cd::PrepareVerdict(listedNone, false, std::wstring(), false) == V::Ready);
        CHECK(cd::PrepareVerdict(listedMain, true, mainPin, false) == V::Ready);
        // Windows' explicit "Let Windows decide" is a choice, although it names no card
        CHECK(cd::PrepareVerdict(listedNone, true, L"GpuPreference=0;", false) == V::ChangedSinceListed);

        // REMOVE. Listed on the main GPU, then moved to the background GPU, or unassigned, before Remove read it: refused
        CHECK(cd::PrepareVerdict(listedMain, true, bgPin, false) == V::ChangedSinceListed);
        CHECK(cd::PrepareVerdict(listedMain, false, std::wstring(), false) == V::ChangedSinceListed);
        CHECK(cd::PrepareVerdict(listedMain, true, L"AutoHDREnable=2097;", false) == V::ChangedSinceListed);

        // the older refusals keep their words, and their places: unreadable first, an uneditable value only when unchanged
        CHECK(cd::PrepareVerdict(listedNone, false, std::wstring(), true) == V::Unreadable);
        CHECK(cd::PrepareVerdict(listedMain, true, mainPin + L"junk", false) == V::NotEditable);
        CHECK(cd::PrepareVerdict(cd::UnreadableChoiceKey(), true, mainPin, false) == V::ChangedSinceListed);
        CHECK_EQ(cd::PrepareRefusalReason(V::Unreadable),
                 std::wstring(L"its current value could not be read, so it was left alone"));
        CHECK_EQ(cd::PrepareRefusalReason(V::NotEditable),
                 std::wstring(L"its current value is in a form this does not edit, so it was left alone"));
        CHECK(cd::PrepareRefusalReason(V::Ready).empty());

        // A ROW THIS TAB CHANGED IS NOT "CHANGED AFTER THE LIST WAS SHOWN" ON THE NEXT APPLY: each result carries the GPU
        // choice that read back, and the window keeps it as the row's listed choice
        FakeGpuEdits f;
        const std::wstring p = L"C:\\T\\tool.exe";
        f.reg[p] = L"AppStatus=1;";
        const std::vector<cd::GpuEditResult> r =
            cd::RunGpuEdits(f.Items(std::vector<std::wstring>(1, p)), false, bgKey, f.Ops());
        CHECK(r.size() == 1 && r[0].outcome == cd::GpuEditOutcome::Done);
        CHECK(r.size() == 1 && r[0].choiceText == bgPin);
        CHECK(r.size() == 1 && cd::PrepareVerdict(r[0].choiceText, true, f.reg[p], false) == V::Ready);

        // 🔴 PASS TWO (Council round 2, v0.5.6). A row it did not finish is unticked when the GPU choice read after the run is
        // neither the listed one nor the one the run meant to leave - another program changed it meanwhile - and only then.
        typedef cd::GpuEditOutcome Oc;
        CHECK(cd::UntickAfterRun(Oc::Refused, listedNone, mainPin, bgPin));
        CHECK(cd::UntickAfterRun(Oc::Unconfirmed, listedNone, mainPin, bgPin));
        CHECK(cd::UntickAfterRun(Oc::NotAttempted, listedMain, bgPin, std::wstring()));
        // the GPU choice unchanged (Windows rewrote AppStatus, say), or what this run meant to write: the tick stays
        CHECK(!cd::UntickAfterRun(Oc::Refused, listedNone, listedNone, bgPin));
        CHECK(!cd::UntickAfterRun(Oc::NotAttempted, listedMain, listedMain, std::wstring()));
        CHECK(!cd::UntickAfterRun(Oc::Unconfirmed, listedNone, bgPin, bgPin));
        // a read-back that failed is no evidence of a change - and the next Apply refuses a row listed as unreadable
        CHECK(!cd::UntickAfterRun(Oc::Unconfirmed, listedNone, cd::UnreadableChoiceKey(), bgPin));
        // Done and AlreadyDone are unticked by their own rule, and never counted as changed
        CHECK(!cd::UntickAfterRun(Oc::Done, listedNone, bgPin, bgPin));
        CHECK(!cd::UntickAfterRun(Oc::AlreadyDone, listedNone, mainPin, bgPin));
        // what a run means to leave: Apply's merged GPU fields; Remove's, none, whether a value is left or deleted
        cd::GpuEditItem pinnedItem;
        pinnedItem.exePath = p;
        pinnedItem.present = true;
        pinnedItem.existing = mainPin + L"AppStatus=1;";
        CHECK_EQ(cd::IntendedChoiceText(pinnedItem, false, bgKey), bgPin);
        CHECK(cd::IntendedChoiceText(pinnedItem, true, std::wstring()).empty());
        pinnedItem.existing = mainPin;
        CHECK(cd::IntendedChoiceText(pinnedItem, true, std::wstring()).empty());

        // driven through RunGpuEdits as RunEdits drives it: as each guarded write happens, another program pins one row to the
        // main GPU, and rewrites only Windows' own fields of the other - both refused
        FakeGpuEdits g;
        const std::wstring pinnedPath = L"C:\\T\\pinned.exe", touchedPath = L"C:\\T\\touched.exe";
        g.reg[pinnedPath] = L"AppStatus=1;";
        g.reg[touchedPath] = L"AppStatus=1;";
        g.result[pinnedPath] = cd::GuardedWriteResult::Changed;
        g.otherProgram[pinnedPath] = mainPin + L"AppStatus=1;";
        g.result[touchedPath] = cd::GuardedWriteResult::Changed;
        g.otherProgram[touchedPath] = L"AppStatus=2;";
        std::vector<std::wstring> both;
        both.push_back(pinnedPath);
        both.push_back(touchedPath);
        const std::vector<cd::GpuEditItem> items = g.Items(both);
        const std::vector<cd::GpuEditResult> rr = cd::RunGpuEdits(items, false, bgKey, g.Ops());
        CHECK(rr.size() == 2 && rr[0].outcome == Oc::Refused && rr[1].outcome == Oc::Refused);
        if (rr.size() == 2 && items.size() == 2) {
            const std::wstring listed0 = cd::GpuChoiceText(items[0].present, items[0].existing, false);
            const std::wstring listed1 = cd::GpuChoiceText(items[1].present, items[1].existing, false);
            CHECK(cd::UntickAfterRun(rr[0].outcome, listed0, rr[0].choiceText, cd::IntendedChoiceText(items[0], false, bgKey)));
            CHECK(!cd::UntickAfterRun(rr[1].outcome, listed1, rr[1].choiceText, cd::IntendedChoiceText(items[1], false, bgKey)));
            // CONTROL - why it must be unticked: re-listed at the pin, a retry's pass one finds nothing changed and goes on
            CHECK(cd::PrepareVerdict(rr[0].choiceText, true, g.reg[pinnedPath], false) == V::Ready);
        }
    }

    Case("AJ42 a value that names no GPU is no GPU choice: a main-GPU pin lost to an update stays unticked");
    {
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        const std::wstring oldPath = L"C:\\Apps\\Chat\\app-1.9\\chat.exe", newPath = L"C:\\Apps\\Chat\\app-1.10\\chat.exe";
        const std::wstring otherPath = L"C:\\Apps\\Notes\\notes.exe";
        // what a walk of the key reads: the old version's main-GPU pin, and the value Windows made for the new version
        std::vector<cd::GpuPreferenceEntry> entries;
        entries.push_back(cd::MakeGpuPreferenceEntry(oldPath, cd::FormatPreferenceValue(mainKey) + L"AppStatus=1;", false));
        entries.push_back(cd::MakeGpuPreferenceEntry(newPath, L"AppStatus=1;AutoHDREnable=2097;", false));
        CHECK(entries[1].choiceKey.empty() && entries[1].choiceText.empty());
        std::vector<std::wstring> running;
        running.push_back(newPath);
        running.push_back(otherPath);
        const auto exists = [&oldPath](const std::wstring& path) { return path != oldPath; };   // the old version is gone

        // the pairs the tab works from leave the Windows-only value out, so the detector finds the lost pin...
        const std::vector<std::pair<std::wstring, std::wstring> > reg = cd::GpuChoicePairs(entries);
        CHECK(reg.size() == 1 && reg[0].first == oldPath && reg[0].second == mainKey);
        const std::vector<cd::OrphanedAssignment> lost = cd::FindOrphanedAssignments(reg, running, exists);
        CHECK(lost.size() == 1 && lost[0].livePath == newPath && lost[0].lostKey == mainKey);

        // ...and rows built from both the way the tab builds them - the assigned key from the pairs, the lost key from the
        // detector - leave the new version unticked, while an application with no value at all is ticked
        std::vector<cd::GpuRow> rows(2);
        rows[0].exeName = L"chat.exe";
        rows[0].exePath = newPath;
        rows[1].exeName = L"notes.exe";
        rows[1].exePath = otherPath;
        for (size_t i = 0; i < rows.size(); ++i) {
            for (size_t k = 0; k < reg.size(); ++k)
                if (cd::WcsIcmp(reg[k].first, rows[i].exePath)) rows[i].assignedKey = reg[k].second;
            for (size_t k = 0; k < lost.size(); ++k)
                if (cd::WcsIcmp(lost[k].livePath, rows[i].exePath)) rows[i].lostKey = lost[k].lostKey;
        }
        const std::vector<cd::GpuRow> picked = cd::SelectForBackground(rows, bgKey, mainKey);
        CHECK(picked.size() == 2 && !picked[0].selected && picked[1].selected);

        // CONTROL - every value as a pair, as before this fix: the Windows-only value hides the lost pin
        std::vector<std::pair<std::wstring, std::wstring> > every;
        for (size_t i = 0; i < entries.size(); ++i) every.push_back(std::make_pair(entries[i].path, entries[i].choiceKey));
        CHECK(cd::FindOrphanedAssignments(every, running, exists).empty());

        // "GpuPreference=0" at the new path names no GPU either, so the lost pin is still found (Council round 2, v0.5.6: [M]
        // Windows' own graphics options write it beside SwapEffectUpgradeEnable) - while Apply still compares it (AJ41)...
        entries[1] = cd::MakeGpuPreferenceEntry(newPath, L"SwapEffectUpgradeEnable=1;GpuPreference=0;", false);
        CHECK(!entries[1].choiceText.empty() && entries[1].choiceKey.empty());
        CHECK(cd::GpuChoicePairs(entries).size() == 1);
        const std::vector<cd::OrphanedAssignment> lostBesideMode0 =
            cd::FindOrphanedAssignments(cd::GpuChoicePairs(entries), running, exists);
        CHECK(lostBesideMode0.size() == 1 && lostBesideMode0[0].lostKey == mainKey);
        // ...while a card, or one of Windows' two modes, at the new path IS a choice, so nothing was lost there
        entries[1] = cd::MakeGpuPreferenceEntry(newPath, L"GpuPreference=2;AppStatus=1;", false);
        CHECK(cd::FindOrphanedAssignments(cd::GpuChoicePairs(entries), running, exists).empty());
        entries[1] = cd::MakeGpuPreferenceEntry(newPath, cd::FormatPreferenceValue(bgKey), false);
        CHECK(cd::FindOrphanedAssignments(cd::GpuChoicePairs(entries), running, exists).empty());
        // ...and a value that could not be read is never taken for no value
        entries[1] = cd::MakeGpuPreferenceEntry(newPath, std::wstring(), true);
        const std::vector<std::pair<std::wstring, std::wstring> > unread = cd::GpuChoicePairs(entries);
        CHECK(unread.size() == 2 && unread[1].second == cd::UnreadableChoiceKey());
        CHECK(cd::FindOrphanedAssignments(unread, running, exists).empty());
    }

    Case("AJ43 the panel's state outlives every message box open when the panel is destroyed, and is freed once");
    {
        // no box open: WM_NCDESTROY frees it at once
        cd::PanelLifetime idle;
        CHECK(cd::DetachPanel(idle));
        // a box open: WM_NCDESTROY leaves it, and the box's return frees it
        cd::PanelLifetime one;
        cd::EnterModal(one);
        CHECK(!cd::DetachPanel(one));
        CHECK(one.detached);
        CHECK(cd::LeaveModal(one));
        // a notice raised inside Apply's question: only the OUTER return frees it, and the inner one already sees `detached`
        cd::PanelLifetime nested;
        cd::EnterModal(nested);
        cd::EnterModal(nested);
        CHECK(!cd::DetachPanel(nested));
        CHECK(!cd::LeaveModal(nested) && nested.detached);
        CHECK(cd::LeaveModal(nested));
        // CONTROL - never destroyed: no return frees it, nested or not
        cd::PanelLifetime alive;
        cd::EnterModal(alive);
        cd::EnterModal(alive);
        CHECK(!cd::LeaveModal(alive));
        CHECK(!cd::LeaveModal(alive));
        CHECK(!alive.detached && alive.busy == 0);
    }

    Case("AJ44 with no GPU chosen, a lost assignment is said with no instruction nobody can follow, nor 'no GPU' under GPUs");
    {
        CHECK(cd::FormatLostWithoutTargetLine(0, false).empty());
        CHECK(cd::FormatLostWithoutTargetLine(0, true).empty());
        // the picker offers nothing
        CHECK_EQ(cd::FormatLostWithoutTargetLine(1, false),
                 std::wstring(L"1 app lost its GPU assignment after an update. No GPU is available to assign it to "
                              L"right now."));
        CHECK_EQ(cd::FormatLostWithoutTargetLine(2, false),
                 std::wstring(L"2 apps lost their GPU assignment after an update. No GPU is available to assign them "
                              L"to right now."));
        // the picker still offers GPUs, with none chosen - a target that was dropped (Council round 2, v0.5.6)
        CHECK_EQ(cd::FormatLostWithoutTargetLine(1, true),
                 std::wstring(L"1 app lost its GPU assignment after an update. Choose a GPU above to assign it again."));
        CHECK_EQ(cd::FormatLostWithoutTargetLine(2, true),
                 std::wstring(L"2 apps lost their GPU assignment after an update. Choose a GPU above to assign them "
                              L"again."));
        for (int canChoose = 0; canChoose < 2; ++canChoose) {
            for (size_t n = 1; n <= 2; ++n) {
                const std::wstring s = cd::FormatLostWithoutTargetLine(n, canChoose != 0);
                CHECK(s.find(L"Apply") == std::wstring::npos);
                CHECK(s.find(L"chosen above") == std::wstring::npos);
                CHECK((s.find(L"No GPU is available") == std::wstring::npos) == (canChoose != 0));
            }
        }
        // CONTROL: the sentence this case used before told the user to Apply to a GPU "chosen above"
        CHECK(cd::FormatGpuIsolateStatusLine(0, 2, 1).find(L"chosen above") != std::wstring::npos);
    }

    Case("AJ45 while Windows' GPU preferences could not all be read, Auto assign ticks nothing and a refresh after a "
         "complete read keeps no tick");
    {
        // Council round 2, v0.5.6 (F1): an incomplete walk of the key can leave out the old version's main-GPU pin, and the
        // new version then reads as an application nobody pinned.
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        const std::wstring oldPath = L"C:\\Apps\\Chat\\app-1.9\\chat.exe", newPath = L"C:\\Apps\\Chat\\app-1.10\\chat.exe";
        const std::vector<std::wstring> running(1, newPath);
        const auto exists = [&oldPath](const std::wstring& path) { return path != oldPath; };   // the old version is gone
        // the new version's row, built from a walk the way the tab builds it: assigned key from the pairs, lost key from the detector
        const auto rowFrom = [&](const std::vector<cd::GpuPreferenceEntry>& entries) {
            const std::vector<std::pair<std::wstring, std::wstring> > reg = cd::GpuChoicePairs(entries);
            cd::GpuRow r;
            r.exeName = L"chat.exe";
            r.exePath = newPath;
            for (size_t k = 0; k < reg.size(); ++k)
                if (cd::WcsIcmp(reg[k].first, newPath)) r.assignedKey = reg[k].second;
            const std::vector<cd::OrphanedAssignment> lost = cd::FindOrphanedAssignments(reg, running, exists);
            for (size_t k = 0; k < lost.size(); ++k)
                if (cd::WcsIcmp(lost[k].livePath, newPath)) r.lostKey = lost[k].lostKey;
            return std::vector<cd::GpuRow>(1, r);
        };
        const cd::GpuPreferenceEntry oldPin =
            cd::MakeGpuPreferenceEntry(oldPath, cd::FormatPreferenceValue(mainKey) + L"AppStatus=1;", false);
        const cd::GpuPreferenceEntry newMeta = cd::MakeGpuPreferenceEntry(newPath, L"AppStatus=1;AutoHDREnable=2097;", false);
        CHECK(newMeta.choiceKey.empty() && newMeta.choiceText.empty());   // readable, and only Windows' own fields

        // no other version of the application in the walk: Auto assign's AnotherVersionMayHoldMainGpuPin has nothing to see (AJ46)
        const std::vector<std::pair<std::wstring, std::wstring> > noOtherVersion;

        // INCOMPLETE: the walk read the new path's value and never reached the old pin. Nothing on the row says "pinned"...
        const std::vector<cd::GpuPreferenceEntry> partialWalk(1, newMeta);
        const std::vector<cd::GpuRow> partial = rowFrom(partialWalk);
        CHECK(partial[0].assignedKey.empty() && partial[0].lostKey.empty());
        // ...so Auto assign, told the walk was incomplete, selects nothing
        const std::vector<cd::GpuRow> incomplete =
            cd::SelectForAutoAssign(partial, bgKey, mainKey, false, cd::GpuChoicePairs(partialWalk));
        CHECK(incomplete.size() == 1 && !incomplete[0].selected);
        // CONTROL - why it must refuse: the same row through the selector alone, or told the walk was complete, is ticked
        CHECK(cd::SelectForBackground(partial, bgKey, mainKey)[0].selected);
        CHECK(cd::SelectForAutoAssign(partial, bgKey, mainKey, true, cd::GpuChoicePairs(partialWalk))[0].selected);

        // COMPLETE, the old pin present: found lost to the update, and skipped through lostKey
        std::vector<cd::GpuPreferenceEntry> whole;
        whole.push_back(oldPin);
        whole.push_back(newMeta);
        const std::vector<cd::GpuRow> found = rowFrom(whole);
        CHECK(found[0].assignedKey.empty() && found[0].lostKey == mainKey);
        CHECK(!cd::SelectForAutoAssign(found, bgKey, mainKey, true, noOtherVersion)[0].selected);
        // CONTROL - it is the lost key that skips it: the same row without it, and with no other version to see, is ticked
        std::vector<cd::GpuRow> withoutLost = found;
        withoutLost[0].lostKey.clear();
        CHECK(cd::SelectForAutoAssign(withoutLost, bgKey, mainKey, true, noOtherVersion)[0].selected);

        // the rule: both reasons, alone and together; the main GPU as target ticks nothing, complete or not
        CHECK(cd::AutoAssignAllowed(true, false));
        CHECK(!cd::AutoAssignAllowed(false, false));
        CHECK(!cd::AutoAssignAllowed(true, true));
        CHECK(!cd::AutoAssignAllowed(false, true));
        CHECK(!cd::SelectForAutoAssign(withoutLost, mainKey, mainKey, true, noOtherVersion)[0].selected);
        // CONTROL: no main GPU known is not "the main GPU is the target"
        CHECK(cd::SelectForAutoAssign(withoutLost, bgKey, std::wstring(), true, noOtherVersion)[0].selected);

        // the reason beside the greyed button
        CHECK_EQ(cd::FormatIncompleteScanStatusLine(),
                 std::wstring(L"Auto assign GPU for Gaming is off because Windows' GPU settings could not all be read, so "
                              L"an application pinned to the main GPU might not be recognised. Tick applications by hand "
                              L"to change them."));

        // A REFRESH WHOSE WALK WAS INCOMPLETE, AFTER ONE THAT WAS COMPLETE, KEEPS NO TICK - not even one whose row did not
        // change, since Auto assign may have made it on that complete visit
        std::vector<std::wstring> offered;
        offered.push_back(mainKey);
        offered.push_back(bgKey);
        const std::vector<cd::TickedRowFacts> ticked(1, cd::FactsOfRow(withoutLost[0], std::wstring(), mainKey));
        CHECK(!cd::KeptTicks(bgKey, offered, ticked, ticked, true, false)[0]);
        // ...but after an incomplete walk every tick was made by hand - Auto assign ticked nothing - so the usual rules keep an
        // unchanged row's tick, as on the refresh an Apply asks for after refusing rows (Council round 2 fix check, v0.5.6)...
        CHECK(cd::KeptTicks(bgKey, offered, ticked, ticked, false, false)[0]);
        // ...and still drop a row that changed: pinned to the main GPU in Windows Settings meanwhile
        cd::GpuRow pinnedMeanwhile = withoutLost[0];
        pinnedMeanwhile.assignedKey = mainKey;
        const std::vector<cd::TickedRowFacts> nowPinned(
            1, cd::FactsOfRow(pinnedMeanwhile, cd::GpuChoiceText(true, cd::FormatPreferenceValue(mainKey), false), mainKey));
        CHECK(!cd::KeptTicks(bgKey, offered, ticked, nowPinned, false, false)[0]);
        // CONTROL: a complete walk keeps it, after either
        CHECK(cd::KeptTicks(bgKey, offered, ticked, ticked, true, true)[0]);
        CHECK(cd::KeptTicks(bgKey, offered, ticked, ticked, false, true)[0]);
    }

    Case("AJ46 Auto assign leaves an application alone while another version of it may hold a main-GPU pin, lost or not");
    {
        // Council round 2 fix check, v0.5.6: a pin is found lost only when Windows says the old path is gone, so an updater
        // that keeps the previous version folder, an old path that cannot be probed, or an old value that cannot be read left
        // the new version with no lostKey - and Auto assign, from a complete walk, ticked it for the background GPU.
        const std::wstring mainKey = L"10DE&2B85&53021462", bgKey = L"10DE&2684&40BF1458";
        const std::wstring oldPath = L"C:\\Apps\\Chat\\app-1.9\\chat.exe", newPath = L"C:\\Apps\\Chat\\app-1.10\\chat.exe";
        const std::vector<std::wstring> running(1, newPath);
        typedef std::vector<std::pair<std::wstring, std::wstring> > Pairs;
        const cd::GpuPreferenceEntry newMeta = cd::MakeGpuPreferenceEntry(newPath, L"AppStatus=1;AutoHDREnable=2097;", false);
        // a complete walk holding the old version's value `old` and the new path's own fields
        const auto walk = [&newMeta](const cd::GpuPreferenceEntry& old) {
            std::vector<cd::GpuPreferenceEntry> entries;
            entries.push_back(old);
            entries.push_back(newMeta);
            return cd::GpuChoicePairs(entries);
        };
        // the new version's row, built the way the tab builds it; `oldExists` is what the disk probe answers for the old path
        const auto rowFrom = [&](const Pairs& reg, bool oldExists) {
            cd::GpuRow r;
            r.exeName = L"chat.exe";
            r.exePath = newPath;
            for (size_t k = 0; k < reg.size(); ++k)
                if (cd::WcsIcmp(reg[k].first, newPath)) r.assignedKey = reg[k].second;
            const std::vector<cd::OrphanedAssignment> lost = cd::FindOrphanedAssignments(
                reg, running, [&](const std::wstring& path) { return path != oldPath || oldExists; });
            for (size_t k = 0; k < lost.size(); ++k)
                if (cd::WcsIcmp(lost[k].livePath, newPath)) r.lostKey = lost[k].lostKey;
            return std::vector<cd::GpuRow>(1, r);
        };
        const Pairs none;

        // (a) THE OLD VERSION FOLDER IS STILL ON DISK - or could not be probed, which MarkOrphans also answers "exists" - and
        // its value names the main GPU: nothing on the row says so...
        const Pairs keptPin = walk(cd::MakeGpuPreferenceEntry(oldPath, cd::FormatPreferenceValue(mainKey), false));
        const std::vector<cd::GpuRow> kept = rowFrom(keptPin, true);
        CHECK(kept[0].assignedKey.empty() && kept[0].lostKey.empty() && !cd::IsMainGpuPin(kept[0], mainKey));
        CHECK(cd::AnotherVersionMayHoldMainGpuPin(kept[0], keptPin, mainKey));
        // ...and Auto assign leaves it alone
        CHECK(!cd::SelectForAutoAssign(kept, bgKey, mainKey, true, keptPin)[0].selected);
        // CONTROL - why it must: the selector alone ticks it, and so does Auto assign shown no other version
        CHECK(cd::SelectForBackground(kept, bgKey, mainKey)[0].selected);
        CHECK(cd::SelectForAutoAssign(kept, bgKey, mainKey, true, none)[0].selected);

        // (b) THE OLD VALUE COULD NOT BE READ, and the old folder is gone: no lost assignment is reported, and it is not ticked
        const Pairs unreadable = walk(cd::MakeGpuPreferenceEntry(oldPath, std::wstring(), true));
        CHECK(unreadable.size() == 1 && unreadable[0].second == cd::UnreadableChoiceKey());
        const std::vector<cd::GpuRow> unread = rowFrom(unreadable, false);
        CHECK(unread[0].assignedKey.empty() && unread[0].lostKey.empty());
        CHECK(!cd::SelectForAutoAssign(unread, bgKey, mainKey, true, unreadable)[0].selected);

        // CONTROL: the old version on the BACKGROUND GPU, its folder kept or gone, is no reason to leave it alone - ticked
        const Pairs bgPin = walk(cd::MakeGpuPreferenceEntry(oldPath, cd::FormatPreferenceValue(bgKey), false));
        CHECK(!cd::AnotherVersionMayHoldMainGpuPin(rowFrom(bgPin, true)[0], bgPin, mainKey));
        CHECK(cd::SelectForAutoAssign(rowFrom(bgPin, true), bgKey, mainKey, true, bgPin)[0].selected);
        CHECK(cd::SelectForAutoAssign(rowFrom(bgPin, false), bgKey, mainKey, true, bgPin)[0].selected);

        // CONTROL: a main-GPU pin of ANOTHER application - another install root, or another file name - is not this one's
        Pairs others;
        others.push_back(std::make_pair(std::wstring(L"C:\\Apps\\Talk\\app-1.9\\chat.exe"), mainKey));
        others.push_back(std::make_pair(std::wstring(L"C:\\Apps\\Chat\\app-1.9\\helper.exe"), mainKey));
        CHECK(!cd::AnotherVersionMayHoldMainGpuPin(kept[0], others, mainKey));
        CHECK(cd::SelectForAutoAssign(kept, bgKey, mainKey, true, others)[0].selected);

        // CONTROL: an application with a GPU value of its own is judged on that value, as before; no main GPU known, no pin
        std::vector<cd::GpuRow> onAmd = kept;
        onAmd[0].assignedKey = L"1002&13C0&88771043";
        CHECK(!cd::AnotherVersionMayHoldMainGpuPin(onAmd[0], keptPin, mainKey));
        CHECK(cd::SelectForAutoAssign(onAmd, bgKey, mainKey, true, keptPin)[0].selected);
        CHECK(!cd::AnotherVersionMayHoldMainGpuPin(kept[0], keptPin, std::wstring()));
    }
}

void Test_AH5_StatusLine() {
    Case("AH5a FormatGpuIsolateStatusLine: gpuCount < 2 returns empty");
    std::wstring result = cd::FormatGpuIsolateStatusLine(5, 1);
    CHECK_EQ(result, std::wstring(L""));

    Case("AH5b FormatGpuIsolateStatusLine: movableApps = 0 returns empty");
    result = cd::FormatGpuIsolateStatusLine(0, 2);
    CHECK_EQ(result, std::wstring(L""));

    Case("AH5c FormatGpuIsolateStatusLine: 1 app uses singular 'app'");
    result = cd::FormatGpuIsolateStatusLine(1, 2);
    CHECK(result.find(L" app ") != std::wstring::npos);
    CHECK(result.find(L" apps ") == std::wstring::npos);

    Case("AH5d FormatGpuIsolateStatusLine: 7 apps uses plural 'apps'");
    result = cd::FormatGpuIsolateStatusLine(7, 2);
    CHECK(result.find(L" apps ") != std::wstring::npos);
    CHECK(result.find(L"7 background") != std::wstring::npos);
}

void Test_AH6_RowsNeedingRestart() {
    Case("AH6a RowsNeedingRestart: returns StaleNotApplied rows only");
    std::vector<cd::GpuRow> rows;

    cd::GpuRow staleRow;
    staleRow.exeName = L"stale.exe";
    staleRow.assignedKey = L"10DE&2B85&53021462";
    staleRow.runningKey = L"DIFFERENT&KEY&HERE";
    staleRow.isProfileGame = false;
    rows.push_back(staleRow);

    cd::GpuRow correctRow;
    correctRow.exeName = L"correct.exe";
    correctRow.assignedKey = L"10DE&2B85&53021462";
    correctRow.runningKey = L"10DE&2B85&53021462";
    correctRow.isProfileGame = false;
    rows.push_back(correctRow);

    cd::GpuRow missingRow;
    missingRow.exeName = L"missing.exe";
    missingRow.assignedKey = L"";
    missingRow.runningKey = L"10DE&2B85&53021462";
    missingRow.isProfileGame = false;
    rows.push_back(missingRow);

    std::vector<cd::GpuRow> result = cd::RowsNeedingRestart(rows, L"10DE&2B85&53021462");
    CHECK_EQ(result.size(), (size_t)1);
    CHECK_EQ(result[0].exeName, std::wstring(L"stale.exe"));
}

void Test_AI1_FindOrphanedAssignments() {
    Case("AI1a FindOrphanedAssignments: exact Claude case - three stale, one live without entry");

    // Build the registry: three stale Claude entries, nothing else
    std::vector<std::pair<std::wstring, std::wstring>> registry;
    registry.push_back({L"C:\\AnthropicClaude\\app-1.24012.9\\claude.exe", L"10DE&2684&40BF1458"});
    registry.push_back({L"C:\\AnthropicClaude\\app-1.26832.0\\claude.exe", L"10DE&2684&40BF1458"});
    registry.push_back({L"C:\\AnthropicClaude\\app-1.49585.0\\claude.exe", L"10DE&2684&40BF1458"});

    // The running process is the live one
    std::vector<std::wstring> running = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    // Mock pathExists: all three stale paths do not exist, the live one does
    auto pathExists = [](const std::wstring& path) {
        if (path.find(L"app-1.52386.3") != std::wstring::npos) return true;
        return false;
    };

    std::vector<cd::OrphanedAssignment> result = cd::FindOrphanedAssignments(registry, running, pathExists);

    CHECK_EQ(result.size(), (size_t)1);
    CHECK_EQ(result[0].exeName, std::wstring(L"claude.exe"));
    CHECK_EQ(result[0].livePath, std::wstring(L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"));
    CHECK_EQ(result[0].lostKey, std::wstring(L"10DE&2684&40BF1458"));
    // The stalePath should be the LAST-SORTING one (most recent version)
    CHECK_EQ(result[0].stalePath, std::wstring(L"C:\\AnthropicClaude\\app-1.49585.0\\claude.exe"));

    Case("AI1b FindOrphanedAssignments: live path already has entry - no orphan reported");
    std::vector<std::pair<std::wstring, std::wstring>> registry2;
    registry2.push_back({L"C:\\AnthropicClaude\\app-1.24012.9\\claude.exe", L"10DE&2684&40BF1458"});
    registry2.push_back({L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe", L"10DE&2B85&53021462"});

    std::vector<std::wstring> running2 = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    auto pathExists2 = [](const std::wstring&) { return false; };

    std::vector<cd::OrphanedAssignment> result2 = cd::FindOrphanedAssignments(registry2, running2, pathExists2);
    CHECK_EQ(result2.size(), (size_t)0);

    Case("AI1c FindOrphanedAssignments: stale basename no match - not reported");
    std::vector<std::pair<std::wstring, std::wstring>> registry3;
    registry3.push_back({L"C:\\OtherApp\\app-1.0\\other.exe", L"10DE&2684&40BF1458"});

    std::vector<std::wstring> running3 = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    auto pathExists3 = [](const std::wstring&) { return false; };

    std::vector<cd::OrphanedAssignment> result3 = cd::FindOrphanedAssignments(registry3, running3, pathExists3);
    CHECK_EQ(result3.size(), (size_t)0);

    Case("AI1d FindOrphanedAssignments: stale entry with empty adapter key - not reported");
    std::vector<std::pair<std::wstring, std::wstring>> registry4;
    registry4.push_back({L"C:\\AnthropicClaude\\app-1.24012.9\\claude.exe", L""});

    std::vector<std::wstring> running4 = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    auto pathExists4 = [](const std::wstring&) { return false; };

    std::vector<cd::OrphanedAssignment> result4 = cd::FindOrphanedAssignments(registry4, running4, pathExists4);
    CHECK_EQ(result4.size(), (size_t)0);

    Case("AI1e FindOrphanedAssignments: stale path still exists on disk - not reported");
    std::vector<std::pair<std::wstring, std::wstring>> registry5;
    registry5.push_back({L"C:\\AnthropicClaude\\app-1.24012.9\\claude.exe", L"10DE&2684&40BF1458"});

    std::vector<std::wstring> running5 = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    auto pathExists5 = [](const std::wstring&) { return true; };

    std::vector<cd::OrphanedAssignment> result5 = cd::FindOrphanedAssignments(registry5, running5, pathExists5);
    CHECK_EQ(result5.size(), (size_t)0);

    Case("AI1f FindOrphanedAssignments: case-insensitive basename match");
    std::vector<std::pair<std::wstring, std::wstring>> registry6;
    registry6.push_back({L"C:\\AnthropicClaude\\app-1.24012.9\\CLAUDE.EXE", L"10DE&2684&40BF1458"});

    std::vector<std::wstring> running6 = {L"C:\\AnthropicClaude\\app-1.52386.3\\claude.exe"};

    auto pathExists6 = [](const std::wstring& path) {
        if (path.find(L"app-1.52386.3") != std::wstring::npos) return true;
        return false;
    };

    std::vector<cd::OrphanedAssignment> result6 = cd::FindOrphanedAssignments(registry6, running6, pathExists6);
    CHECK_EQ(result6.size(), (size_t)1);
    CHECK_EQ(result6[0].exeName, std::wstring(L"claude.exe"));

    Case("AI1g FindOrphanedAssignments: empty inputs - empty result");
    std::vector<std::pair<std::wstring, std::wstring>> emptyRegistry;
    std::vector<std::wstring> emptyRunning;
    auto emptyPathExists = [](const std::wstring&) { return false; };

    std::vector<cd::OrphanedAssignment> emptyResult = cd::FindOrphanedAssignments(emptyRegistry, emptyRunning, emptyPathExists);
    CHECK_EQ(emptyResult.size(), (size_t)0);
}

void Test_AI2_DriftStatusLine() {
    Case("AI2a FormatGpuIsolateStatusLine: (7,2,0) returns isolate sentence");
    std::wstring result = cd::FormatGpuIsolateStatusLine(7, 2, 0);
    CHECK(result.find(L"background") != std::wstring::npos);
    // v0.5.6: it names the picker, not "a second GPU" - the picker offers the main GPU too.
    CHECK_EQ(result, std::wstring(L"7 background apps can be moved to the GPU chosen above."));
    CHECK(result.find(L"lost") == std::wstring::npos);

    Case("AI2b FormatGpuIsolateStatusLine: (7,2,1) returns drift sentence, singular");
    result = cd::FormatGpuIsolateStatusLine(7, 2, 1);
    CHECK(result.find(L"1 app lost") != std::wstring::npos);
    CHECK(result.find(L"its") != std::wstring::npos);
    // THE FIX THAT WORKS, NOT "restart it". This case used to PIN the false instruction: the live
    // path has no registry entry, so a restart reapplies nothing. Caught in the window's screenshot.
    CHECK(result.find(L"tick it and Apply") != std::wstring::npos);
    CHECK(result.find(L"restart") == std::wstring::npos);
    CHECK(result.find(L"isolated") == std::wstring::npos);

    Case("AI2c FormatGpuIsolateStatusLine: (7,2,3) returns drift sentence, plural");
    result = cd::FormatGpuIsolateStatusLine(7, 2, 3);
    CHECK(result.find(L"3 apps lost") != std::wstring::npos);
    CHECK(result.find(L"their") != std::wstring::npos);
    CHECK(result.find(L"tick them and Apply") != std::wstring::npos);
    CHECK(result.find(L"restart") == std::wstring::npos);
    CHECK(result.find(L"isolated") == std::wstring::npos);

    Case("AI2d FormatGpuIsolateStatusLine: (0,1,5) returns empty because gpuCount < 2");
    result = cd::FormatGpuIsolateStatusLine(0, 1, 5);
    CHECK_EQ(result, std::wstring(L""));

    Case("AI2e FormatGpuIsolateStatusLine: drift outranks movable - (5,2,1) reports drift");
    result = cd::FormatGpuIsolateStatusLine(5, 2, 1);
    CHECK(result.find(L"lost") != std::wstring::npos);
    CHECK(result.find(L"5 background") == std::wstring::npos);
}

}  // namespace

// ===========================================================================
// ===========================================================================
// AK. Which GPU CUDA uses (v0.5.8)
//
// Every function below is reachable with no NVIDIA driver on the machine and no driver session open:
// the pure half lives in gpu_cuda.h and everything that touches NVIDIA is a function object this suite
// substitutes. The two id strings are the ones the operator's driver really enumerates [M] 2026-09-19.
//
// ROUND 3 IS A REDESIGN, NOT MORE GUARDS (D1-D10). Two review
// rounds kept finding new corners of one domain - the restore journal and the Remove path - so the
// journal and Remove were replaced rather than patched again. The tests that pin the new rules, one each,
// are AK17 (D1 and D3), AK19 (D2), AK18 (D4), AK6g (D5), AK14f (D6), AK8g (D7), AK10h (D8) and AK12d
// with AK7h (D9).
// ===========================================================================

namespace cudatest {

const wchar_t* kId5090 = L"id,2.0:2B8510DE,00000100,GF - (432,2,161,32607) @ (0)";
const wchar_t* kId4090 = L"id,2.0:268410DE,00000300,GF - (400,2,161,23028) @ (0)";
const wchar_t* kKey5090 = L"10DE&2B85&53021462";
const wchar_t* kKey4090 = L"10DE&2684&40BF1458";
const wchar_t* kKeyAmd = L"1002&164E&164E1002";

std::vector<cd::NvidiaGpu> TwoCards() {
    std::vector<cd::NvidiaGpu> gpus;
    cd::NvidiaGpu a;
    a.adapterKey = kKey5090;
    a.busId = 1;
    gpus.push_back(a);
    cd::NvidiaGpu b;
    b.adapterKey = kKey4090;
    b.busId = 3;
    gpus.push_back(b);
    return gpus;
}

std::vector<std::wstring> BothIds() {
    std::vector<std::wstring> ids;
    ids.push_back(L"autoselect");
    ids.push_back(kId4090);
    ids.push_back(kId5090);
    return ids;
}

// A whole NVIDIA driver in memory: profiles, their CUDA setting, and every refusal a real one can give.
//
// 🔴 THE SESSION AND THE DATABASE ARE SEPARATE MAPS, and that is the point of this fake rather than a
// detail of it. NvAPI_DRS_SaveSettings does not save one setting - it writes the WHOLE open session. A
// fake that committed every write on its own could never show the failure the Council found: a row whose
// own save failed is still sitting in the session, and the NEXT row's save puts it in the database with
// no record naming it.
struct Fake {
    bool available = true;
    cd::CudaRefusal openRefusal = cd::CudaRefusal::NoNvidiaDriver;
    std::vector<cd::NvidiaGpu> gpus;
    std::vector<std::wstring> ids;
    std::map<std::wstring, cd::CudaProfile> byExe;     // lower-case exe path -> the profile that governs it
    std::set<std::wstring> profiles;                   // every profile name the database holds, empty ones too
    std::map<std::wstring, std::wstring> settings;     // THE SESSION: profile name -> 0x10354FF8
    // 🔴 R5-1: SETTINGS ON A PROFILE THAT ARE NOT OURS - what a user sets in NVIDIA Control Panel, which
    // NVIDIA keeps in the SAME profile as our CUDA setting. A count is enough: the rule only asks whether
    // there are any, and deleting the profile would take all of them with it.
    std::map<std::wstring, size_t> otherSettings;      // profile name -> settings this product did not write
    // 🔴 R6-1: PROFILES WHOSE CUDA VALUE IS READ BUT IS NOT THAT PROFILE'S OWN. NvAPI_DRS_GetSetting
    // answers from NVIDIA's general settings when the profile itself has no such setting, so a value
    // coming back is not proof of where it lives. A profile in here reads exactly as any other and
    // enumerates WITHOUT 0x10354FF8 - which is the contradiction RestoreCudaForRow must refuse.
    std::set<std::wstring> inherited;
    bool settingsListFails = false;    // R6-1: NvAPI_DRS_EnumSettings refuses
    bool noEnumSettings = false;       // R6-2: this driver does not offer it at all, so the op is EMPTY
    // 🔴 R6-11: the adopt re-check inside createProfileForExe loses a race - the plan adopted the entry,
    // and a member arrived between the lookup and the create call.
    bool adoptRaceLost = false;
    size_t adoptRaceOtherApps = 1;     // what that second guard MEASURED
    bool adoptRaceIncomplete = false;  // ... or could not measure
    std::map<std::wstring, std::wstring> saved;        // THE DATABASE: only what a save really committed
    std::vector<std::wstring> written;                 // "<profile>=<value>" or "<profile>=CLEAR", in order
    // What CreateApplication answers. 🔴 IT IS ASKED AFTER THE PROFILE IS MADE, WHICH IS WHERE THE REAL -167
    // ARRIVES - so a test that sets it also exercises E7's clean-up of the profile made moments before.
    cd::CudaCreate createAnswer = cd::CudaCreate::Created;
    bool writeFails = false;
    bool writeIgnored = false;   // the driver answers yes and changes nothing, which a read-back catches
    int failWriteNumber = 0;     // or only the Nth write refuses, counting from 1
    bool lookupFails = false;    // every lookup answers Failed, as a refused FindApplicationByName does
    bool nameLookupFails = false;   // R5-2: FindProfileByName answers Failed, so nothing is adopted or made
    bool nameAppsIncomplete = false;   // R5-2: a by-name membership the driver would not finish listing
    bool readFails = false;      // every read answers Failed, as a refused GetSetting does
    bool saveFails = false;
    int failSaveNumber = 0;      // or only the Nth save refuses, counting from 1
    bool noDelete = false;       // a driver that offers no way to remove a profile
    bool deleteFails = false;    // it offers one and refuses
    int saves = 0;
    int creates = 0;
    int deletes = 0;
    int writes = 0;
};

// The driver already keeps `profileName` for `exe`, under the application entry `entry`.
void Own(Fake& f, const std::wstring& exe, const std::wstring& profileName, const std::wstring& entry,
         bool predefined = false, size_t otherApps = 0) {
    cd::CudaProfile p;
    p.profileName = profileName;
    p.appEntry = entry;
    p.isPredefined = predefined;
    p.otherApps = otherApps;
    f.byExe[cd::ToLower(exe)] = p;
    f.profiles.insert(profileName);
}

// An entry of OURS that an earlier run left behind: the profile is in the database, but it covers no
// application, so a lookup for the executable still answers Absent.
void LeftBehind(Fake& f, const std::wstring& exe) { f.profiles.insert(cd::CudaProfileNameFor(exe)); }

// NVDRS_PROFILE::numOfSettings, as the driver would report it: our own CUDA setting when there is one, plus
// every setting somebody else put on the same profile (R5-1).
size_t SettingsOn(const Fake& f, const std::wstring& profileName) {
    size_t n = f.settings.count(profileName) != 0 ? 1u : 0u;
    const std::map<std::wstring, size_t>::const_iterator it = f.otherSettings.find(profileName);
    if (it != f.otherSettings.end()) n += it->second;
    return n;
}

// 🔴 R6-1: WHAT NvAPI_DRS_EnumSettings WOULD LIST - the profile's OWN setting ids, and nothing it merely
// inherits. This is the answer the delete decision is taken on, and the whole point of round 6 is that it
// is not the same thing as the count above: a profile can hold ONE setting that is not ours at all.
std::vector<unsigned long> SettingIdsOn(const Fake& f, const std::wstring& profileName) {
    std::vector<unsigned long> ids;
    if (f.settings.count(profileName) != 0 && f.inherited.count(profileName) == 0)
        ids.push_back(cd::CudaSettingId());
    const std::map<std::wstring, size_t>::const_iterator it = f.otherSettings.find(profileName);
    const size_t n = it != f.otherSettings.end() ? it->second : 0;
    // Real NVIDIA setting ids that are NOT ours: OGL_IMPLICIT_GPU_AFFINITY_ID and its neighbours.
    for (size_t k = 0; k < n; ++k) ids.push_back(0x20D0F3E6ul + static_cast<unsigned long>(k));
    return ids;
}

cd::CudaOps OpsOf(Fake& f) {
    cd::CudaOps ops;
    ops.available = f.available;
    ops.openRefusal = f.openRefusal;
    ops.listGpus = [&f]() { return f.gpus; };
    ops.listIds = [&f]() { return f.ids; };
    ops.findProfileForExe = [&f](const std::wstring& exePath, cd::CudaProfile& profile) {
        profile = cd::CudaProfile();
        if (f.lookupFails) return cd::CudaLookup::Failed;
        const std::map<std::wstring, cd::CudaProfile>::const_iterator it = f.byExe.find(cd::ToLower(exePath));
        if (it == f.byExe.end()) return cd::CudaLookup::Absent;
        profile = it->second;
        profile.numSettings = SettingsOn(f, profile.profileName);   // R5-1, as GetProfileInfo reports it
        return cd::CudaLookup::Found;
    };
    // 🔴 R5-2: what the driver says about a profile asked for BY NAME. The live one is FindProfileByName +
    // GetProfileInfo + EnumApplications; this one answers the same three states from the maps above.
    ops.findProfileByName = [&f](const std::wstring& profileName, cd::CudaProfile& profile) {
        profile = cd::CudaProfile();
        if (f.nameLookupFails || profileName.empty()) return cd::CudaLookup::Failed;
        if (f.profiles.count(profileName) == 0) return cd::CudaLookup::Absent;
        profile.profileName = profileName;
        size_t apps = 0;
        for (std::map<std::wstring, cd::CudaProfile>::const_iterator it = f.byExe.begin();
             it != f.byExe.end(); ++it) {
            if (!cd::IEquals(it->second.profileName, profileName)) continue;
            apps += 1 + it->second.otherApps;
            if (it->second.isPredefined) profile.isPredefined = true;
        }
        profile.otherApps = apps;                 // nothing is excluded: none of them can be ours
        profile.appsComplete = !f.nameAppsIncomplete;
        profile.numSettings = SettingsOn(f, profileName);
        return cd::CudaLookup::Found;
    };
    ops.readSetting = [&f](const std::wstring& profile, std::wstring& value) {
        value.clear();
        if (f.readFails) return cd::CudaRead::Failed;
        const std::map<std::wstring, std::wstring>::const_iterator it = f.settings.find(profile);
        if (it == f.settings.end()) return cd::CudaRead::Absent;
        value = it->second;
        return cd::CudaRead::Value;
    };
    ops.writeSetting = [&f](const std::wstring& profile, bool clear, const std::wstring& value) {
        ++f.writes;
        if (f.writeFails || f.writes == f.failWriteNumber) return false;
        f.written.push_back(profile + L"=" + (clear ? std::wstring(L"CLEAR") : value));
        if (f.writeIgnored) return true;
        if (clear) f.settings.erase(profile);
        else f.settings[profile] = value;
        return true;
    };
    // The live one looks the name up, makes the profile only when there is none, refuses to touch one it
    // was not told it may adopt (E1), and takes a profile it just made away again when the application
    // will not go into it (E7). This one answers the same way from the maps above.
    // 🔴 R6-12: `adoptName` is the entry the caller ruled on, EMPTY when there is none. The live one opens
    // exactly that name, because [M] probe S23 measured NvAPI_DRS_FindProfileByName to be CASE-SENSITIVE -
    // and `f.profiles` is a std::set<std::wstring>, so the lookup below has case in exactly the same way.
    ops.createProfileForExe = [&f](const std::wstring& exePath, const std::wstring& adoptName,
                                   cd::CudaProfile& made) {
        ++f.creates;
        const bool reuseNamed = !adoptName.empty();
        const std::wstring name = reuseNamed ? adoptName : cd::CudaProfileNameFor(exePath);
        const bool exists = f.profiles.count(name) != 0;
        if (exists && !reuseNamed) return cd::CudaCreate::NameTaken;
        bool created = false;
        if (!exists) {
            f.profiles.insert(name);
            created = true;
        }
        // 🔴 `made` IS FILLED EVEN WHEN THE ANSWER IS NOT Created, exactly as the live one fills it: a
        // profile this call made is a change to the driver whatever the application call answered, and
        // taking it away again is the CALLER's decision (E7).
        made = cd::CudaProfile();
        made.profileName = name;
        made.appEntry = cd::CudaAppKeyFor(exePath);
        made.createdNow = created;
        // 🔴 R6-11: THE SECOND GUARD, TAKEN AGAINST THE HANDLE ACTUALLY ABOUT TO BE WRITTEN. The live one
        // re-reads GetProfileInfo here and answers NameTaken when a member arrived since the plan adopted
        // the entry, filling `made` with what it measured.
        if (reuseNamed && f.adoptRaceLost) {
            made.otherApps = f.adoptRaceIncomplete ? 0 : f.adoptRaceOtherApps;
            made.appsComplete = !f.adoptRaceIncomplete;
            return cd::CudaCreate::NameTaken;
        }
        if (f.createAnswer != cd::CudaCreate::Created) return f.createAnswer;
        f.byExe[cd::ToLower(exePath)] = made;
        return cd::CudaCreate::Created;
    };
    // The live one re-reads the profile and refuses unless all four guards hold (gpu_cuda.cpp,
    // DeleteOwnProfile). This one answers the same way from the maps above, so a test that asks it to
    // delete somebody else's profile - or one whose single application is not the entry it was given -
    // gets the same no.
    // 🔴 R6-2: IT IS INSTALLED UNCONDITIONALLY, EXACTLY AS MakeCudaOps INSTALLS ITS OWN - and
    // ApplyCudaDriverCalls at the end of this function is what takes it away again when the driver has no
    // delete entry points. That wiring is the round-5 defect both seats found, so this suite has to run
    // the real gate rather than an `if (!f.noDelete)` of its own that could stay right while the product's
    // went wrong.
    {
        ops.deleteProfile = [&f](const std::wstring& profileName, const std::wstring& appEntry) {
            ++f.deletes;
            if (f.deleteFails) return false;
            if (!cd::IsCudaProfileWeMade(profileName) || appEntry.empty()) return false;
            if (f.profiles.count(profileName) == 0) return false;
            size_t apps = 0, others = 0;
            bool predefined = false;
            for (std::map<std::wstring, cd::CudaProfile>::const_iterator it = f.byExe.begin();
                 it != f.byExe.end(); ++it) {
                if (it->second.profileName != profileName) continue;
                ++apps;
                others += it->second.otherApps;
                if (!cd::IEquals(it->second.appEntry, appEntry)) ++others;
                if (it->second.isPredefined) predefined = true;
            }
            if (predefined || apps > 1 || others > 0) return false;
            // 🔴 R5-1 / R6-1, GUARD 5, exactly as gpu_cuda.cpp's DeleteOwnProfile now refuses it: it
            // enumerates the entry's own setting IDS and deletes nothing that holds a setting this product
            // did not write. A count is not an identity, and an enumeration nobody could make refuses.
            if (f.settingsListFails || f.noEnumSettings) return false;
            const std::vector<unsigned long> ids = SettingIdsOn(f, profileName);
            for (size_t i = 0; i < ids.size(); ++i)
                if (ids[i] != cd::CudaSettingId()) return false;
            for (std::map<std::wstring, cd::CudaProfile>::iterator it = f.byExe.begin(); it != f.byExe.end();) {
                if (it->second.profileName == profileName) f.byExe.erase(it++);
                else ++it;
            }
            f.settings.erase(profileName);
            f.profiles.erase(profileName);
            return true;
        };
    }
    // 🔴 R6-1 / R6-2: THE SETTING IDS, AND THE OPERATION IS ABSENT ON A DRIVER THAT CANNOT LIST THEM. It
    // is installed here and taken away again by ApplyCudaDriverCalls below, exactly as MakeCudaOps does
    // it, so the capability gate this suite pins is the one the product really runs.
    ops.listSettingIds = [&f](const std::wstring& profileName, std::vector<unsigned long>& ids) {
        ids.clear();
        if (f.settingsListFails) return false;
        ids = SettingIdsOn(f, profileName);
        return true;
    };
    ops.save = [&f]() {
        ++f.saves;
        if (f.saveFails || f.saves == f.failSaveNumber) return false;
        f.saved = f.settings;   // the whole session, which is what the driver's own save does
        return true;
    };
    // 🔴 R6-2: THE SAME ONE GATE THE .cpp USES. `noDelete` and `noEnumSettings` are entry points that did
    // not resolve, so they must reach the operations through ApplyCudaDriverCalls and not through an `if`
    // of this file's own - otherwise this suite would be pinning a rule the product does not run.
    cd::CudaDriverCalls calls;
    calls.deleteProfile = !f.noDelete;
    calls.deleteApplication = !f.noDelete;
    calls.enumSettings = !f.noEnumSettings;
    cd::ApplyCudaDriverCalls(ops, calls);
    return ops;
}

// The record on disk, without the disk: SaveCudaRecordRow's own rules (gpu_cuda.cpp) over a CudaRecord.
struct Rec {
    cd::CudaRecord record;
    bool fails = false;   // every write to the record refuses
    int saves = 0;
    Rec() {
        record.state = cd::CudaRecordState::Ok;
        record.path = L"C:\\d\\gpu-cuda-record.txt";
    }
};

cd::CudaApplyInputs InputsOf(Rec& r) {
    cd::CudaApplyInputs in;
    in.record = [&r](const cd::CudaRecordRow& row) {
        ++r.saves;
        if (r.fails) return false;
        cd::CudaRecordRow line = row;
        if (line.when.empty()) line.when = L"2026-09-20 12:00:00";
        if (!cd::CudaRecordRowIsWritable(line)) return false;
        r.record.rows = cd::CudaRowsWith(r.record.rows, line);
        return true;
    };
    return in;
}

cd::CudaRowResult Apply(const std::wstring& exe, const cd::CudaTarget& t, const cd::CudaOps& ops, Rec& rec) {
    return cd::WriteCudaForRow(exe, t, ops, rec.record, InputsOf(rec));
}

// One line of the record, as ReadCudaRecord would have parsed it.
cd::CudaRecordRow Line(const std::wstring& profile, const std::wstring& entry, const std::wstring& lastWrote) {
    cd::CudaRecordRow row;
    row.profileName = profile;
    row.appEntry = entry;
    row.lastWrote = lastWrote;
    row.when = L"2026-09-20 12:00:00";
    return row;
}

}  // namespace cudatest

void Test_AK1_ParseUniversalGpuId() {
    Case("AK1 the two joinable fields of a driver id string, and everything that is not one");
    const cd::UniversalGpuId a = cd::ParseUniversalGpuId(cudatest::kId5090);
    CHECK(a.ok);
    CHECK_EQ((unsigned int)a.deviceId, 0x2B8510DEu);
    CHECK_EQ((unsigned int)a.busId, 1u);                       // 0x00000100 >> 8
    const cd::UniversalGpuId b = cd::ParseUniversalGpuId(cudatest::kId4090);
    CHECK(b.ok);
    CHECK_EQ((unsigned int)b.deviceId, 0x268410DEu);
    CHECK_EQ((unsigned int)b.busId, 3u);                       // 0x00000300 >> 8
    // The driver's own list holds these beside the real ids, and neither is a GPU.
    CHECK(!cd::ParseUniversalGpuId(L"autoselect").ok);
    CHECK(!cd::ParseUniversalGpuId(L"none").ok);
    CHECK(!cd::ParseUniversalGpuId(L"").ok);
    CHECK(!cd::ParseUniversalGpuId(L"id,2.0:2B8510DE").ok);            // one field, no comma after it
    CHECK(!cd::ParseUniversalGpuId(L"id,2.0:2B8510DE,00000100").ok);   // the second comma is missing
    CHECK(!cd::ParseUniversalGpuId(L"id,2.0:ZZZZ,00000100,GF").ok);    // not hexadecimal
    CHECK(!cd::ParseUniversalGpuId(L"id,2.0:,00000100,GF").ok);        // empty field
    CHECK(!cd::ParseUniversalGpuId(L"id,2.0:2B8510DEE,00000100,GF").ok);   // nine digits is not a 32-bit id
}

void Test_AK2_TheKeyCarriesTheDeviceId() {
    Case("AK2 an adapter key is the device id NVAPI reports, written the other way round");
    unsigned long id = 0;
    CHECK(cd::DeviceIdFromAdapterKey(cudatest::kKey5090, id));
    CHECK_EQ((unsigned int)id, 0x2B8510DEu);
    CHECK(cd::DeviceIdFromAdapterKey(cudatest::kKey4090, id));
    CHECK_EQ((unsigned int)id, 0x268410DEu);
    CHECK(cd::IsNvidiaAdapterKey(cudatest::kKey5090));
    CHECK(cd::IsNvidiaAdapterKey(cudatest::kKey4090));
    CHECK(!cd::IsNvidiaAdapterKey(cudatest::kKeyAmd));
    // Not adapter keys at all, and none of them may be read as one.
    CHECK(!cd::IsNvidiaAdapterKey(L""));
    CHECK(!cd::IsNvidiaAdapterKey(cd::WindowsPowerSavingKey()));
    CHECK(!cd::IsNvidiaAdapterKey(cd::WindowsHighPerformanceKey()));
    CHECK(!cd::IsNvidiaAdapterKey(cd::UnreadableChoiceKey()));
    CHECK(!cd::DeviceIdFromAdapterKey(L"10DE-2B85-53021462", id));
    CHECK(!cd::DeviceIdFromAdapterKey(L"GGGG&2B85&53021462", id));
}

void Test_AK3_MatchUniversalId() {
    Case("AK3 a card is joined to its id string by device id AND bus id, or not at all");
    const std::vector<cd::NvidiaGpu> gpus = cudatest::TwoCards();
    const std::vector<std::wstring> ids = cudatest::BothIds();

    const cd::CudaMatch hit = cd::MatchUniversalId(cudatest::kKey5090, gpus, ids);
    CHECK(hit.refusal == cd::CudaRefusal::None);
    CHECK_EQ(hit.id, std::wstring(cudatest::kId5090));   // VERBATIM: the tail is never rebuilt
    CHECK_EQ(cd::MatchUniversalId(cudatest::kKey4090, gpus, ids).id, std::wstring(cudatest::kId4090));

    Case("AK3b every refusal MatchUniversalId can give");
    CHECK(cd::MatchUniversalId(cudatest::kKey5090, std::vector<cd::NvidiaGpu>(), ids).refusal ==
          cd::CudaRefusal::NoNvidiaDriver);
    CHECK(cd::MatchUniversalId(cudatest::kKeyAmd, gpus, ids).refusal == cd::CudaRefusal::NoNvidiaGpuForKey);
    {
        std::vector<cd::NvidiaGpu> twins = gpus;
        twins[1].adapterKey = cudatest::kKey5090;   // two physically identical cards
        CHECK(cd::MatchUniversalId(cudatest::kKey5090, twins, ids).refusal ==
              cd::CudaRefusal::AmbiguousIdenticalCards);
    }
    {
        std::vector<std::wstring> only4090;
        only4090.push_back(cudatest::kId4090);
        CHECK(cd::MatchUniversalId(cudatest::kKey5090, gpus, only4090).refusal == cd::CudaRefusal::NoIdForGpu);
        CHECK(cd::MatchUniversalId(cudatest::kKey5090, gpus, std::vector<std::wstring>()).refusal ==
              cd::CudaRefusal::NoIdForGpu);
    }
    {
        std::vector<std::wstring> twice = ids;
        twice.push_back(cudatest::kId5090);
        CHECK(cd::MatchUniversalId(cudatest::kKey5090, gpus, twice).refusal == cd::CudaRefusal::SeveralIdsForGpu);
    }
    Case("AK3c the bus id is part of the join, not decoration");
    {
        std::vector<cd::NvidiaGpu> moved = gpus;
        moved[0].busId = 9;   // same card id, another slot: the driver's string no longer names it
        CHECK(cd::MatchUniversalId(cudatest::kKey5090, moved, ids).refusal == cd::CudaRefusal::NoIdForGpu);
    }
}

void Test_AK4_CudaPlanFor() {
    Case("AK4 a non-NVIDIA target is silence, not a refusal (founder decision 18)");
    const std::vector<cd::NvidiaGpu> gpus = cudatest::TwoCards();
    const cd::CudaPlan amd = cd::CudaPlanFor(cudatest::kKeyAmd, gpus);
    CHECK(!amd.act);
    CHECK(amd.refusal == cd::CudaRefusal::None);
    CHECK(amd.excludeKeys.empty());
    for (int k = 0; k < 4; ++k) {
        const std::wstring key = k == 0 ? std::wstring()
                                        : k == 1 ? cd::WindowsPowerSavingKey()
                                                 : k == 2 ? cd::WindowsHighPerformanceKey()
                                                          : cd::UnreadableChoiceKey();
        const cd::CudaPlan p = cd::CudaPlanFor(key, gpus);
        CHECK(!p.act);
        CHECK(p.refusal == cd::CudaRefusal::None);
    }

    Case("AK4b an NVIDIA target excludes every other NVIDIA card, each once");
    const cd::CudaPlan two = cd::CudaPlanFor(cudatest::kKey4090, gpus);
    CHECK(two.act);
    CHECK_EQ(two.excludeKeys.size(), (size_t)1);
    CHECK_EQ(two.excludeKeys[0], std::wstring(cudatest::kKey5090));

    Case("AK4c one NVIDIA card: nothing is excluded, and that is still a plan");
    {
        std::vector<cd::NvidiaGpu> one;
        one.push_back(gpus[0]);
        const cd::CudaPlan p = cd::CudaPlanFor(cudatest::kKey5090, one);
        CHECK(p.act);
        CHECK(p.excludeKeys.empty());
    }

    Case("AK4d an NVIDIA target the driver cannot account for IS said");
    CHECK(cd::CudaPlanFor(cudatest::kKey5090, std::vector<cd::NvidiaGpu>()).refusal ==
          cd::CudaRefusal::NoNvidiaDriver);
    {
        std::vector<cd::NvidiaGpu> only4090;
        only4090.push_back(gpus[1]);
        CHECK(cd::CudaPlanFor(cudatest::kKey5090, only4090).refusal == cd::CudaRefusal::NoNvidiaGpuForKey);
    }

    Case("AK4e three NVIDIA cards refuse: the separator between two ids is unmeasured");
    {
        std::vector<cd::NvidiaGpu> three = gpus;
        cd::NvidiaGpu c;
        c.adapterKey = L"10DE&2204&38821043";
        c.busId = 5;
        three.push_back(c);
        const cd::CudaPlan p = cd::CudaPlanFor(cudatest::kKey5090, three);
        CHECK(!p.act);
        CHECK(p.refusal == cd::CudaRefusal::SeveralToExclude);
    }

    Case("AK4f two entries of the SAME other card count once, so a twin listing is not three cards");
    {
        std::vector<cd::NvidiaGpu> twice = gpus;
        twice.push_back(gpus[0]);
        const cd::CudaPlan p = cd::CudaPlanFor(cudatest::kKey4090, twice);
        CHECK(p.act);
        CHECK_EQ(p.excludeKeys.size(), (size_t)1);
    }
}

void Test_AK5_CudaTargetFor() {
    Case("AK5 the one value a target implies");
    const std::vector<cd::NvidiaGpu> gpus = cudatest::TwoCards();
    const std::vector<std::wstring> ids = cudatest::BothIds();
    const cd::CudaTarget t = cd::CudaTargetFor(cudatest::kKey4090, gpus, ids);
    CHECK(t.act);
    CHECK_EQ(t.value, std::wstring(cudatest::kId5090));   // exclude the OTHER card
    CHECK(t.refusal == cd::CudaRefusal::None);

    Case("AK5b the only NVIDIA card there is: CUDA may use everything, written as NVIDIA's own word");
    {
        std::vector<cd::NvidiaGpu> one;
        one.push_back(gpus[0]);
        const cd::CudaTarget solo = cd::CudaTargetFor(cudatest::kKey5090, one, ids);
        CHECK(solo.act);
        CHECK_EQ(solo.value, cd::CudaNoneValue());
    }

    Case("AK5c a refusal anywhere below leaves act false and the reason intact");
    CHECK(!cd::CudaTargetFor(cudatest::kKeyAmd, gpus, ids).act);
    CHECK(cd::CudaTargetFor(cudatest::kKeyAmd, gpus, ids).refusal == cd::CudaRefusal::None);
    CHECK(cd::CudaTargetFor(cudatest::kKey4090, gpus, std::vector<std::wstring>()).refusal ==
          cd::CudaRefusal::NoIdForGpu);
    CHECK(!cd::CudaTargetFor(cudatest::kKey4090, gpus, std::vector<std::wstring>()).act);
}

// 🔴 E1: THE PRODUCT WRITES CUDA ONLY WHERE IT OWNS THE PROFILE EXCLUSIVELY. Everything else is a refusal
// that names the entry and counts the programs it covers, and leaves the driver database untouched.
void Test_AK6_WriteCudaForRow() {
    Case("AK6 E1 first case: NVIDIA has no entry, so one of ours is made, written, recorded and saved");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    const cd::CudaOps ops = cudatest::OpsOf(f);
    const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
    cudatest::Rec rec;

    const cd::CudaRowResult made = cudatest::Apply(exe, target, ops, rec);
    CHECK(made.outcome == cd::CudaOutcome::Written);
    CHECK(made.recorded);
    CHECK(!made.hadPrevious);                                  // there was no entry at all
    CHECK_EQ(f.creates, 1);
    CHECK_EQ(f.saves, 1);
    CHECK_EQ(f.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId5090));
    // 🔴 E2: THE LINE IS THE WHOLE RECORD - entry, value, time. There is no original to keep, because
    // before this row NVIDIA had no entry for the application at all.
    CHECK_EQ(rec.record.rows.size(), (size_t)1);
    CHECK_EQ(rec.record.rows[0].profileName, cd::CudaProfileNameFor(exe));
    CHECK_EQ(rec.record.rows[0].appEntry, cd::CudaAppKeyFor(exe));
    CHECK_EQ(rec.record.rows[0].lastWrote, std::wstring(cudatest::kId5090));
    CHECK(!rec.record.rows[0].when.empty());

    Case("AK6b running it again changes nothing, writes nothing and records nothing");
    const int savesBefore = rec.saves;
    const cd::CudaRowResult again = cudatest::Apply(exe, target, ops, rec);
    CHECK(again.outcome == cd::CudaOutcome::AlreadySet);
    CHECK_EQ(f.saves, 1);
    CHECK_EQ(f.written.size(), (size_t)1);
    CHECK_EQ(rec.saves, savesBefore);                          // the line already says exactly this

    Case("AK6c a second Apply on our own entry moves the line's value, and there is still one line");
    const cd::CudaTarget other = cd::CudaTargetFor(cudatest::kKey5090, f.gpus, f.ids);
    const cd::CudaRowResult swapped = cudatest::Apply(exe, other, ops, rec);
    CHECK(swapped.outcome == cd::CudaOutcome::Written);
    CHECK(swapped.hadPrevious);
    CHECK_EQ(swapped.previousValue, std::wstring(cudatest::kId5090));
    CHECK_EQ(f.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId4090));
    CHECK_EQ(rec.record.rows.size(), (size_t)1);
    CHECK_EQ(rec.record.rows[0].lastWrote, std::wstring(cudatest::kId4090));
    CHECK_EQ(f.creates, 1);                                    // nothing was made a second time

    Case("AK6d 🔴 E1: NVIDIA'S OWN ENTRY IS NOT WRITTEN, and the refusal names it and counts its programs");
    {
        cudatest::Fake s;
        s.gpus = cudatest::TwoCards();
        s.ids = cudatest::BothIds();
        cudatest::Own(s, L"C:\\Chrome\\chrome.exe", L"Google Chrome", L"chrome.exe", true, 2);
        s.settings[L"Google Chrome"] = cudatest::kId4090;
        cudatest::Rec srec;
        const cd::CudaRowResult r = cudatest::Apply(
            L"C:\\Chrome\\chrome.exe", cd::CudaTargetFor(cudatest::kKey4090, s.gpus, s.ids),
            cudatest::OpsOf(s), srec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NvidiaManagesIt);
        CHECK_EQ(s.creates, 0);
        CHECK(s.written.empty());
        CHECK_EQ(s.saves, 0);
        CHECK(srec.record.rows.empty());
        CHECK_EQ(s.settings[L"Google Chrome"], std::wstring(cudatest::kId4090));   // theirs, untouched
        // The sentence E9 requires, built from what the row itself answered.
        CHECK_EQ(r.profileName, std::wstring(L"Google Chrome"));
        CHECK_EQ(r.otherApps, (size_t)2);
        const std::wstring says = cd::CudaRowRefusalText(r);
        CHECK(says.find(L"Google Chrome") != std::wstring::npos);
        CHECK(says.find(L"2 other programs") != std::wstring::npos);
        CHECK(says.find(L"NVIDIA Control Panel") != std::wstring::npos);
    }

    Case("AK6e every refusal a write can meet, and none of them writes anything");
    {
        cudatest::Fake n;
        n.available = false;
        n.openRefusal = cd::CudaRefusal::NoNvidiaDriver;
        cudatest::Rec nrec;
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NoNvidiaDriver);
        CHECK(n.written.empty());
    }
    {
        cudatest::Fake n;
        n.available = false;
        n.openRefusal = cd::CudaRefusal::SessionRefused;
        cudatest::Rec nrec;
        CHECK(cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec).refusal == cd::CudaRefusal::SessionRefused);
    }
    {
        // 🔴 E7: -167 ARRIVES AFTER THE PROFILE IS MADE, AND THE PROFILE GOES AGAIN. Without that an empty
        // "Game Optimizer - <path>" sits in the open session and the NEXT row's save commits it.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.createAnswer = cd::CudaCreate::AlreadyInUse;
        cudatest::Rec nrec;
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NameAlreadyInUse);
        CHECK(n.written.empty());
        CHECK_EQ(n.saves, 0);
        CHECK(nrec.record.rows.empty());
        CHECK(n.profiles.empty());                             // E7: nothing was left behind
        CHECK_EQ(n.deletes, 1);
    }
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.createAnswer = cd::CudaCreate::Failed;
        cudatest::Rec nrec;
        CHECK(cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec).refusal == cd::CudaRefusal::ProfileNotCreated);
        CHECK(n.profiles.empty());                             // E7 again: the same clean-up
    }
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.writeFails = true;
        cudatest::Rec nrec;
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::SettingNotWritten);
        CHECK_EQ(n.saves, 0);
        CHECK_EQ(nrec.saves, 0);
        CHECK(r.profileDeleted);                               // and the entry it made went with it
    }
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.saveFails = true;
        cudatest::Rec nrec;
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
    }

    Case("AK6f a target that does nothing asks the driver nothing at all");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Rec nrec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKeyAmd, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::NotAsked);
        CHECK(r.refusal == cd::CudaRefusal::None);
        CHECK(n.written.empty());
        CHECK_EQ(n.creates, 0);
    }

    Case("AK6g the line goes down BEFORE the save, and a record that refuses takes the change back out");
    {
        // 🔴 The old order was write, save, then record - so a record that could not take the row left a
        // change on disk that Remove assignment could not even see. The line goes first now: a row whose
        // line cannot be written is undone in the driver instead of being left there unrecorded.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Rec nrec;
        nrec.fails = true;
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotRecorded);
        CHECK(!r.recorded);
        CHECK(r.undone);
        CHECK_EQ(nrec.saves, 1);                                 // the record WAS asked, and refused
        CHECK_EQ(n.saves, 0);                                    // 🔴 and nothing was ever saved
        CHECK(n.saved.empty());
        CHECK_EQ(n.settings.count(cd::CudaProfileNameFor(exe)), (size_t)0);
        CHECK_EQ(n.byExe.count(cd::ToLower(exe)), (size_t)0);    // the entry it made went with it
        CHECK(n.profiles.empty());
        CHECK(nrec.record.rows.empty());
    }

    Case("AK6h 🔴 an entry of ours that already holds the target still has its LINE brought up to date");
    {
        // Apply to card A. The user then sets exactly the value a second Apply would write, in NVIDIA's own
        // Control Panel. Without this the line still claims card A's value, and the next Remove compares
        // the driver with a value nobody holds and reports a conflict this product itself caused.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        const cd::CudaOps nops = cudatest::OpsOf(n);
        cudatest::Rec nrec;
        cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), nops, nrec);
        CHECK_EQ(nrec.record.rows[0].lastWrote, std::wstring(cudatest::kId5090));
        n.settings[cd::CudaProfileNameFor(exe)] = cudatest::kId4090;   // the user, in NVIDIA's own panel
        const int writesBefore = n.writes;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey5090, n.gpus, n.ids), nops, nrec);
        CHECK(r.outcome == cd::CudaOutcome::AlreadySet);
        CHECK(r.recorded);
        CHECK_EQ(n.writes, writesBefore);                        // nothing was written to the driver
        CHECK_EQ(nrec.record.rows.size(), (size_t)1);
        CHECK_EQ(nrec.record.rows[0].lastWrote, std::wstring(cudatest::kId4090));
        // And the restore that follows it finds no conflict at all.
        CHECK(cd::RestoreCudaForRow(exe, *cd::CudaLineFor(nrec.record, exe), nops).outcome ==
              cd::CudaOutcome::Written);
    }
}

// 🔴 E3: THE UNDO IS THE ENTRY, NOT THE VALUE. Before this product there was no entry at all, so putting
// it back means taking the whole entry away - and only while it is still ours and still holds what we left.
void Test_AK7_RestoreCudaForRow() {
    Case("AK7 E3 first case: our own entry, still ours, still holding our value - it goes, and is confirmed");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const cd::CudaRecordRow line = cudatest::Line(ours, entry, cudatest::kId5090);
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.profileDeleted);
        CHECK_EQ(f.deletes, 1);
        CHECK_EQ(f.saves, 1);
        CHECK(f.profiles.empty());
        CHECK(f.settings.empty());
        CHECK_EQ(f.byExe.count(cd::ToLower(exe)), (size_t)0);
    }

    Case("AK7b E3 second case: a value somebody else set since is a CONFLICT - nothing is written");
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cd::CudaNoneValue();   // chosen in NVIDIA's own panel since
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::ChangedSinceWritten);
        CHECK_EQ(n.deletes, 0);
        CHECK_EQ(n.saves, 0);
        CHECK_EQ(n.settings[ours], cd::CudaNoneValue());
    }
    {
        cudatest::Fake n;   // the setting is gone entirely: also not ours to take an entry away over
        cudatest::Own(n, exe, ours, entry);
        CHECK(cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::ChangedSinceWritten);
    }
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        n.readFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);   // a failed read is never "it must still be ours"
        CHECK_EQ(n.deletes, 0);
    }

    Case("AK7c E3 third case: the entry is already gone, which is ABSENT and is not a failure");
    {
        cudatest::Fake n;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.outcome == cd::CudaOutcome::Absent);
        CHECK(r.refusal == cd::CudaRefusal::None);
        CHECK(n.written.empty());
        CHECK_EQ(n.saves, 0);
        CHECK_EQ(n.deletes, 0);
    }

    Case("AK7d no driver, a damaged note and a lookup nobody could make are each said, and touch nothing");
    {
        cudatest::Fake n;
        n.available = false;
        n.openRefusal = cd::CudaRefusal::SessionRefused;
        CHECK(cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::SessionRefused);
    }
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        cd::CudaRecordRow bad = line;
        bad.lastWrote = L"not a gpu id at all";
        CHECK(cd::RestoreCudaForRow(exe, bad, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::RecordUnusable);
        bad = line;
        bad.lastWrote.clear();                   // nothing to compare the driver with
        CHECK(cd::RestoreCudaForRow(exe, bad, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::RecordUnusable);
        bad = line;
        bad.profileName.clear();
        CHECK(cd::RestoreCudaForRow(exe, bad, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::RecordUnusable);
        bad = line;
        bad.appEntry.clear();
        CHECK(cd::RestoreCudaForRow(exe, bad, cudatest::OpsOf(n)).refusal == cd::CudaRefusal::RecordUnusable);
        CHECK(n.written.empty());
        CHECK_EQ(n.deletes, 0);
        // NVIDIA's own word for "nothing is excluded" IS a legal value and must not be read as damage.
        cudatest::Fake ok;
        cudatest::Own(ok, exe, ours, entry);
        ok.settings[ours] = cd::CudaNoneValue();
        cd::CudaRecordRow fine = line;
        fine.lastWrote = cd::CudaNoneValue();
        CHECK(cd::RestoreCudaForRow(exe, fine, cudatest::OpsOf(ok)).outcome == cd::CudaOutcome::Written);
    }
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        n.lookupFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::CouldNotLookUp);
        CHECK_EQ(n.deletes, 0);
    }

    Case("AK7e an entry that could not be taken away leaves everything as it is, and says which");
    {
        // A driver with no way to delete a profile at all: nothing was touched, so a later row may still
        // save and the run does not have to stop.
        cudatest::Fake n;
        n.noDelete = true;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::ProfileNotRemoved);
        CHECK(!r.sessionDirty);
        CHECK_EQ(n.settings[ours], std::wstring(cudatest::kId5090));
    }
    {
        // It offers one and refuses. That may have taken the application entry out of the open session on
        // its way to refusing, so no later row of this run may save.
        cudatest::Fake n;
        n.deleteFails = true;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::ProfileNotRemoved);
        CHECK(r.sessionDirty);
        CHECK_EQ(n.saves, 0);
    }
    {
        // The delete landed in the SESSION and the save refused, so nothing reached the database - the line
        // stays and the run stops rather than letting a later row's save commit it.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        n.saveFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.sessionDirty);
        CHECK(!r.profileDeleted);
        CHECK(n.saved.empty());
    }

    Case("AK7f a delete the driver claims and does not do is caught by the read-back");
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.settings[ours] = cudatest::kId5090;
        cd::CudaOps ignoring = cudatest::OpsOf(n);
        // The driver answers yes and leaves the entry where it is: the second lookup still finds ours.
        ignoring.deleteProfile = [](const std::wstring&, const std::wstring&) { return true; };
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, ignoring);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotConfirmed);
        CHECK(!r.profileDeleted);
    }
}

void Test_AK8_TheRecordOnDisk() {
    Case("AK8 E2: a record round-trips, and one line is four fields and nothing more");
    std::vector<cd::CudaRecordRow> rows;
    const cd::CudaRecordRow a = cudatest::Line(L"Game Optimizer - C:\\Chrome\\chrome.exe",
                                               L"c:/chrome/chrome.exe", cudatest::kId4090);
    rows.push_back(a);
    cd::CudaRecordRow b = cudatest::Line(L"Game Optimizer - C:\\Apps\\chat\\chat.exe",
                                         L"c:/apps/chat/chat.exe", cd::CudaNoneValue());
    b.when = L"2026-09-20 12:00:01";
    rows.push_back(b);

    const std::wstring text = cd::FormatCudaRecordFile(rows);
    CHECK(text.find(cd::FormatCudaRecordHeader()) == 0);
    CHECK(text.find(cd::CudaRecordFileName()) != std::wstring::npos);   // the header names the file
    CHECK(text.find(L"\r\nGame Optimizer - C:\\Chrome\\chrome.exe\tc:/chrome/chrome.exe\t") != std::wstring::npos);
    std::vector<cd::CudaRecordRow> back;
    CHECK(cd::ParseCudaRecordFile(text, back));
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0].profileName, a.profileName);
    CHECK_EQ(back[0].appEntry, a.appEntry);
    CHECK_EQ(back[0].lastWrote, a.lastWrote);     // the field Remove compares the driver with
    CHECK_EQ(back[0].when, a.when);
    CHECK_EQ(back[1].lastWrote, cd::CudaNoneValue());

    Case("AK8b 🔴 E6: THE FIRST LINE IS A HEADER, and a file without one is UNREADABLE - never empty");
    {
        std::vector<cd::CudaRecordRow> out;
        const std::wstring magic = cd::CudaRecordMagic() + L"\t" + cd::CudaRecordVersion() + L"\r\n";
        // a header and no rows at all: a real, readable, empty record - the state a spent last line leaves
        CHECK(cd::ParseCudaRecordFile(magic, out));
        CHECK(out.empty());
        CHECK(cd::ParseCudaRecordFile(magic + L"; a comment\r\n\r\nP\tA\tnone\tt\r\n", out));
        CHECK_EQ(out.size(), (size_t)1);
        CHECK_EQ(out[0].profileName, std::wstring(L"P"));
        CHECK_EQ(out[0].appEntry, std::wstring(L"A"));
        CHECK_EQ(out[0].lastWrote, cd::CudaNoneValue());
        CHECK_EQ(out[0].when, std::wstring(L"t"));
        // 🔴 AND EVERY WAY A FILE CAN FAIL TO BE ONE OF OURS ANSWERS false, not "there was nothing".
        CHECK(!cd::ParseCudaRecordFile(std::wstring(), out));                  // empty
        CHECK(!cd::ParseCudaRecordFile(L"\r\n\r\n", out));                     // blank lines only
        CHECK(!cd::ParseCudaRecordFile(L"; only a comment\r\n", out));         // comment-only
        CHECK(!cd::ParseCudaRecordFile(L"; a comment\r\n" + magic, out));      // the header is not first
        CHECK(!cd::ParseCudaRecordFile(cd::CudaRecordMagic() + L"\t1\r\n", out));   // an older version
        CHECK(!cd::ParseCudaRecordFile(cd::CudaRecordMagic() + L"\t3\r\n", out));   // and a newer one
        CHECK(!cd::ParseCudaRecordFile(L"Some Other Product\t2\r\n", out));
        // the v0.5.8 round-1 and round-2 formats, which this version must not guess at
        CHECK(!cd::ParseCudaRecordFile(L"Google Chrome\tchrome.exe\t(none)\tnone\r\n", out));
        CHECK(!cd::ParseCudaRecordFile(L"1\tP\tA\t0\tnone\tnone\tt\r\n", out));
        // one field too many, one too few, and a row missing what the compare needs
        CHECK(!cd::ParseCudaRecordFile(magic + L"P\tA\tnone\tt\textra\r\n", out));
        CHECK(!cd::ParseCudaRecordFile(magic + L"P\tA\tnone\r\n", out));
        CHECK(!cd::ParseCudaRecordFile(magic + L"\tA\tnone\tt\r\n", out));
        CHECK(!cd::ParseCudaRecordFile(magic + L"P\tA\t\tt\r\n", out));
        // one bad line poisons the whole file, even beside good ones
        CHECK(!cd::ParseCudaRecordFile(magic + L"P\tA\tnone\tt\r\nrubbish\r\n", out));
        CHECK(out.empty());
    }

    Case("AK8c a field with a tab or a line break is never written, nor a row with nothing to compare");
    {
        cd::CudaRecordRow bad = a;
        CHECK(cd::CudaRecordRowIsWritable(bad));
        bad.profileName = L"has\there";
        CHECK(!cd::CudaRecordRowIsWritable(bad));
        bad = a;
        bad.appEntry = L"has\r\nhere";
        CHECK(!cd::CudaRecordRowIsWritable(bad));
        bad = a;
        bad.lastWrote = L"id\t2";
        CHECK(!cd::CudaRecordRowIsWritable(bad));
        bad = a;
        bad.lastWrote.clear();
        CHECK(!cd::CudaRecordRowIsWritable(bad));
        bad = a;
        bad.appEntry.clear();
        CHECK(!cd::CudaRecordRowIsWritable(bad));
        bad = a;
        bad.when.clear();                       // a line with no time is not one this writes
        CHECK(!cd::CudaRecordRowIsWritable(bad));
    }

    Case("AK8d only a value the driver itself could have written is usable");
    {
        CHECK(cd::CudaValueIsLegal(cd::CudaNoneValue()));
        CHECK(cd::CudaValueIsLegal(cudatest::kId5090));
        CHECK(!cd::CudaValueIsLegal(L""));
        CHECK(!cd::CudaValueIsLegal(L"(none)"));
        CHECK(!cd::CudaValueIsLegal(L"autoselect"));
        CHECK(!cd::CudaValueIsLegal(L"id,2.0:ZZZZ,00000100,GF"));
        CHECK(cd::CudaRecordRowIsUsable(a));
        CHECK(cd::CudaRecordRowIsUsable(b));
    }

    Case("AK8e the one file has a plain name and lives beside config.ini, not beside the .reg");
    CHECK_EQ(cd::CudaRecordFileName(), std::wstring(L"gpu-cuda-record.txt"));
    CHECK(cd::CudaRecordFileName().find(L"before") == std::wstring::npos);
    CHECK_EQ(cd::CudaRecordVersion(), std::wstring(L"2"));

    Case("AK8f our own application entry has ONE form, and a bare file name is never it");
    CHECK_EQ(cd::CudaAppKeyFor(L"C:\\Apps\\Chat\\chat.exe"), std::wstring(L"c:/apps/chat/chat.exe"));
    CHECK_EQ(cd::CudaAppKeyFor(L"c:/apps/chat/chat.exe"), std::wstring(L"c:/apps/chat/chat.exe"));
    CHECK(cd::CudaAppKeyFor(L"C:\\Apps\\Chat\\chat.exe") != std::wstring(L"chat.exe"));
    CHECK(cd::CudaAppKeyFor(std::wstring()).empty());
}

void Test_AK9_OneLinePerApplication() {
    Case("AK9 E2: ONE LINE PER APPLICATION - a second Apply replaces it, it never piles up");
    cd::CudaRecordRow first = cudatest::Line(L"Game Optimizer - C:\\Chrome\\chrome.exe",
                                             L"c:/chrome/chrome.exe", cudatest::kId5090);
    first.when = L"1";
    std::vector<cd::CudaRecordRow> rows = cd::CudaRowsWith(std::vector<cd::CudaRecordRow>(), first);
    CHECK_EQ(rows.size(), (size_t)1);
    cd::CudaRecordRow second = first;
    second.lastWrote = cudatest::kId4090;
    second.when = L"2";
    rows = cd::CudaRowsWith(rows, second);
    CHECK_EQ(rows.size(), (size_t)1);
    CHECK_EQ(rows[0].lastWrote, std::wstring(cudatest::kId4090));
    CHECK_EQ(rows[0].when, std::wstring(L"2"));
    cd::CudaRecordRow other = first;
    other.appEntry = L"c:/apps/chat/chat.exe";
    rows = cd::CudaRowsWith(rows, other);
    CHECK_EQ(rows.size(), (size_t)2);          // another application is another line
    CHECK_EQ(rows[0].appEntry, std::wstring(L"c:/chrome/chrome.exe"));   // and the order is kept
    rows = cd::CudaRowsWithout(rows, L"C:/CHROME/CHROME.EXE");           // the entry is matched without case
    CHECK_EQ(rows.size(), (size_t)1);
    CHECK_EQ(rows[0].appEntry, std::wstring(L"c:/apps/chat/chat.exe"));
    CHECK_EQ(cd::CudaRowsWithout(rows, L"nothing.exe").size(), (size_t)1);

    Case("AK9b the line for one executable is found by the one form our own entry takes");
    {
        cd::CudaRecord record;
        record.state = cd::CudaRecordState::Ok;
        record.rows.push_back(cudatest::Line(L"Game Optimizer - C:\\Apps\\Chat\\chat.exe",
                                             L"c:/apps/chat/chat.exe", cudatest::kId4090));
        const cd::CudaRecordRow* got = cd::CudaLineFor(record, L"C:\\Apps\\Chat\\chat.exe");
        CHECK(got != nullptr);
        CHECK_EQ(got->lastWrote, std::wstring(cudatest::kId4090));
        // 🔴 ANOTHER COPY OF THE SAME PROGRAM IS ANOTHER PROGRAM. A bare "chat.exe" entry would have
        // covered it, and this product never makes one.
        CHECK(cd::CudaLineFor(record, L"C:\\Other\\chat.exe") == nullptr);
        CHECK(cd::CudaLineFor(record, L"C:\\Apps\\other.exe") == nullptr);
        CHECK(cd::CudaLineFor(record, std::wstring()) == nullptr);
        cd::CudaRecord bare;
        bare.state = cd::CudaRecordState::Ok;
        bare.rows.push_back(cudatest::Line(L"Google Chrome", L"chat.exe", cudatest::kId4090));
        CHECK(cd::CudaLineFor(bare, L"C:\\Apps\\Chat\\chat.exe") == nullptr);
    }

    Case("AK9c the three states a record can be in, and Missing is the only silent one");
    {
        cd::CudaRecord fresh;
        CHECK(fresh.state == cd::CudaRecordState::Missing);
        CHECK(fresh.rows.empty());
    }
}

void Test_AK10_TheWordsTheDialogsUse() {
    Case("AK10 every refusal has its own sentence, and None has none");
    const cd::CudaRefusal all[] = {
        cd::CudaRefusal::NoNvidiaDriver,          cd::CudaRefusal::NoNvidiaGpuForKey,
        cd::CudaRefusal::AmbiguousIdenticalCards, cd::CudaRefusal::NoIdForGpu,
        cd::CudaRefusal::SeveralIdsForGpu,        cd::CudaRefusal::SeveralToExclude,
        cd::CudaRefusal::SessionRefused,          cd::CudaRefusal::NameAlreadyInUse,
        cd::CudaRefusal::ProfileNameTaken,        cd::CudaRefusal::ProfileNotCreated,
        cd::CudaRefusal::SettingNotWritten,       cd::CudaRefusal::NotSaved,
        cd::CudaRefusal::CouldNotRead,            cd::CudaRefusal::CouldNotLookUp,
        cd::CudaRefusal::MembershipUnknown,       cd::CudaRefusal::RecordUnreadable,
        cd::CudaRefusal::RecordUnusable,          cd::CudaRefusal::RecordedProfileChanged,
        cd::CudaRefusal::ChangedSinceWritten,     cd::CudaRefusal::ProfileNotRemoved,
        cd::CudaRefusal::NotRecorded,             cd::CudaRefusal::NotConfirmed,
        cd::CudaRefusal::NotUndone };
    std::set<std::wstring> seen;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const std::wstring s = cd::CudaRefusalReason(all[i]);
        CHECK(!s.empty());
        CHECK(s.find(L"error") == std::wstring::npos);   // no codes: the product's voice, not the driver's
        seen.insert(s);
    }
    CHECK_EQ(seen.size(), sizeof(all) / sizeof(all[0]));   // no two refusals share a sentence
    CHECK(cd::CudaRefusalReason(cd::CudaRefusal::None).empty());
    // 🔴 E1's OWN REFUSAL HAS NO FIXED SENTENCE, because it has to name an entry and count its programs.
    CHECK(cd::CudaRefusalReason(cd::CudaRefusal::NvidiaManagesIt).empty());

    Case("AK10b 🔴 E9: the sentence that names what NVIDIA manages, and counts what goes with it");
    {
        const std::wstring none = cd::FormatCudaNvidiaManagesLine(L"Google Chrome", 0);
        CHECK_EQ(none, std::wstring(L"NVIDIA keeps which GPU CUDA uses for it in its own settings entry "
                                    L"\"Google Chrome\", which Game Optimizer has no record of making, so "
                                    L"Game Optimizer left it alone; set that entry in NVIDIA Control Panel"));
        CHECK(cd::FormatCudaNvidiaManagesLine(L"Microsoft Edge Beta", 1).find(L"1 other program as well") !=
              std::wstring::npos);
        const std::wstring six = cd::FormatCudaNvidiaManagesLine(L"Microsoft Edge Beta", 5);
        CHECK(six.find(L"\"Microsoft Edge Beta\"") != std::wstring::npos);
        CHECK(six.find(L"5 other programs as well") != std::wstring::npos);
        CHECK(six.find(L"NVIDIA Control Panel") != std::wstring::npos);
        // and the row's own refusal picks it, while every other refusal keeps its fixed sentence
        cd::CudaRowResult r;
        r.refusal = cd::CudaRefusal::NvidiaManagesIt;
        r.profileName = L"Microsoft Edge Beta";
        r.otherApps = 5;
        CHECK_EQ(cd::CudaRowRefusalText(r), six);
        r.refusal = cd::CudaRefusal::CouldNotRead;
        CHECK_EQ(cd::CudaRowRefusalText(r), cd::CudaRefusalReason(cd::CudaRefusal::CouldNotRead));
    }

    Case("AK10c the Apply question gains ONE line, and only when a CUDA change is planned");
    CHECK(cd::FormatCudaConfirmLine(false, L"RTX 4090").empty());
    {
        const std::wstring plain = cd::FormatCudaConfirmLine(true, L"RTX 4090");
        // 🔴 IT ASKS, IT DOES NOT PROMISE, and it says out loud what E1 costs the user: a program NVIDIA
        // already keeps its own settings for is left to NVIDIA Control Panel.
        CHECK_EQ(plain, std::wstring(L"This also asks NVIDIA to use RTX 4090 for the CUDA work of any of them "
                                     L"that use CUDA. Game Optimizer only does that through a settings entry "
                                     L"of its own, so a program NVIDIA already keeps its own settings for - "
                                     L"Chrome, Edge and others - is left to NVIDIA Control Panel. The result "
                                     L"says which ones changed."));
        CHECK(plain.find(L"will be set to use") == std::wstring::npos);
    }

    Case("AK10d a confirm with no CUDA line is byte-identical to the one v0.5.7 asked");
    {
        cd::AssignConfirm c;
        c.count = 2;
        c.targetName = L"RTX 4090";
        const std::wstring without = cd::FormatAssignConfirm(c);
        c.cudaLine = cd::FormatCudaConfirmLine(true, L"RTX 4090");
        const std::wstring with = cd::FormatAssignConfirm(c);
        CHECK_EQ(with.compare(0, without.size(), without), 0);
        CHECK_EQ(with, without + L"\r\n\r\n" + c.cudaLine);
    }

    Case("AK10e the result says what landed, what did not, and where the record is");
    {
        cd::CudaResultText t;
        CHECK(cd::FormatCudaApplyLines(t).empty());        // nothing happened: nothing is said
        CHECK(cd::FormatCudaRemoveLines(t).empty());
        t.changed = 2;
        t.targetName = L"RTX 4090";
        t.already = 1;
        t.recordStarted = true;
        t.recordPath = L"C:\\d\\gpu-cuda-record.txt";
        const std::wstring apply = cd::FormatCudaApplyLines(t);
        CHECK(apply.find(L"2 of them will use RTX 4090 for CUDA as well.") != std::wstring::npos);
        CHECK(apply.find(L"1 already used it for CUDA") != std::wstring::npos);
        CHECK(apply.find(L"C:\\d\\gpu-cuda-record.txt") != std::wstring::npos);
        t.wholeReason = cd::CudaRefusalReason(cd::CudaRefusal::NoNvidiaDriver);
        t.anyRefused = true;
        t.refusedLines = L"\r\n    chat.exe - " + cd::FormatCudaNvidiaManagesLine(L"Google Chrome", 3);
        const std::wstring bad = cd::FormatCudaApplyLines(t);
        CHECK(bad.find(L"not changed for any of them: this computer has no NVIDIA driver to ask.") !=
              std::wstring::npos);
        CHECK(bad.find(L"3 other programs as well") != std::wstring::npos);
        cd::CudaResultText r;
        r.changed = 3;
        CHECK(cd::FormatCudaRemoveLines(r).find(L"put back for 3 of them.") != std::wstring::npos);
        r.wholeReason = cd::CudaRefusalReason(cd::CudaRefusal::SessionRefused);
        // 🔴 E4: WHEN THE CUDA HALF CANNOT RUN, NO GPU ASSIGNMENT IS REMOVED EITHER, and the result says it.
        CHECK(cd::FormatCudaRemoveLines(r).find(L"so no GPU assignment was removed either") !=
              std::wstring::npos);
    }

    Case("AK10f 🔴 E5: a change that could not be taken back out is the loudest thing either dialog says");
    {
        cd::CudaResultText t;
        t.anyUnresolved = true;
        t.unresolvedLines = L"\r\n    chat.exe - " + cd::CudaRefusalReason(cd::CudaRefusal::NotUndone);
        t.recordPath = L"C:\\d\\gpu-cuda-record.txt";
        const std::wstring apply = cd::FormatCudaApplyLines(t);
        CHECK(apply.find(L"was changed and could NOT be put back for:") != std::wstring::npos);
        CHECK(apply.find(L"chat.exe") != std::wstring::npos);
        CHECK(apply.find(L"Nothing after that was tried, for the GPU setting or for CUDA.") !=
              std::wstring::npos);
        CHECK(apply.find(L"C:\\d\\gpu-cuda-record.txt") != std::wstring::npos);
        const std::wstring remove = cd::FormatCudaRemoveLines(t);
        CHECK(remove.find(L"was changed and could NOT be put back for:") != std::wstring::npos);
        // And a run with nothing unresolved never says any of it.
        cd::CudaResultText quiet;
        quiet.changed = 1;
        quiet.targetName = L"RTX 4090";
        CHECK(cd::FormatCudaApplyLines(quiet).find(L"could NOT be put back") == std::wstring::npos);
    }

    Case("AK10g Remove's own CUDA line says what goes away, for how many, and that the box does not stop it");
    {
        // Nothing of ours on record for any ticked row: Remove's question says nothing about CUDA at all.
        CHECK(cd::FormatCudaRestoreConfirmLine(0, false, std::wstring()).empty());
        CHECK(cd::FormatCudaRestoreConfirmLine(0, true, std::wstring()).empty());
        const std::wstring one = cd::FormatCudaRestoreConfirmLine(1, false, std::wstring());
        CHECK_EQ(one, std::wstring(L"This also takes away the NVIDIA settings entry Game Optimizer made for 1 "
                                   L"of them, so which GPU CUDA uses goes back to what it was before. The "
                                   L"result says which ones changed."));
        CHECK(cd::FormatCudaRestoreConfirmLine(2, false, std::wstring()).find(L"for 2 of them") !=
              std::wstring::npos);
        // 🔴 WITH THE CHECK BOX CLEARED, THE QUESTION SAYS THE UNDO HAPPENS ANYWAY. The box gates Apply's
        // writes only - if it gated this too, the one switch a user flips to stop this touching CUDA would
        // also be the switch that makes an earlier CUDA change permanent.
        const std::wstring off = cd::FormatCudaRestoreConfirmLine(1, true, std::wstring());
        CHECK_EQ(off, one + L" Game Optimizer does that whether or not \"Also set which GPU CUDA uses\" is "
                            L"ticked, so a change it made can always be undone.");
        CHECK(one.find(L"whether or not") == std::wstring::npos);   // and it is not said when the box is ticked
    }

    Case("AK10h 🔴 E4: a Remove that cannot put CUDA back says BEFORE Yes that nothing is removed");
    {
        const std::wstring blocked =
            cd::FormatCudaRestoreConfirmLine(3, false, cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable));
        CHECK(blocked.find(L"Nothing is removed at all") == 0);
        CHECK(blocked.find(cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)) != std::wstring::npos);
        CHECK(blocked.find(L"would leave no way back") != std::wstring::npos);
        CHECK(blocked.find(L"takes away the NVIDIA settings entry") == std::wstring::npos);
        // and it is said even when no ticked row has a line, because nothing is removed either way
        CHECK(!cd::FormatCudaRestoreConfirmLine(0, false,
                                                cd::CudaRefusalReason(cd::CudaRefusal::NoNvidiaDriver)).empty());
        cd::RemoveConfirm c;
        c.count = 3;
        c.cudaLine = blocked;
        CHECK(cd::FormatRemoveConfirm(c).find(L"Nothing is removed at all") != std::wstring::npos);
    }

    Case("AK10i a restore whose record could not be updated afterwards is named, not left quiet");
    {
        cd::CudaResultText u;
        u.changed = 1;
        u.anyUnrecorded = true;
        u.unrecordedLines = L"\r\n    chat.exe - the record could not be updated";
        const std::wstring s = cd::FormatCudaRemoveLines(u);
        CHECK(s.find(L"put back for 1 of them.") != std::wstring::npos);
        CHECK(s.find(L"the record of it could not be updated") != std::wstring::npos);
    }

    Case("AK10j a Remove refusal says the GPU assignment was left in place, so it can be tried again");
    {
        cd::CudaResultText t;
        t.anyRefused = true;
        t.refusedLines = L"\r\n    chat.exe - " + cd::CudaRefusalReason(cd::CudaRefusal::ChangedSinceWritten);
        const std::wstring s = cd::FormatCudaRemoveLines(t);
        CHECK(s.find(L"their GPU assignment was left in place and Remove assignment can try again") !=
              std::wstring::npos);
    }
}

void Test_AK11_TheSettingRoundTrips() {
    Case("AK11 'Also set which GPU CUDA uses' is ON by default and survives a save and load");
    cd::Config c;
    CHECK(c.setCudaGpu);
    c.setCudaGpu = false;
    cd::Config back;
    std::wstring err;
    CHECK(cd::ParseConfig(cd::SerializeConfig(c), back, &err));
    CHECK(!back.setCudaGpu);
    c.setCudaGpu = true;
    CHECK(cd::ParseConfig(cd::SerializeConfig(c), back, &err));
    CHECK(back.setCudaGpu);
    // An older config has no such key, and the default must not flip to off because it is missing.
    cd::Config old;
    CHECK(cd::ParseConfig(L"[gpus]\r\nauto_isolate=true\r\n", old, &err));
    CHECK(old.setCudaGpu);
}

// The branches the Council's three rounds put into the product, one test each: a mutant that takes any of
// them out has to go red here.
void Test_AK12_AFailedReadIsNeverNoSetting() {
    Case("AK12 a CUDA setting on OUR OWN entry that could not be read REFUSES the row");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    cudatest::Own(f, exe, cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe));
    f.settings[cd::CudaProfileNameFor(exe)] = cudatest::kId4090;
    f.readFails = true;
    const cd::CudaOps ops = cudatest::OpsOf(f);
    const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
    cudatest::Rec rec;
    rec.record.rows.push_back(cudatest::Line(cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe),
                                             cudatest::kId4090));

    const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
    CHECK(!p.act);
    CHECK(p.refusal == cd::CudaRefusal::CouldNotRead);
    CHECK(!p.hadPrevious);                               // and above all NOT "there was none"
    const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
    CHECK(r.outcome == cd::CudaOutcome::Refused);
    CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
    CHECK(f.written.empty());                            // nothing written over what nobody can see
    CHECK_EQ(f.saves, 0);

    Case("AK12b our own entry holding no such setting is still 'there was none'");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Own(n, exe, cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe));
        cudatest::Rec nrec;
        nrec.record.rows.push_back(cudatest::Line(cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe),
                                                  cd::CudaNoneValue()));
        const cd::CudaRowResult r2 =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r2.outcome == cd::CudaOutcome::Written);
        CHECK(!r2.hadPrevious);
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId5090));
    }

    Case("AK12c no reader at all is a failed read, not an empty one");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Own(n, exe, cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe));
        cd::CudaOps bare = cudatest::OpsOf(n);
        bare.readSetting = nullptr;
        cudatest::Rec nrec;
        nrec.record.rows.push_back(cudatest::Line(cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe),
                                                  cudatest::kId4090));
        CHECK(cd::WriteCudaForRow(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), bare, nrec.record,
                                  cudatest::InputsOf(nrec))
                  .refusal == cd::CudaRefusal::CouldNotRead);
        CHECK(n.written.empty());
    }

    Case("AK12d a LIVE value the driver could not have written counts as could-not-read");
    {
        // 🔴 Recording it would make a note Remove must later refuse as damaged, so the overwrite could
        // never be undone through this product. Refusing the row leaves the strange value alone instead.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Own(n, exe, cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe));
        n.settings[cd::CudaProfileNameFor(exe)] = L"autoselect";   // not a CUDA_EXCLUDED_GPUS_ID value
        cudatest::Rec nrec;
        nrec.record.rows.push_back(cudatest::Line(cd::CudaProfileNameFor(exe), cd::CudaAppKeyFor(exe),
                                                  cudatest::kId4090));
        const cd::CudaRowResult r2 =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r2.outcome == cd::CudaOutcome::Refused);
        CHECK(r2.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK(n.written.empty());
        CHECK_EQ(n.saves, 0);
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(exe)], std::wstring(L"autoselect"));
        // an empty string is the same thing, and NVIDIA's own "none" is not
        n.settings[cd::CudaProfileNameFor(exe)] = std::wstring();
        CHECK(cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec)
                  .refusal == cd::CudaRefusal::CouldNotRead);
        n.settings[cd::CudaProfileNameFor(exe)] = cd::CudaNoneValue();
        CHECK(cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec)
                  .outcome == cd::CudaOutcome::Written);
    }
}

void Test_AK13_MembershipAndIdenticalCards() {
    Case("AK13 an entry whose applications could not all be listed refuses the row, and claims no count");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    cudatest::Own(f, exe, L"Microsoft Edge Beta", L"chat.exe", false, 1);
    f.byExe[cd::ToLower(exe)].appsComplete = false;
    const cd::CudaOps ops = cudatest::OpsOf(f);
    const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
    cudatest::Rec rec;
    const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
    CHECK(!p.act);
    CHECK(p.refusal == cd::CudaRefusal::MembershipUnknown);
    CHECK_EQ(p.otherApps, (size_t)0);    // 🔴 half a count is never carried out as if it were the whole one
    const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
    CHECK(r.refusal == cd::CudaRefusal::MembershipUnknown);
    CHECK(f.written.empty());
    CHECK_EQ(f.saves, 0);
    CHECK_EQ(f.creates, 0);
    // 🔴 R5-7: AND THE SENTENCE NAMES THE ENTRY, because the lookup found it and only its membership could
    // not be listed. It still states no COUNT - that is the number nobody measured.
    CHECK_EQ(cd::CudaRowRefusalText(r),
             cd::FormatCudaMembershipUnknownLine(L"Microsoft Edge Beta"));
    CHECK(cd::CudaRowRefusalText(r).find(L"\"Microsoft Edge Beta\"") != std::wstring::npos);
    CHECK(cd::CudaRowRefusalText(r).find(L"other program") == std::wstring::npos);

    Case("AK13b an entry whose list IS complete is still NVIDIA's, and is still not written (E1)");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Own(n, exe, L"Microsoft Edge Beta", L"chat.exe", false, 1);
        cudatest::Rec nrec;
        const cd::CudaRowResult r2 =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r2.outcome == cd::CudaOutcome::Refused);
        CHECK(r2.refusal == cd::CudaRefusal::NvidiaManagesIt);
        CHECK_EQ(r2.otherApps, (size_t)1);
        CHECK(n.settings.empty());
    }

    Case("AK13c two cards carrying the target key refuse rather than excluding nothing");
    {
        std::vector<cd::NvidiaGpu> twins = cudatest::TwoCards();
        twins[1].adapterKey = cudatest::kKey5090;   // two of the same card, one key between them
        const cd::CudaPlan p2 = cd::CudaPlanFor(cudatest::kKey5090, twins);
        CHECK(!p2.act);
        CHECK(p2.refusal == cd::CudaRefusal::AmbiguousIdenticalCards);
        CHECK(p2.excludeKeys.empty());
        const cd::CudaTarget t = cd::CudaTargetFor(cudatest::kKey5090, twins, cudatest::BothIds());
        CHECK(!t.act);
        CHECK(t.refusal == cd::CudaRefusal::AmbiguousIdenticalCards);
        CHECK(t.value.empty());   // NOT NVIDIA's "nothing is excluded", which is what it used to write
    }
}

void Test_AK14_AFailedSaveLeavesNothingBehind() {
    Case("AK14 a row whose save failed is taken back out of the session");
    const std::wstring first = L"C:\\Apps\\one\\one.exe";
    const std::wstring second = L"C:\\Apps\\two\\two.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    f.saveFails = true;
    const cd::CudaOps ops = cudatest::OpsOf(f);
    const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
    cudatest::Rec rec;

    const cd::CudaRowResult a = cudatest::Apply(first, target, ops, rec);
    CHECK(a.outcome == cd::CudaOutcome::Refused);
    CHECK(a.refusal == cd::CudaRefusal::NotSaved);
    CHECK(a.undone);
    CHECK(!a.unresolved);
    CHECK_EQ(f.settings.count(cd::CudaProfileNameFor(first)), (size_t)0);   // gone from the SESSION
    CHECK_EQ(f.byExe.count(cd::ToLower(first)), (size_t)0);                 // and so is the entry it made
    CHECK(f.saved.empty());

    Case("AK14b so the NEXT row's save cannot commit it, which is the whole point of the rollback");
    f.saveFails = false;
    const cd::CudaRowResult b = cudatest::Apply(second, target, ops, rec);
    CHECK(b.outcome == cd::CudaOutcome::Written);
    CHECK_EQ(f.saved.count(cd::CudaProfileNameFor(second)), (size_t)1);
    CHECK_EQ(f.saved.count(cd::CudaProfileNameFor(first)), (size_t)0);
    CHECK_EQ(f.saved.size(), (size_t)1);

    Case("AK14c undoing a row on an entry of ours that already held a value puts THAT value back");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Own(n, first, cd::CudaProfileNameFor(first), cd::CudaAppKeyFor(first));
        n.settings[cd::CudaProfileNameFor(first)] = cudatest::kId4090;   // what an earlier Apply left
        n.saveFails = true;
        cudatest::Rec nrec;
        nrec.record.rows.push_back(cudatest::Line(cd::CudaProfileNameFor(first), cd::CudaAppKeyFor(first),
                                                  cudatest::kId4090));
        const cd::CudaRowResult r =
            cudatest::Apply(first, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.undone);
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(first)], std::wstring(cudatest::kId4090));   // NOT cleared
        CHECK_EQ(n.deletes, 0);                                                                 // NOT deleted
    }

    Case("AK14d a save that works but a value that does not read back is refused and undone too");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.writeIgnored = true;   // the driver answers yes and changes nothing
        cudatest::Rec nrec;
        const cd::CudaRowResult r =
            cudatest::Apply(first, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotConfirmed);
        CHECK(r.undone);
        CHECK(!r.unresolved);
        CHECK_EQ(n.byExe.count(cd::ToLower(first)), (size_t)0);   // the entry it made went with it
        CHECK(n.profiles.empty());
    }

    Case("AK14e 🔴 E5: A ROLLBACK THAT ITSELF FAILS IS NOT 'undone' - it is unresolved, and it stops the run");
    {
        // 🔴 The old UndoCudaWrite wrote the old value back and reported undone=true whatever the driver
        // answered, so a rollback that failed looked exactly like one that worked - and the next row's
        // save then committed the change nobody had taken out.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.saveFails = true;
        n.failWriteNumber = 2;   // the row's own write lands; the rollback's write does not
        cudatest::Rec nrec;
        const cd::CudaRowResult r =
            cudatest::Apply(first, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotUndone);
        CHECK(!r.undone);
        CHECK(r.unresolved);
        CHECK(r.recorded);                                       // the line was written before the save
        CHECK_EQ(nrec.record.rows.size(), (size_t)1);
        // and the value is still sitting in the session, which is exactly why no later row may be tried
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(first)], std::wstring(cudatest::kId5090));
    }
    {
        // The same shape on the read-back path: the write landed on disk, and the undo's save refuses.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        n.writeIgnored = true;     // so the read-back cannot confirm it
        n.failSaveNumber = 2;      // the row saves; the rollback's save does not
        cudatest::Rec nrec;
        const cd::CudaRowResult r =
            cudatest::Apply(first, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.refusal == cd::CudaRefusal::NotUndone);
        CHECK(r.unresolved);
        CHECK(!r.undone);
    }
}

void Test_AK15_WhatRemoveSaysWhenThereWasNothingToPutBack() {
    Case("AK15 'there was no record' is SAID, and is not the sentence 'it could not be read'");
    cd::CudaResultText t;
    t.noRecord = 3;
    const std::wstring quiet = cd::FormatCudaRemoveLines(t);
    CHECK(quiet.find(L"3 of them never had their CUDA GPU changed here") != std::wstring::npos);
    CHECK(quiet.find(L"was not put back for") == std::wstring::npos);
    t.anyRefused = true;
    t.refusedLines = L"\r\n    chat.exe - " + cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable);
    const std::wstring loud = cd::FormatCudaRemoveLines(t);
    CHECK(loud.find(L"Which GPU CUDA uses was not put back for the applications below") != std::wstring::npos);
    CHECK(loud.find(cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)) != std::wstring::npos);
    CHECK(cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable) !=
          cd::CudaRefusalReason(cd::CudaRefusal::RecordUnusable));

    Case("AK15b an entry that is gone is said as 'nothing to put back', not as a failure");
    {
        cd::CudaResultText u;
        u.already = 1;
        const std::wstring one = cd::FormatCudaRemoveLines(u);
        CHECK(one.find(L"no longer has NVIDIA settings of its own") != std::wstring::npos);
        CHECK(one.find(L"not put back for") == std::wstring::npos);
        u.already = 2;
        CHECK(cd::FormatCudaRemoveLines(u).find(L"no longer have NVIDIA settings of their own") !=
              std::wstring::npos);
    }

    Case("AK15c a Remove where nothing at all happened still says nothing at all");
    CHECK(cd::FormatCudaRemoveLines(cd::CudaResultText()).empty());
}

// A LOOKUP that failed must never be taken for "NVIDIA has no settings entry for it yet" - which is worse
// than a failed READ, because that answer does not stop the row, it makes a new entry for a program NVIDIA
// may already manage.
void Test_AK16_AFailedLookupIsNeverNoProfile() {
    Case("AK16 a profile lookup that FAILED refuses the row and makes nothing");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    f.settings[L"Google Chrome"] = cudatest::kId4090;   // whatever NVIDIA may already keep for that name
    f.lookupFails = true;
    const cd::CudaOps ops = cudatest::OpsOf(f);
    const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
    cudatest::Rec rec;

    const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
    CHECK(!p.act);
    CHECK(p.refusal == cd::CudaRefusal::CouldNotLookUp);
    CHECK(!p.profileExists);                             // and above all NOT "so one must be made"
    CHECK(p.profileName.empty());
    const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
    CHECK(r.outcome == cd::CudaOutcome::Refused);
    CHECK(r.refusal == cd::CudaRefusal::CouldNotLookUp);
    CHECK_EQ(f.creates, 0);                              // THE COST OF THE OLD ANSWER: a whole new entry
    CHECK(f.written.empty());
    CHECK_EQ(f.saves, 0);
    CHECK(rec.record.rows.empty());
    CHECK_EQ(f.settings[L"Google Chrome"], std::wstring(cudatest::kId4090));

    Case("AK16b the driver SAYING it has no entry still means 'make one'");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::Rec nrec;
        const cd::CudaRowResult r2 =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r2.outcome == cd::CudaOutcome::Written);
        CHECK_EQ(n.creates, 1);
        CHECK(!r2.hadPrevious);
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId5090));
    }

    Case("AK16c no lookup at all is a failed lookup, not an empty one");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cd::CudaOps bare = cudatest::OpsOf(n);
        bare.findProfileForExe = nullptr;
        cudatest::Rec nrec;
        const cd::CudaRowResult r3 = cd::WriteCudaForRow(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids),
                                                         bare, nrec.record, cudatest::InputsOf(nrec));
        CHECK(r3.outcome == cd::CudaOutcome::Refused);
        CHECK(r3.refusal == cd::CudaRefusal::CouldNotLookUp);
        CHECK_EQ(n.creates, 0);
        CHECK(n.written.empty());
    }
}

// 🔴 THE ROUND-2 BLOCKER, IN ONE TEST. Assign an application to NVIDIA card A, later assign it to card B,
// then Remove. The old per-run journal compared the driver against what the FIRST Apply wrote, saw the
// SECOND Apply's value, and announced that somebody else had changed it.
void Test_AK17_TwoAppliesThenRemove() {
    Case("AK17 assign to one NVIDIA card, then the other, then Remove: our entry goes, with no conflict");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    cudatest::Fake f;
    f.gpus = cudatest::TwoCards();
    f.ids = cudatest::BothIds();
    const cd::CudaOps ops = cudatest::OpsOf(f);
    cudatest::Rec rec;

    const cd::CudaRowResult one =
        cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), ops, rec);
    CHECK(one.outcome == cd::CudaOutcome::Written);
    CHECK_EQ(f.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId5090));
    CHECK_EQ(rec.record.rows.size(), (size_t)1);

    const cd::CudaRowResult two =
        cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey5090, f.gpus, f.ids), ops, rec);
    CHECK(two.outcome == cd::CudaOutcome::Written);
    CHECK_EQ(f.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId4090));
    // 🔴 STILL ONE LINE, and it names the value the LAST Apply wrote - which is what Remove compares.
    CHECK_EQ(rec.record.rows.size(), (size_t)1);
    CHECK_EQ(rec.record.rows[0].lastWrote, std::wstring(cudatest::kId4090));

    const cd::CudaRecordRow* line = cd::CudaLineFor(rec.record, exe);
    CHECK(line != nullptr);
    const cd::CudaRowResult back = cd::RestoreCudaForRow(exe, *line, ops);
    CHECK(back.outcome == cd::CudaOutcome::Written);
    CHECK(back.refusal != cd::CudaRefusal::ChangedSinceWritten);   // 🔴 the false conflict is gone
    CHECK(f.settings.empty());                                     // and the entry went with the setting
    CHECK(f.profiles.empty());

    Case("AK17b and the line is spent, so a second Remove finds nothing of ours to put back");
    rec.record.rows = cd::CudaRowsWithout(rec.record.rows, line->appEntry);
    CHECK(rec.record.rows.empty());
    CHECK(cd::CudaLineFor(rec.record, exe) == nullptr);

    Case("AK17c a real change by somebody else IS still a conflict, and is left alone");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        const cd::CudaOps nops = cudatest::OpsOf(n);
        cudatest::Rec nrec;
        cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), nops, nrec);
        n.settings[cd::CudaProfileNameFor(exe)] = cudatest::kId4090;   // the user, in NVIDIA's own panel
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, *cd::CudaLineFor(nrec.record, exe), nops);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::ChangedSinceWritten);
        CHECK_EQ(n.settings[cd::CudaProfileNameFor(exe)], std::wstring(cudatest::kId4090));
        CHECK_EQ(nrec.record.rows.size(), (size_t)1);       // and the line is KEPT, so it can be retried
        CHECK_EQ(n.deletes, 0);
    }
}

// 🔴 E1 IS THE WHOLE OF v0.5.8, so it gets the test that enumerates every shape of entry there is.
void Test_AK18_OnlyAnEntryWeOwn() {
    Case("AK18 E1: the six shapes an entry can have, and the ONE this product writes on");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const cd::CudaRecordRow line = cudatest::Line(ours, entry, cudatest::kId5090);
    {
        // 1. NO ENTRY AT ALL -> one of ours is made. (AK6 proves the write; this proves the ruling.)
        cudatest::Fake n;
        cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(res.state == cd::CudaLookup::Absent);
    }
    {
        // 2. OUR OWN ENTRY, ONE MEMBER, NAMED IN THE RECORD -> written.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(cd::CudaOwnershipRefusal(res, exe, ours) == cd::CudaRefusal::None);
        // 🔴 AND THE RECORD IS PART OF THE TEST. Without a line naming it, an entry carrying our own name
        // is one an earlier run - or a user - left behind, and it is not ours to write on.
        CHECK(cd::CudaOwnershipRefusal(res, exe, std::wstring()) == cd::CudaRefusal::NvidiaManagesIt);
        CHECK(cd::CudaOwnershipRefusal(res, exe, L"Google Chrome") == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        // 3. A PREDEFINED NVIDIA ENTRY -> never written, whatever it is called.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry, true);
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(cd::CudaOwnershipRefusal(res, exe, ours) == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        // 4. AN ENTRY WITH A SECOND MEMBER -> never written: the other member cannot be given a different
        // CUDA GPU, which is the root founder decision 22 answers.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry, false, 1);
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(cd::CudaOwnershipRefusal(res, exe, ours) == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        // 5. A MEMBERSHIP THAT COULD NOT BE LISTED -> never written, and no count is claimed.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        n.byExe[cd::ToLower(exe)].appsComplete = false;
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(cd::CudaOwnershipRefusal(res, exe, ours) == cd::CudaRefusal::MembershipUnknown);
    }
    {
        // 6. AN ENTRY MATCHED BY BARE FILE NAME -> never ours: our own entry is always the full path.
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, L"chat.exe");
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(n));
        CHECK(cd::CudaOwnershipRefusal(res, exe, ours) == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        // and a lookup nobody could make is neither of the six
        cudatest::Fake dead;
        dead.available = false;
        const cd::CudaResolved none = cd::ResolveCudaProfile(exe, cudatest::OpsOf(dead));
        CHECK(none.state == cd::CudaLookup::Failed);
        CHECK(cd::CudaOwnershipRefusal(none, exe, ours) == cd::CudaRefusal::CouldNotLookUp);
    }

    Case("AK18b an entry carrying our name that no line claims is NOT adopted, and nothing is changed");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::LeftBehind(n, exe);            // the profile is there; it covers no application
        cudatest::Rec nrec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::ProfileNameTaken);
        CHECK(n.written.empty());
        CHECK_EQ(n.saves, 0);
        CHECK(nrec.record.rows.empty());
        CHECK_EQ(n.byExe.count(cd::ToLower(exe)), (size_t)0);   // its application entry was NOT put back
        CHECK_EQ(n.profiles.count(ours), (size_t)1);            // and the entry itself is untouched
    }

    Case("AK18c the SAME entry, with a line of ours naming it, IS adopted - and read before it is written");
    {
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::LeftBehind(n, exe);
        n.settings[ours] = cudatest::kId4090;    // a value that earlier run left in it
        cudatest::Rec nrec;
        nrec.record.rows.push_back(line);
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.hadPrevious);
        CHECK_EQ(r.previousValue, std::wstring(cudatest::kId4090));
        CHECK_EQ(n.settings[ours], std::wstring(cudatest::kId5090));
        CHECK_EQ(nrec.record.rows.size(), (size_t)1);
        CHECK_EQ(nrec.record.rows[0].lastWrote, std::wstring(cudatest::kId5090));
    }
    {
        // An adopted entry whose setting cannot be read refuses the row, exactly as a found one does.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::LeftBehind(n, exe);
        n.readFails = true;
        cudatest::Rec nrec;
        nrec.record.rows.push_back(line);
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK(n.written.empty());
        CHECK_EQ(n.saves, 0);
    }
    {
        // 🔴 AND AN ADOPTED ENTRY IS NOT DELETED WHEN THE ROW ROLLS BACK: this run did not create it.
        cudatest::Fake n;
        n.gpus = cudatest::TwoCards();
        n.ids = cudatest::BothIds();
        cudatest::LeftBehind(n, exe);
        n.saveFails = true;
        cudatest::Rec nrec;
        nrec.record.rows.push_back(line);
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), nrec);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.undone);
        CHECK(!r.profileDeleted);
        CHECK_EQ(n.deletes, 0);
        CHECK_EQ(n.profiles.count(ours), (size_t)1);
    }

    Case("AK18d the delete carries the application entry, and nothing else is ever deleted");
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, L"c:/somebody/else.exe");   // our name, another program's entry
        CHECK(!cd::DropCudaProfileWeMade(ours, entry, cudatest::OpsOf(n)));
        CHECK_EQ(n.deletes, 1);                                 // asked, and refused by the driver's guard
        CHECK_EQ(n.byExe.size(), (size_t)1);
    }
    {
        cudatest::Fake n;
        cudatest::Own(n, exe, ours, entry);
        // Never asked at all: no entry, or a name this feature does not make.
        CHECK(!cd::DropCudaProfileWeMade(ours, std::wstring(), cudatest::OpsOf(n)));
        CHECK(!cd::DropCudaProfileWeMade(L"Google Chrome", entry, cudatest::OpsOf(n)));
        CHECK_EQ(n.deletes, 0);   // 🔴 NEITHER of them even reaches the driver: the guards are asked first
        CHECK(cd::DropCudaProfileWeMade(ours, entry, cudatest::OpsOf(n)));
        CHECK_EQ(n.deletes, 1);
    }
}

// Remove fails closed on exactly what Apply fails closed on. Until round 3 it wrote on a profile NAME
// taken out of a file and asked the driver nothing at all.
void Test_AK19_RemoveFailsClosedOnIdentity() {
    Case("AK19 Remove refuses every entry that is not still the one our line names");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const cd::CudaRecordRow rec = cudatest::Line(ours, entry, cudatest::kId5090);
    {
        cudatest::Fake n;                                     // another entry answers for it now
        cudatest::Own(n, exe, L"Google Chrome", L"chat.exe", true);
        n.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, rec, cudatest::OpsOf(n));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::RecordedProfileChanged);
        CHECK_EQ(n.deletes, 0);
        CHECK_EQ(n.settings[ours], std::wstring(cudatest::kId5090));
    }
    {
        cudatest::Fake n;                                     // ours, and it has gained a second member
        cudatest::Own(n, exe, ours, entry, false, 1);
        n.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, rec, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::RecordedProfileChanged);
        CHECK_EQ(n.deletes, 0);
    }
    {
        cudatest::Fake n;                                     // ours, and NVIDIA now calls it predefined
        cudatest::Own(n, exe, ours, entry, true);
        n.settings[ours] = cudatest::kId5090;
        CHECK(cd::RestoreCudaForRow(exe, rec, cudatest::OpsOf(n)).refusal ==
              cd::CudaRefusal::RecordedProfileChanged);
    }
    {
        cudatest::Fake n;                                     // it no longer covers our application entry
        cudatest::Own(n, exe, ours, L"chat.exe");
        n.settings[ours] = cudatest::kId5090;
        CHECK(cd::RestoreCudaForRow(exe, rec, cudatest::OpsOf(n)).refusal ==
              cd::CudaRefusal::RecordedProfileChanged);
    }
    {
        cudatest::Fake n;                                     // a membership it would not finish listing
        cudatest::Own(n, exe, ours, entry);
        n.byExe[cd::ToLower(exe)].appsComplete = false;
        n.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, rec, cudatest::OpsOf(n));
        CHECK(r.refusal == cd::CudaRefusal::MembershipUnknown);
        CHECK_EQ(r.otherApps, (size_t)0);
        CHECK_EQ(n.deletes, 0);
    }
}

// 🔴 E4 AND E5 AT THE LEVEL THE ROWS ARE RUN AT. The CUDA half used to run in a second pass over the
// results, so a Remove could strip an assignment before finding out the NVIDIA entry had to stay, and an
// Apply whose rollback failed carried on writing later applications.
void Test_AK20_TheCudaGateDecidesTheRow() {
    Case("AK20 E4: on Remove a CUDA refusal leaves the row's GPU assignment exactly where it is");
    std::map<std::wstring, std::wstring> prefs;
    prefs[L"a.exe"] = L"GpuPreference=2;";
    prefs[L"b.exe"] = L"GpuPreference=2;";
    std::vector<cd::GpuEditItem> items;
    cd::GpuEditItem a;
    a.exePath = L"a.exe";
    a.present = true;
    a.existing = L"GpuPreference=2;";
    items.push_back(a);
    cd::GpuEditItem b = a;
    b.exePath = L"b.exe";
    items.push_back(b);
    std::vector<size_t> asked;
    cd::GpuEditOps ops;
    ops.write = [&prefs](const std::wstring& path, bool, const std::wstring&, bool deleting,
                         const std::wstring& value, unsigned long&, bool) {
        if (deleting) prefs.erase(path);
        else prefs[path] = value;
        return cd::GuardedWriteResult::Written;
    };
    ops.read = [&prefs](const std::wstring& path, std::wstring& value, bool& unreadable) {
        unreadable = false;
        const std::map<std::wstring, std::wstring>::const_iterator it = prefs.find(path);
        value = it == prefs.end() ? std::wstring() : it->second;
        return it != prefs.end();
    };
    ops.record = [](const cd::GpuPreferenceBefore&) { return true; };
    ops.cudaRow = [&asked](size_t index, std::wstring& reason) {
        asked.push_back(index);
        if (index != 0) return cd::GpuRowGate::Go;
        reason = L"its GPU assignment was left in place because which GPU CUDA uses could not be put back";
        return cd::GpuRowGate::SkipRow;
    };
    const std::vector<cd::GpuEditResult> out = cd::RunGpuEdits(items, true, std::wstring(), ops);
    CHECK(out[0].outcome == cd::GpuEditOutcome::Refused);
    CHECK(out[0].reason.find(L"left in place") != std::wstring::npos);
    CHECK_EQ(prefs.count(L"a.exe"), (size_t)1);       // 🔴 the assignment is still there, so Remove stays lit
    CHECK(out[1].outcome == cd::GpuEditOutcome::Done);
    CHECK_EQ(prefs.count(L"b.exe"), (size_t)0);       // and a row the CUDA half was happy with still went
    CHECK_EQ(asked.size(), (size_t)2);

    Case("AK20b E5: a CUDA change that could not be taken back out stops every later row, GPU included");
    {
        prefs.clear();
        prefs[L"a.exe"] = L"AppStatus=1;";
        prefs[L"b.exe"] = L"AppStatus=1;";
        asked.clear();
        cd::GpuEditOps apply = ops;
        apply.cudaRow = [&asked](size_t index, std::wstring&) {
            asked.push_back(index);
            return index == 0 ? cd::GpuRowGate::StopRun : cd::GpuRowGate::Go;
        };
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, false, L"10DE&2684&40BF1458", apply);
        CHECK(r[0].outcome == cd::GpuEditOutcome::Done);                  // its own write landed first
        CHECK(r[1].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK(r[1].reason.find(L"which GPU CUDA uses was changed for an application before it") !=
              std::wstring::npos);
        CHECK_EQ(asked.size(), (size_t)1);            // 🔴 the later row's CUDA half is not asked either
        CHECK(prefs[L"b.exe"].find(L"GpuPreference") == std::wstring::npos);
    }

    Case("AK20c with no CUDA half at all, both actions behave exactly as v0.5.7's did");
    {
        prefs.clear();
        prefs[L"a.exe"] = L"AppStatus=1;";
        prefs[L"b.exe"] = L"AppStatus=1;";
        cd::GpuEditOps plain = ops;
        plain.cudaRow = nullptr;
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, false, L"10DE&2684&40BF1458", plain);
        CHECK(r[0].outcome == cd::GpuEditOutcome::Done);
        CHECK(r[1].outcome == cd::GpuEditOutcome::Done);
    }

    // 🔴 R5-3: THE ROUND-4 FINDING BOTH SEATS MADE, AND THE ONE THE OPERATOR WOULD HAVE MET FIRST. Every
    // application v0.5.6 and v0.5.7 pinned is a row whose Windows GPU preference ALREADY equals what this
    // Apply intends - AlreadyDone - and the CUDA half was gated on Done, so "GPU assignment shall also
    // change the CUDA dedication GPU" did nothing at all for them, in silence.
    Case("AK20d 🔴 R5-3: APPLY RUNS THE CUDA HALF FOR A ROW ALREADY PINNED TO THE TARGET");
    {
        const std::wstring target = L"10DE&2684&40BF1458";
        const std::wstring pinned = cd::MergeGpuPreferenceValue(std::wstring(), target);
        prefs.clear();
        prefs[L"a.exe"] = pinned;             // already exactly what this Apply would write
        prefs[L"b.exe"] = L"AppStatus=1;";    // and one that really changes
        std::vector<cd::GpuEditItem> two;
        cd::GpuEditItem already;
        already.exePath = L"a.exe";
        already.present = true;
        already.existing = pinned;
        two.push_back(already);
        cd::GpuEditItem moves;
        moves.exePath = L"b.exe";
        moves.present = true;
        moves.existing = L"AppStatus=1;";
        two.push_back(moves);
        asked.clear();
        cd::GpuEditOps apply = ops;
        apply.cudaRow = [&asked](size_t index, std::wstring&) {
            asked.push_back(index);
            return cd::GpuRowGate::Go;
        };
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(two, false, target, apply);
        CHECK(r[0].outcome == cd::GpuEditOutcome::AlreadyDone);
        CHECK(r[1].outcome == cd::GpuEditOutcome::Done);
        // 🔴 BOTH rows, not only the one that was written - and one && chain, so a mutant that asks for
        // fewer makes this go RED rather than read past the end of the vector.
        CHECK(asked.size() == 2 && asked[0] == 0 && asked[1] == 1);
    }

    Case("AK20e and an AlreadyDone row's CUDA half can stop the run exactly as a Done row's can (E5)");
    {
        const std::wstring target = L"10DE&2684&40BF1458";
        const std::wstring pinned = cd::MergeGpuPreferenceValue(std::wstring(), target);
        prefs.clear();
        prefs[L"a.exe"] = pinned;
        prefs[L"b.exe"] = L"AppStatus=1;";
        std::vector<cd::GpuEditItem> two;
        cd::GpuEditItem already;
        already.exePath = L"a.exe";
        already.present = true;
        already.existing = pinned;
        two.push_back(already);
        cd::GpuEditItem moves;
        moves.exePath = L"b.exe";
        moves.present = true;
        moves.existing = L"AppStatus=1;";
        two.push_back(moves);
        asked.clear();
        cd::GpuEditOps apply = ops;
        apply.cudaRow = [&asked](size_t index, std::wstring&) {
            asked.push_back(index);
            return index == 0 ? cd::GpuRowGate::StopRun : cd::GpuRowGate::Go;
        };
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(two, false, target, apply);
        CHECK(r[1].outcome == cd::GpuEditOutcome::NotAttempted);
        CHECK_EQ(asked.size(), (size_t)1);
        CHECK(prefs[L"b.exe"].find(L"GpuPreference") == std::wstring::npos);
    }

    Case("AK20f but a Refused, Unconfirmed or NotAttempted row still drags NO CUDA change");
    {
        // The row's guarded write refuses, so its Windows GPU preference is NOT what this run wants - and
        // its CUDA setting must not move either. This is the half of R5-3 that did NOT change.
        prefs.clear();
        prefs[L"a.exe"] = L"AppStatus=1;";
        prefs[L"b.exe"] = L"AppStatus=1;";
        asked.clear();
        cd::GpuEditOps apply = ops;
        apply.write = [](const std::wstring&, bool, const std::wstring&, bool, const std::wstring&,
                         unsigned long&, bool) { return cd::GuardedWriteResult::Changed; };
        apply.cudaRow = [&asked](size_t index, std::wstring&) {
            asked.push_back(index);
            return cd::GpuRowGate::Go;
        };
        const std::vector<cd::GpuEditResult> r = cd::RunGpuEdits(items, false, L"10DE&2684&40BF1458", apply);
        CHECK(r[0].outcome == cd::GpuEditOutcome::Refused);
        CHECK(r[1].outcome == cd::GpuEditOutcome::Refused);
        CHECK(asked.empty());
    }
}

// 🔴 R5-1, THE ROUND-4 BLOCKER (adversarial review, round 4). NVIDIA keeps EVERY per-application setting in one profile, so the
// entry this product made for an executable is also where NVIDIA Control Panel puts that executable's
// other settings. Remove used to delete the whole profile and take them with it.
void Test_AK21_OnlyOurOwnSettingIsRemoved() {
    Case("AK21 R5-1: an entry holding settings this product did not write is NEVER deleted");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const cd::CudaRecordRow line = cudatest::Line(ours, entry, cudatest::kId5090);
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 2;              // the user's own two settings, in the SAME entry
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Written);      // it IS a successful restore
        CHECK(r.settingCleared);
        CHECK(!r.profileDeleted);
        CHECK_EQ(f.deletes, 0);                            // 🔴 the delete was never even asked for
        CHECK_EQ(f.profiles.count(ours), (size_t)1);       // the entry is still there
        CHECK_EQ(f.byExe.count(cd::ToLower(exe)), (size_t)1);   // with its application still in it
        CHECK_EQ(f.otherSettings[ours], (size_t)2);        // and the user's settings untouched
        CHECK_EQ(f.settings.count(ours), (size_t)0);       // only ours went
        // ONE && CHAIN, NOT TWO CHECKS: the second half indexes the vector the first half sizes, and a
        // mutant that empties it must make this go RED rather than read past the end.
        CHECK(f.written.size() == 1 && f.written[0] == ours + L"=CLEAR");
        CHECK_EQ(f.saves, 1);                              // and it was saved and read back
    }

    Case("AK21b an entry holding NOTHING but ours still goes whole, exactly as it did before");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.profileDeleted);
        CHECK(!r.settingCleared);
        CHECK_EQ(f.deletes, 1);
        CHECK(f.profiles.empty());
    }

    Case("AK21c 🔴 THE HEADER'S OWN BRANCH IS WHAT SAVES THEM, not the driver wrapper's guard");
    {
        // A deleteProfile that deletes whatever it is handed - what DeleteOwnProfile would be without its
        // numOfSettings guard. The decision in gpu_cuda.h must be the one that keeps the entry.
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 1;
        int asked = 0;
        cd::CudaOps eager = cudatest::OpsOf(f);
        eager.deleteProfile = [&asked, &f](const std::wstring& name, const std::wstring&) {
            ++asked;
            f.profiles.erase(name);
            f.settings.erase(name);
            f.otherSettings.erase(name);
            return true;
        };
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, eager);
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.settingCleared);
        CHECK_EQ(asked, 0);                                // 🔴 it was never asked
        CHECK_EQ(f.otherSettings[ours], (size_t)1);        // so the user's setting is still there
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
    }

    Case("AK21d every way the clear can fail is said, and none of them reports a restore");
    {
        cudatest::Fake f;                                  // the driver refuses the clear
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 1;
        f.writeFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::SettingNotRemoved);
        CHECK(!r.settingCleared);
        CHECK_EQ(f.settings[ours], std::wstring(cudatest::kId5090));
        CHECK_EQ(f.saves, 0);
    }
    {
        cudatest::Fake f;                                  // the clear lands in the session, the save refuses
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 1;
        f.saveFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.sessionDirty);                             // so no later row of this run may save
        CHECK(f.saved.empty());
    }
    {
        cudatest::Fake f;                                  // the driver says yes and the setting is still there
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 1;
        f.writeIgnored = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotConfirmed);
        CHECK(!r.settingCleared);
    }

    Case("AK21e the result says WHICH of the two restores happened");
    {
        cd::CudaResultText t;
        t.changed = 2;
        t.keptEntry = 1;
        const std::wstring s = cd::FormatCudaRemoveLines(t);
        CHECK(s.find(L"put back for 2 of them.") != std::wstring::npos);
        CHECK(s.find(L"only Game Optimizer's own CUDA setting was taken out of it and the entry itself was "
                     L"left standing.") != std::wstring::npos);
        cd::CudaResultText plain;
        plain.changed = 2;
        CHECK(cd::FormatCudaRemoveLines(plain).find(L"left standing") == std::wstring::npos);
        cd::CudaResultText many;
        many.changed = 3;
        many.keptEntry = 2;
        CHECK(cd::FormatCudaRemoveLines(many).find(L"those entries were left standing.") != std::wstring::npos);
    }
}

// 🔴 R5-2, THE ROUND-4 HIGH (adversarial review, round 4). The adopt path took "a profile of that name exists" straight to
// CreateApplication with no GetProfileInfo at all, and then reported otherApps = 0 as a constant.
void Test_AK22_TheAdoptPathPassesTheOwnershipCheck() {
    Case("AK22 R5-2: the one shape that may be adopted, and the four that may not");
    {
        cd::CudaProfile empty;                       // the leftover of a half-finished earlier run
        CHECK(cd::CudaAdoptRefusal(empty, true) == cd::CudaRefusal::None);
        // 🔴 and not without a line of ours naming it, however it is spelled
        CHECK(cd::CudaAdoptRefusal(empty, false) == cd::CudaRefusal::ProfileNameTaken);
    }
    {
        cd::CudaProfile joined;
        joined.otherApps = 1;                        // another program has joined it since
        CHECK(cd::CudaAdoptRefusal(joined, true) == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        cd::CudaProfile theirs;
        theirs.isPredefined = true;
        CHECK(cd::CudaAdoptRefusal(theirs, true) == cd::CudaRefusal::NvidiaManagesIt);
    }
    {
        cd::CudaProfile unknown;                     // a membership the driver would not finish listing
        unknown.appsComplete = false;
        unknown.otherApps = 3;
        CHECK(cd::CudaAdoptRefusal(unknown, true) == cd::CudaRefusal::MembershipUnknown);
    }

    Case("AK22b 🔴 an entry of our name that ANOTHER PROGRAM has joined is refused, named and counted");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        const std::wstring ours = cd::CudaProfileNameFor(exe);
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        // somebody else's program is in the entry that carries our name - and the lookup for OUR executable
        // still answers Absent, which is exactly how the old code got here
        cudatest::Own(f, L"C:\\Other\\other.exe", ours, L"c:/other/other.exe");
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId5090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NvidiaManagesIt);
        CHECK_EQ(r.profileName, ours);
        CHECK_EQ(r.otherApps, (size_t)1);            // 🔴 MEASURED, not the constant zero
        CHECK_EQ(f.creates, 0);                      // and CreateApplication was never reached
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK(rec.record.rows.size() == 1);
        CHECK(cd::CudaRowRefusalText(r).find(L"1 other program as well") != std::wstring::npos);
    }

    Case("AK22c an entry of our name NVIDIA calls its own is refused too");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        const std::wstring ours = cd::CudaProfileNameFor(exe);
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        cudatest::Own(f, L"C:\\Other\\other.exe", ours, L"c:/other/other.exe", true);
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId5090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NvidiaManagesIt);
        CHECK_EQ(f.creates, 0);
    }

    Case("AK22d a by-name membership nobody could list refuses, and claims no count");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        const std::wstring ours = cd::CudaProfileNameFor(exe);
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.nameAppsIncomplete = true;
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId5090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::MembershipUnknown);
        CHECK_EQ(r.otherApps, (size_t)0);
        CHECK_EQ(f.creates, 0);
        CHECK(cd::CudaRowRefusalText(r).find(ours) != std::wstring::npos);   // R5-7: and it is named
    }

    Case("AK22e a by-name lookup the driver would not answer makes nothing and adopts nothing");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.nameLookupFails = true;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotLookUp);
        CHECK_EQ(f.creates, 0);
        CHECK(f.profiles.empty());
        CHECK(rec.record.rows.empty());
    }
    {
        // and no such op at all is a failed lookup, never "there is no entry of that name"
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cd::CudaOps bare = cudatest::OpsOf(f);
        bare.findProfileByName = nullptr;
        cudatest::Rec rec;
        const cd::CudaRowResult r = cd::WriteCudaForRow(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids),
                                                        bare, rec.record, cudatest::InputsOf(rec));
        CHECK(r.refusal == cd::CudaRefusal::CouldNotLookUp);
        CHECK_EQ(f.creates, 0);
    }
}

// 🔴 R5-4: A CLEAN-UP THAT FAILED IS UNRESOLVED. The profile this row created is still in the open session
// and NvAPI_DRS_SaveSettings commits the WHOLE session, so a later row's save would commit it.
void Test_AK23_ACleanupThatFailedStopsTheRun() {
    Case("AK23 R5-4: a rollback whose profile delete fails is unresolved, dirty, and stops the run");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.saveFails = true;      // the row's save refuses, so the row rolls back
        f.deleteFails = true;    // and the profile it made will not go
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotUndone);
        CHECK(!r.undone);
        CHECK(r.unresolved);         // 🔴 which is what stops the whole run (E5's own gate)
        CHECK(r.sessionDirty);       // 🔴 and what stops any later row from saving
        CHECK_EQ(f.profiles.count(cd::CudaProfileNameFor(exe)), (size_t)1);   // it really is still there
    }

    Case("AK23b the same when the application would not go into the profile this row just made (E7)");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.createAnswer = cd::CudaCreate::AlreadyInUse;
        f.deleteFails = true;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::ProfileNotRemoved);
        CHECK(r.unresolved);
        CHECK(r.sessionDirty);
        CHECK(!r.profileDeleted);
        CHECK_EQ(f.saves, 0);
    }

    Case("AK23c and the same when the setting would not be written at all");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.writeFails = true;
        f.deleteFails = true;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::ProfileNotRemoved);
        CHECK(r.unresolved);
        CHECK(r.sessionDirty);
    }

    Case("AK23d a clean-up that WORKS is none of those, and the run carries on");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.createAnswer = cd::CudaCreate::AlreadyInUse;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NameAlreadyInUse);
        CHECK(r.profileDeleted);
        CHECK(!r.unresolved);
        CHECK(!r.sessionDirty);
        CHECK(f.profiles.empty());
    }
}

// The sentences round 5 made specific, and the two guards that keep the record and the driver honest.
void Test_AK24_TheSentencesThatMustNameSomething() {
    Case("AK24 🔴 R5-7: -167 names the entry that holds the file name, or says it could not be named");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.createAnswer = cd::CudaCreate::AlreadyInUse;
        // the driver keeps that BASE NAME - not the full path - under somebody else's entry, which is
        // exactly what -167 means [M]
        cudatest::Own(f, L"chat.exe", L"Some Other Game", L"chat.exe", false, 2);
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NameAlreadyInUse);
        CHECK_EQ(r.profileName, std::wstring(L"Some Other Game"));
        const std::wstring says = cd::CudaRowRefusalText(r);
        CHECK(says.find(L"\"Some Other Game\"") != std::wstring::npos);
        CHECK(says.find(L"NVIDIA Control Panel") != std::wstring::npos);
        CHECK(says != cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse));
    }
    {
        // and when it cannot be found, the sentence says THAT - it does not pretend there is no entry
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.createAnswer = cd::CudaCreate::AlreadyInUse;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NameAlreadyInUse);
        CHECK(r.profileName.empty());
        CHECK_EQ(cd::CudaRowRefusalText(r), cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse));
        CHECK(cd::CudaRowRefusalText(r).find(L"could not be named") != std::wstring::npos);
    }
    CHECK_EQ(cd::FormatCudaNameInUseLine(std::wstring(), 0),
             cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse));
    CHECK_EQ(cd::FormatCudaNameInUseLine(std::wstring(), 3),
             cd::CudaRefusalReason(cd::CudaRefusal::NameAlreadyInUse));   // no name, so no count either
    CHECK(cd::FormatCudaMembershipUnknownLine(std::wstring()) ==
          cd::CudaRefusalReason(cd::CudaRefusal::MembershipUnknown));
    CHECK(cd::FormatCudaMembershipUnknownLine(L"Microsoft Edge Beta").find(L"\"Microsoft Edge Beta\"") !=
          std::wstring::npos);

    Case("AK24b 🔴 R5-6: the unreadable-note reason names the folder AND the legacy file pattern");
    {
        const std::wstring s = cd::CudaWholeRunReason(cd::CudaRefusal::RecordUnreadable, L"C:\\d\\cfg");
        CHECK(s.find(cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable)) == 0);
        CHECK(s.find(L"C:\\d\\cfg") != std::wstring::npos);
        CHECK(s.find(L"gpu-cuda-before-*.txt") != std::wstring::npos);
        CHECK(s.find(cd::CudaRecordFileName()) != std::wstring::npos);
        // 🔴 pre-publish review: the record is the only note of what to undo, so the sentence tells the user to
        // KEEP it and names only the gpu-cuda-before-*.txt files as the ones to move - never "those two"
        CHECK(s.find(L"move any gpu-cuda-before-*.txt file out") != std::wstring::npos);
        CHECK(s.find(L"keep " + cd::CudaRecordFileName() + L" where it is") != std::wstring::npos);
        CHECK(s.find(L"those two") == std::wstring::npos);
        CHECK(s.find(L"earlier version") == std::wstring::npos);
        // every other refusal keeps its fixed sentence, folder or no folder
        CHECK_EQ(cd::CudaWholeRunReason(cd::CudaRefusal::NoNvidiaDriver, L"C:\\d\\cfg"),
                 cd::CudaRefusalReason(cd::CudaRefusal::NoNvidiaDriver));
        CHECK_EQ(cd::CudaWholeRunReason(cd::CudaRefusal::RecordUnreadable, std::wstring()),
                 cd::CudaRefusalReason(cd::CudaRefusal::RecordUnreadable));
        // and it reaches the question Remove asks BEFORE Yes (E4)
        const std::wstring asked = cd::FormatCudaRestoreConfirmLine(2, false, s);
        CHECK(asked.find(L"Nothing is removed at all") == 0);
        CHECK(asked.find(L"gpu-cuda-before-*.txt") != std::wstring::npos);
    }

    Case("AK24c 🔴 R5-5: an entry of our name that no note claims is SAID, and nothing is done about it");
    {
        const std::wstring s = cd::FormatCudaOrphanEntryLine(L"Game Optimizer - C:\\Apps\\chat\\chat.exe");
        CHECK(s.find(L"\"Game Optimizer - C:\\Apps\\chat\\chat.exe\"") != std::wstring::npos);
        CHECK(s.find(L"has no note of making") != std::wstring::npos);
        CHECK(s.find(L"the GPU assignment was removed anyway") != std::wstring::npos);
        CHECK(s.find(L"NVIDIA Control Panel") != std::wstring::npos);
        cd::CudaResultText t;
        t.anyOrphan = true;
        t.orphanLines = L"\r\n    chat.exe - " + s;
        const std::wstring said = cd::FormatCudaRemoveLines(t);
        CHECK(said.find(L"Nothing was done to those entries:") != std::wstring::npos);
        CHECK(said.find(L"chat.exe") != std::wstring::npos);
        // 🔴 and it is NOT the "never had their CUDA GPU changed here" sentence, which claims the opposite
        CHECK(said.find(L"never had their CUDA GPU changed here") == std::wstring::npos);
        CHECK(cd::FormatCudaRemoveLines(cd::CudaResultText()).empty());
    }
}

// ===========================================================================
// ROUND 6. The one idea: a COUNT is not an IDENTITY, and round 4 already proved it for APPLICATIONS.
// Round 5 made the same mistake one level down for SETTINGS, and a third time for CAPABILITY - a rule
// written where the tests reach, against a seam the .cpp always filled.
// ===========================================================================

// 🔴 R6-1. The delete decision is taken on WHICH settings an entry holds, never on how many.
void Test_AK26_SettingsAreIdentifiedNotCounted() {
    Case("AK26 🔴 R6-1: JudgeCudaSettings rules on the ids, and ZERO is not a safe answer");
    {
        std::vector<unsigned long> ids;
        // an EMPTY enumeration while the read just returned our value: the value is inherited
        CHECK(cd::JudgeCudaSettings(ids) == cd::CudaSettingsVerdict::NotOurs);
        ids.push_back(cd::CudaSettingId());
        CHECK(cd::JudgeCudaSettings(ids) == cd::CudaSettingsVerdict::OnlyOurs);
        ids.push_back(0x20D0F3E6ul);
        CHECK(cd::JudgeCudaSettings(ids) == cd::CudaSettingsVerdict::AlsoOthers);
        // 🔴 ONE SETTING, AND IT IS NOT OURS - the exact shape `numSettings == 1` read as "it is mine"
        std::vector<unsigned long> theirs;
        theirs.push_back(0x20D0F3E6ul);
        CHECK(cd::JudgeCudaSettings(theirs) == cd::CudaSettingsVerdict::NotOurs);
        // and the id itself is the one NVIDIA documents
        CHECK_EQ((unsigned int)cd::CudaSettingId(), 0x10354FF8u);
    }

    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const cd::CudaRecordRow line = cudatest::Line(ours, entry, cudatest::kId5090);

    Case("AK26b 🔴 ONE setting that is NOT ours: the entry is never deleted, whatever the read answered");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;   // what NvAPI_DRS_GetSetting answers
        f.inherited.insert(ours);               // ... from NVIDIA's general settings, not this entry
        f.otherSettings[ours] = 1;              // the ONE setting it really holds is the user's
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::SettingNotOurs);
        CHECK(!r.profileDeleted);
        CHECK(!r.settingCleared);
        CHECK_EQ(f.deletes, 0);                          // 🔴 never even asked
        CHECK(f.written.empty());                        // and no clear either: there is nothing to clear
        CHECK_EQ(f.saves, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
        CHECK_EQ(f.otherSettings[ours], (size_t)1);
        CHECK(!r.sessionDirty);
    }

    Case("AK26c ZERO of its own settings while the read returns our value is the same contradiction");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.inherited.insert(ours);               // and nothing else on it at all
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.refusal == cd::CudaRefusal::SettingNotOurs);
        CHECK_EQ(f.deletes, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
        CHECK(!cd::CudaRefusalReason(cd::CudaRefusal::SettingNotOurs).empty());
    }

    Case("AK26d an enumeration the driver REFUSED is never read as 'it holds none'");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.settingsListFails = true;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::SettingsUnknown);
        CHECK_EQ(f.deletes, 0);
        CHECK(f.written.empty());
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
        CHECK(!cd::CudaRefusalReason(cd::CudaRefusal::SettingsUnknown).empty());
    }

    Case("AK26e 🔴 R6-2: and a driver that does not OFFER the enumeration answers the same way");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.noEnumSettings = true;
        const cd::CudaOps ops = cudatest::OpsOf(f);
        CHECK(!ops.listSettingIds);            // 🔴 the operation is EMPTY, not a lambda that cannot work
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, ops);
        CHECK(r.refusal == cd::CudaRefusal::SettingsUnknown);
        CHECK_EQ(f.deletes, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
    }

    Case("AK26f the two answers that DO act still act, and the driver guard agrees with the decision");
    {
        cudatest::Fake f;                                  // exactly ours: the entry goes whole
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.profileDeleted);
        CHECK(f.profiles.empty());
    }
    {
        cudatest::Fake f;                                  // ours AND the user's: only ours comes out
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 2;
        const cd::CudaRowResult r = cd::RestoreCudaForRow(exe, line, cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.settingCleared);
        CHECK(!r.profileDeleted);
        CHECK_EQ(f.deletes, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
    }
    {
        // 🔴 AND THE DRIVER WRAPPER REFUSES IT TOO, so a caller that asked anyway would still be told no.
        // This is the guard in gpu_cuda.cpp's DeleteOwnProfile, mirrored by the fake above.
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.settings[ours] = cudatest::kId5090;
        f.otherSettings[ours] = 1;
        const cd::CudaOps ops = cudatest::OpsOf(f);
        CHECK(!cd::DropCudaProfileWeMade(ours, entry, ops));
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
        CHECK_EQ(f.otherSettings[ours], (size_t)1);
    }
}

// 🔴 R6-2. The capability gate, which is the round-4 defect shape one more time: a rule written where the
// tests reach, against a seam MakeCudaOps always filled.
void Test_AK27_CapabilityIsHonestAtTheSeam() {
    Case("AK27 🔴 R6-2: taking an entry away needs BOTH calls, and one without the other is no capability");
    {
        cd::CudaDriverCalls c;
        CHECK(!cd::CudaCanDeleteProfile(c));
        c.deleteProfile = true;
        CHECK(!cd::CudaCanDeleteProfile(c));       // DeleteApplicationEx is still missing
        c.deleteProfile = false;
        c.deleteApplication = true;
        CHECK(!cd::CudaCanDeleteProfile(c));       // and DeleteProfile is
        c.deleteProfile = true;
        CHECK(cd::CudaCanDeleteProfile(c));
        CHECK(!cd::CudaCanListSettings(c));
        c.enumSettings = true;
        CHECK(cd::CudaCanListSettings(c));
    }

    Case("AK27b 🔴 the gate EMPTIES the operation, which is what MakeCudaOps used to fail to do");
    {
        // An ops with everything installed, exactly as MakeCudaOps writes it before the gate runs.
        cd::CudaOps ops;
        ops.deleteProfile = [](const std::wstring&, const std::wstring&) { return true; };
        ops.listSettingIds = [](const std::wstring&, std::vector<unsigned long>&) { return true; };
        cd::CudaDriverCalls none;
        cd::ApplyCudaDriverCalls(ops, none);
        CHECK(!ops.deleteProfile);                 // 🔴 the null PlanCudaForRow refuses NoWayToUndo on
        CHECK(!ops.listSettingIds);                // 🔴 and the null RestoreCudaForRow refuses on
    }
    {
        cd::CudaOps ops;
        ops.deleteProfile = [](const std::wstring&, const std::wstring&) { return true; };
        ops.listSettingIds = [](const std::wstring&, std::vector<unsigned long>&) { return true; };
        cd::CudaDriverCalls all;
        all.deleteProfile = true;
        all.deleteApplication = true;
        all.enumSettings = true;
        cd::ApplyCudaDriverCalls(ops, all);
        CHECK(!!ops.deleteProfile);                // a driver that has them keeps them
        CHECK(!!ops.listSettingIds);
    }
    {
        // half a delete capability is no delete capability, and the gate is what says so
        cd::CudaOps ops;
        ops.deleteProfile = [](const std::wstring&, const std::wstring&) { return true; };
        cd::CudaDriverCalls half;
        half.deleteProfile = true;                 // DeleteApplicationEx did not resolve
        cd::ApplyCudaDriverCalls(ops, half);
        CHECK(!ops.deleteProfile);
    }

    Case("AK27c and the refusal the gate makes REACHABLE: no delete calls means no entry is ever made");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.noDelete = true;
        const cd::CudaOps ops = cudatest::OpsOf(f);
        CHECK(!ops.deleteProfile);                 // 🔴 the gate ran, not an `if` of the test's own
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), ops, rec);
        CHECK(r.refusal == cd::CudaRefusal::NoWayToUndo);
        CHECK_EQ(f.creates, 0);
        CHECK(f.profiles.empty());
    }

    Case("AK27d 🔴 pre-publish review: no settings enumeration means no entry is ever made either");
    {
        // Remove refuses an entry whose settings it cannot list (AK26e), so Apply must not make one it could
        // never take back. Refused in the PLAN, exactly where the missing delete calls are refused.
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.noEnumSettings = true;
        const cd::CudaOps ops = cudatest::OpsOf(f);
        CHECK(!ops.listSettingIds);                // 🔴 the gate ran, not an `if` of the test's own
        CHECK(!!ops.deleteProfile);                // and the delete calls ARE there: this is the other refusal
        const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
        cudatest::Rec rec;
        const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
        CHECK(!p.act);
        CHECK(p.refusal == cd::CudaRefusal::NoWayToListSettings);
        const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NoWayToListSettings);
        CHECK_EQ(f.creates, 0);
        CHECK(f.profiles.empty());
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK(rec.record.rows.empty());
        const std::wstring said = cd::CudaRefusalReason(cd::CudaRefusal::NoWayToListSettings);
        CHECK(said.find(L"list which settings") != std::wstring::npos);
        CHECK(said != cd::CudaRefusalReason(cd::CudaRefusal::NoWayToUndo));   // it names ITS call, not the other
    }

    Case("AK27e 🔴 and an entry of ours that ALREADY EXISTS is refused too: Remove could not undo that write either");
    {
        // Unlike NoWayToUndo (AK25b's second case), where an existing entry's setting can still be cleared.
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        const std::wstring ours = cd::CudaProfileNameFor(exe);
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.noEnumSettings = true;
        cudatest::Own(f, exe, ours, cd::CudaAppKeyFor(exe));
        f.settings[ours] = cudatest::kId4090;
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId4090));
        const cd::CudaOps ops = cudatest::OpsOf(f);
        CHECK(!ops.listSettingIds);
        CHECK(!!ops.deleteProfile);
        const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
        const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
        CHECK(!p.act);
        CHECK(p.refusal == cd::CudaRefusal::NoWayToListSettings);
        const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NoWayToListSettings);
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK_EQ(f.settings[ours], std::wstring(cudatest::kId4090));
        CHECK(rec.record.rows.size() == 1 && rec.record.rows[0].lastWrote == cudatest::kId4090);
    }
}

// 🔴 R6-5. The save that follows a successful delete inside a rollback used to be thrown away.
void Test_AK28_TheRollbacksOwnSaveIsChecked() {
    Case("AK28 🔴 R6-5: a rollback whose save refuses is NOT an undo, and it stops the run");
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        // The row's own write and save land, the READ-BACK disagrees, so the row rolls back - and that
        // rollback has to reach the database too, which is the `onDisk` path R6-5 lives on.
        f.writeIgnored = true;      // the driver says yes and the value is not there
        // 🔴 THE THIRD SAVE IS THE ONE R6-5 IS ABOUT. The row's own save is the first; the rollback's
        // save of the cleared setting is the second, and both of those were already checked. The third is
        // the save that commits the DELETE of the profile this row made, and its answer was thrown away.
        f.failSaveNumber = 3;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotUndone);   // 🔴 not NotConfirmed: the undo did not finish
        CHECK(!r.undone);
        CHECK(r.unresolved);        // 🔴 which is what stops the whole run
        CHECK(r.sessionDirty);      // 🔴 and what stops any later row from committing it
    }

    Case("AK28b and when that save WORKS the undo is an undo, and the run carries on");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.writeIgnored = true;
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NotConfirmed);
        CHECK(r.undone);
        CHECK(!r.unresolved);
        CHECK(!r.sessionDirty);
        CHECK(f.profiles.empty());
    }
}

// 🔴 R6-6. Completeness is unique entries, never raw rows.
void Test_AK29_MembershipIsJudgedOnUniqueEntries() {
    Case("AK29 🔴 R6-6: a duplicated row can NEVER satisfy 'we saw them all'");
    {
        std::vector<std::wstring> listed;
        listed.push_back(L"c:/a/one.exe");
        listed.push_back(L"c:/a/one.exe");           // the driver handed one entry back twice
        const cd::CudaMembership m = cd::JudgeCudaMembership(listed, 2, std::wstring());
        CHECK(!m.complete);                          // 🔴 the second member was never listed at all
        CHECK_EQ(m.otherApps, (size_t)1);
    }
    {
        // and without case, exactly as every other entry comparison in this feature
        std::vector<std::wstring> listed;
        listed.push_back(L"c:/a/one.exe");
        listed.push_back(L"C:/A/ONE.EXE");
        CHECK(!cd::JudgeCudaMembership(listed, 2, std::wstring()).complete);
    }

    Case("AK29b an EMPTY application name is 'cannot judge', never a row to step over");
    {
        std::vector<std::wstring> listed;
        listed.push_back(L"c:/a/one.exe");
        listed.push_back(std::wstring());
        const cd::CudaMembership m = cd::JudgeCudaMembership(listed, 2, std::wstring());
        CHECK(!m.complete);
        CHECK_EQ(m.otherApps, (size_t)1);            // and it names nobody it did not see
    }

    Case("AK29c the ordinary answers still answer, so this refuses nothing it should not");
    {
        std::vector<std::wstring> one;
        one.push_back(L"c:/a/one.exe");
        const cd::CudaMembership ours = cd::JudgeCudaMembership(one, 1, L"c:/a/one.exe");
        CHECK(ours.complete);
        CHECK_EQ(ours.otherApps, (size_t)0);         // the one entry on it is the one we asked about
        const cd::CudaMembership theirs = cd::JudgeCudaMembership(one, 1, L"c:/a/two.exe");
        CHECK(theirs.complete);
        CHECK_EQ(theirs.otherApps, (size_t)1);
        // 🔴 AN EMPTY PROFILE IS COMPLETE, and it has to be: the clean-up after a half-finished create
        // (E7) hands the delete guard exactly that.
        const cd::CudaMembership empty = cd::JudgeCudaMembership(std::vector<std::wstring>(), 0, L"c:/a/x.exe");
        CHECK(empty.complete);
        CHECK_EQ(empty.otherApps, (size_t)0);
    }

    Case("AK29d fewer unique entries than the driver's own count is INCOMPLETE");
    {
        std::vector<std::wstring> listed;
        listed.push_back(L"c:/a/one.exe");
        CHECK(!cd::JudgeCudaMembership(listed, 2, std::wstring()).complete);   // it said there were two
        // and a page exactly as long as the count is complete - the arithmetic, not the loop's exit
        std::vector<std::wstring> eight;
        for (int i = 0; i < 8; ++i) eight.push_back(L"c:/a/" + std::to_wstring(i) + L".exe");
        CHECK(cd::JudgeCudaMembership(eight, 8, std::wstring()).complete);
        CHECK_EQ(cd::JudgeCudaMembership(eight, 8, std::wstring()).otherApps, (size_t)8);
    }
}

// 🔴 R6-7. The clear-only restore keeps its line, so the entry is not stranded.
void Test_AK30_TheClearOnlyRestoreKeepsItsLine() {
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);

    Case("AK30 🔴 R6-7: a line saying 'the entry is ours and holds nothing of ours' is a good line");
    {
        cd::CudaRecordRow r = cudatest::Line(ours, entry, cd::CudaNothingWritten());
        CHECK(cd::CudaRowHoldsNothingOfOurs(r));
        CHECK(cd::CudaRecordRowIsUsable(r));         // it is never handed to the driver, so it need not be legal
        CHECK(!cd::CudaValueIsLegal(cd::CudaNothingWritten()));   // 🔴 and it never can be
        CHECK(cd::CudaRecordRowIsWritable(r));       // it survives one line of a tab-separated record
        // and it round-trips through the file, so a later run reads it back
        std::vector<cd::CudaRecordRow> rows;
        rows.push_back(r);
        std::vector<cd::CudaRecordRow> back;
        CHECK(cd::ParseCudaRecordFile(cd::FormatCudaRecordFile(rows), back));
        CHECK(back.size() == 1 && cd::CudaRowHoldsNothingOfOurs(back[0]));
        // a line carrying a real value is not one of these
        CHECK(!cd::CudaRowHoldsNothingOfOurs(cudatest::Line(ours, entry, cudatest::kId5090)));
    }

    Case("AK30b Remove meets one: nothing taken away, nothing failed, and the LINE IS KEPT");
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, ours, entry);
        f.otherSettings[ours] = 1;                   // the user's own setting is why it is still standing
        const cd::CudaRowResult r =
            cd::RestoreCudaForRow(exe, cudatest::Line(ours, entry, cd::CudaNothingWritten()), cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::NothingOfOurs);
        CHECK(r.refusal == cd::CudaRefusal::None);   // 🔴 it is not a failure and must never read as one
        CHECK(!r.profileDeleted);
        CHECK(!r.settingCleared);
        CHECK(!r.sessionDirty);
        CHECK_EQ(f.deletes, 0);
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1);
    }

    Case("AK30c but an entry that has GONE still drops its line, and one that changed is still refused");
    {
        cudatest::Fake f;                            // nothing in the driver at all
        const cd::CudaRowResult r =
            cd::RestoreCudaForRow(exe, cudatest::Line(ours, entry, cd::CudaNothingWritten()), cudatest::OpsOf(f));
        CHECK(r.outcome == cd::CudaOutcome::Absent); // the line is spent by the caller, as it always was
    }
    {
        cudatest::Fake f;
        cudatest::Own(f, exe, L"Google Chrome", L"chat.exe", true, 0);
        const cd::CudaRowResult r =
            cd::RestoreCudaForRow(exe, cudatest::Line(ours, entry, cd::CudaNothingWritten()), cudatest::OpsOf(f));
        CHECK(r.refusal == cd::CudaRefusal::RecordedProfileChanged);
    }

    Case("AK30d 🔴 AND A LATER APPLY OWNS THAT ENTRY AGAIN - which is the whole reason the line is kept");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::Own(f, exe, ours, entry);
        f.otherSettings[ours] = 1;
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cd::CudaNothingWritten()));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK_EQ(f.settings[ours], std::wstring(cudatest::kId5090));
        CHECK_EQ(f.creates, 0);                      // nothing was made: the entry was already there
        CHECK(rec.record.rows.size() == 1 && rec.record.rows[0].lastWrote == cudatest::kId5090);
    }

    Case("AK30e the sentence for it is true, and is not the 'we never touched this' one");
    {
        cd::CudaResultText t;
        t.nothingOfOurs = 1;
        const std::wstring one = cd::FormatCudaRemoveLines(t);
        CHECK(one.find(L"already had nothing of Game Optimizer's in its NVIDIA settings entry") !=
              std::wstring::npos);
        CHECK(one.find(L"never had their CUDA GPU changed here") == std::wstring::npos);
        CHECK(one.find(L"was not put back for") == std::wstring::npos);
        cd::CudaResultText many;
        many.nothingOfOurs = 3;
        CHECK(cd::FormatCudaRemoveLines(many).find(L"those entries were left alone.") != std::wstring::npos);
        // and it is silent when there are none
        cd::CudaResultText plain;
        plain.changed = 1;
        CHECK(cd::FormatCudaRemoveLines(plain).find(L"already had nothing of Game Optimizer's") ==
              std::wstring::npos);
    }

    Case("AK30f 🔴 R6-10: the record file's own header describes BOTH restores, not just the deletion");
    {
        const std::wstring head = cd::FormatCudaRecordHeader();
        CHECK(head.find(L"takes the whole\r\n; entry away") == std::wstring::npos);   // the old, half-true line
        CHECK(head.find(L"ONE OF TWO WAYS") != std::wstring::npos);
        CHECK(head.find(L"only Game Optimizer's own CUDA setting is") != std::wstring::npos);
        CHECK(head.find(L"\"-\" in place of") != std::wstring::npos);
        CHECK(head.find(cd::CudaRecordFileName()) != std::wstring::npos);
    }
}

// 🔴 R6-8, R6-9 and R6-11: the adopt path leaves nothing behind, and two sentences that were wrong.
void Test_AK31_TheAdoptPathAndTwoWrongSentences() {
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);

    Case("AK31 🔴 R6-8: a read this cannot use refuses the row with NOTHING added to the driver");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);                // the empty entry an earlier run left behind
        f.readFails = true;                          // and the driver will not say what is in it
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cudatest::kId4090));   // a line claims it
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK_EQ(r.profileName, ours);               // and it says WHICH entry
        // 🔴 THE POINT: the application entry was never put in. It used to be added first and then
        // abandoned, leaving NVIDIA resolving this executable to an entry nothing recorded.
        CHECK_EQ(f.creates, 0);
        CHECK_EQ(f.byExe.count(cd::ToLower(exe)), (size_t)0);
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK_EQ(f.profiles.count(ours), (size_t)1); // the empty entry is left exactly as it was
    }
    {
        // a value that is not one the driver could have written is the same answer
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.settings[ours] = L"autoselect";
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cudatest::kId4090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK_EQ(f.creates, 0);
        CHECK_EQ(f.byExe.count(cd::ToLower(exe)), (size_t)0);
    }
    {
        // and a read that CAN be used still carries the previous value into the row's own undo
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.settings[ours] = cudatest::kId4090;        // an earlier run of ours left this in it
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cudatest::kId4090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(r.hadPrevious);
        CHECK_EQ(r.previousValue, std::wstring(cudatest::kId4090));
        CHECK_EQ(f.creates, 1);
    }

    Case("AK31b 🔴 R6-9: -167's sentence is GIVEN the count it measured, in three forms");
    {
        CHECK(cd::FormatCudaNameInUseLine(L"Some Other Game", 0).find(L"other program") == std::wstring::npos);
        CHECK(cd::FormatCudaNameInUseLine(L"Some Other Game", 0).find(L"\"Some Other Game\"") !=
              std::wstring::npos);
        CHECK(cd::FormatCudaNameInUseLine(L"Some Other Game", 1).find(L"covers 1 other program as well") !=
              std::wstring::npos);
        CHECK(cd::FormatCudaNameInUseLine(L"Some Other Game", 4).find(L"covers 4 other programs as well") !=
              std::wstring::npos);
        // every form still sends the user somewhere they can act
        CHECK(cd::FormatCudaNameInUseLine(L"X", 2).find(L"NVIDIA Control Panel") != std::wstring::npos);
    }
    {
        // and the row really carries it through: the base-name lookup found two other programs
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.createAnswer = cd::CudaCreate::AlreadyInUse;
        cudatest::Own(f, L"chat.exe", L"Some Other Game", L"chat.exe", false, 2);
        cudatest::Rec rec;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NameAlreadyInUse);
        CHECK_EQ(r.otherApps, (size_t)2);
        CHECK(cd::CudaRowRefusalText(r).find(L"covers 2 other programs as well") != std::wstring::npos);
    }

    Case("AK31c 🔴 R6-11: an adopt that loses its race is NOT 'we have no record of making it'");
    {
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.adoptRaceLost = true;                      // a member arrived between the lookup and the create
        f.adoptRaceOtherApps = 2;
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cudatest::kId4090));   // a line DOES claim it
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NvidiaManagesIt);
        CHECK_EQ(r.otherApps, (size_t)2);
        const std::wstring says = cd::CudaRowRefusalText(r);
        CHECK(says.find(L"covers 2 other programs as well") != std::wstring::npos);
        // 🔴 the false sentence is gone: a record line claims this entry, so this is not one we never made
        CHECK(says.find(L"has no record of making") == std::wstring::npos);
        CHECK(f.written.empty());
    }
    {
        // and when that second guard is the thing that could not be measured, no count is claimed
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.adoptRaceLost = true;
        f.adoptRaceIncomplete = true;
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, entry, cudatest::kId4090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::MembershipUnknown);
        CHECK_EQ(r.otherApps, (size_t)0);
    }
    {
        // 🔴 AND THE OTHER HALF STILL STANDS: with NO line of ours, the name really is taken by something
        // this product has no record of making, and that sentence is the true one.
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        cudatest::Rec rec;                           // no line at all
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::ProfileNameTaken);
        CHECK(cd::CudaRefusalReason(cd::CudaRefusal::ProfileNameTaken).find(L"no record of making") !=
              std::wstring::npos);
    }
}

void Test_AK25_TheRecordAndTheDriverGuards() {
    Case("AK25 🔴 R5-8: two lines for one application make the record UNREADABLE, not 'the first wins'");
    const std::wstring magic = cd::CudaRecordMagic() + L"\t" + cd::CudaRecordVersion() + L"\r\n";
    std::vector<cd::CudaRecordRow> out;
    CHECK(!cd::ParseCudaRecordFile(magic + L"P\tc:/a/chat.exe\tnone\tt\r\nQ\tc:/a/chat.exe\tnone\tu\r\n", out));
    CHECK(out.empty());
    // without case, exactly as CudaLineFor matches an entry
    CHECK(!cd::ParseCudaRecordFile(magic + L"P\tc:/a/chat.exe\tnone\tt\r\nP\tC:/A/CHAT.EXE\tnone\tu\r\n", out));
    CHECK(out.empty());
    // and two lines for two DIFFERENT applications are an ordinary record
    CHECK(cd::ParseCudaRecordFile(magic + L"P\tc:/a/chat.exe\tnone\tt\r\nQ\tc:/b/chat.exe\tnone\tu\r\n", out));
    CHECK_EQ(out.size(), (size_t)2);

    Case("AK25b 🔴 R5-9: a driver that cannot take a profile away again makes none in the first place");
    {
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.noDelete = true;                  // NvAPI_DRS_DeleteProfile is not offered by this driver
        const cd::CudaOps ops = cudatest::OpsOf(f);
        const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids);
        cudatest::Rec rec;
        const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, ops, rec.record);
        CHECK(!p.act);                      // 🔴 refused in the PLAN, before anything can be created
        CHECK(p.refusal == cd::CudaRefusal::NoWayToUndo);
        const cd::CudaRowResult r = cudatest::Apply(exe, target, ops, rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NoWayToUndo);
        CHECK_EQ(f.creates, 0);
        CHECK(f.profiles.empty());
        CHECK(f.written.empty());
        CHECK_EQ(f.saves, 0);
        CHECK(rec.record.rows.empty());
        CHECK(!cd::CudaRefusalReason(cd::CudaRefusal::NoWayToUndo).empty());
    }
    {
        // but an entry of ours that ALREADY exists is only written to, never created, so it still works
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        const std::wstring ours = cd::CudaProfileNameFor(exe);
        cudatest::Fake f;
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        f.noDelete = true;
        cudatest::Own(f, exe, ours, cd::CudaAppKeyFor(exe));
        cudatest::Rec rec;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId4090));
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Written);
    }

    Case("AK25c 🔴 R5-10: every profile-name comparison is without case");
    {
        CHECK(cd::IsCudaProfileWeMade(L"Game Optimizer - C:\\a\\b.exe"));
        CHECK(cd::IsCudaProfileWeMade(L"GAME OPTIMIZER - C:\\A\\B.EXE"));
        CHECK(cd::IsCudaProfileWeMade(L"game optimizer - c:/a/b.exe"));
        CHECK(!cd::IsCudaProfileWeMade(L"Game Optimizer"));            // the stem alone is not one
        CHECK(!cd::IsCudaProfileWeMade(L"Google Chrome"));
        CHECK(!cd::IsCudaProfileWeMade(std::wstring()));
        // and the ownership ruling agrees with a driver that answers in another case
        const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
        cudatest::Fake f;
        cudatest::Own(f, exe, L"GAME OPTIMIZER - C:\\APPS\\CHAT\\CHAT.EXE", cd::CudaAppKeyFor(exe));
        const cd::CudaResolved res = cd::ResolveCudaProfile(exe, cudatest::OpsOf(f));
        CHECK(cd::CudaOwnershipRefusal(res, exe, cd::CudaProfileNameFor(exe)) == cd::CudaRefusal::None);
    }
}

// 🔴 R6-12: THE ADOPT LOOKUP ASKS FOR THE NAME THE RECORD STORES, BECAUSE THE DRIVER'S LOOKUP HAS CASE.
//
// [M] probe S23, against the real driver: NvAPI_DRS_FindProfileByName is CASE-SENSITIVE - the exact name
// answers Found, and the same name in UPPER CASE or lower case answers Absent with an empty name back.
// Every ownership comparison in this product is IEquals and STAYS that way (R5-10), so an executable path
// Windows hands back in another case still reads as OURS - while a profile name RECOMPUTED from that path
// misses the entry in the driver. The adopt path used to recompute it, so it made a SECOND entry differing
// from the first only in case: an entry no record line claims, which CudaOwnershipRefusal then answers
// "NVIDIA manages this one" to forever. That is the very trap R6-7 was built to close, arriving through
// another door.
//
// `Fake::profiles` is a std::set<std::wstring> and `Fake::settings` a std::map keyed the same way, so both
// lookups have case exactly as the driver's do - which is what makes this test passable only by asking for
// the recorded spelling.
void Test_AK32_TheAdoptLookupUsesTheRecordedName() {
    Case("AK32 🔴 R6-12: a line claims the entry, so the lookup uses ITS spelling, not today's path's");
    const std::wstring made = L"C:\\Apps\\chat\\chat.exe";    // how the entry was made, and how it is named
    const std::wstring today = L"C:\\APPS\\CHAT\\CHAT.EXE";   // the same file, as Windows hands it back now
    const std::wstring ours = cd::CudaProfileNameFor(made);
    // The two halves of the defect: the line is still matched to this row - the application entry is the
    // lower-case path either way - while the profile NAMES differ, by letter case alone.
    CHECK_EQ(cd::CudaAppKeyFor(today), cd::CudaAppKeyFor(made));
    CHECK_NE(cd::CudaProfileNameFor(today), ours);
    cudatest::Fake n;
    n.gpus = cudatest::TwoCards();
    n.ids = cudatest::BothIds();
    cudatest::LeftBehind(n, made);            // the entry stands, spelled as it was MADE, covering nobody
    n.settings[ours] = cudatest::kId4090;     // and holding what an earlier run of ours left in it
    cudatest::Rec rec;
    rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(made), cudatest::kId4090));
    const cd::CudaRowResult r =
        cudatest::Apply(today, cd::CudaTargetFor(cudatest::kKey4090, n.gpus, n.ids), cudatest::OpsOf(n), rec);
    CHECK(r.outcome == cd::CudaOutcome::Written);
    CHECK_EQ(r.profileName, ours);                              // the RECORDED spelling, never today's
    // 🔴 THE ENTRY THAT WAS ALREADY THERE IS THE ONE THAT WAS READ AND WRITTEN. A lookup by the recomputed
    // name would have answered Absent about both, so neither of these could be true by accident.
    CHECK(r.hadPrevious);
    CHECK_EQ(r.previousValue, std::wstring(cudatest::kId4090));
    CHECK_EQ(n.settings[ours], std::wstring(cudatest::kId5090));
    // 🔴 AND NO SECOND ENTRY DIFFERING ONLY IN CASE - which is the whole defect.
    CHECK_EQ(n.profiles.size(), (size_t)1);
    CHECK_EQ(n.profiles.count(ours), (size_t)1);
    CHECK_EQ(n.profiles.count(cd::CudaProfileNameFor(today)), (size_t)0);
    CHECK_EQ(n.creates, 1);                                     // asked once, and it ADOPTED
    CHECK(!r.profileDeleted);
    CHECK_EQ(rec.record.rows.size(), (size_t)1);                // still one line, still naming that entry
    CHECK_EQ(rec.record.rows[0].profileName, ours);
    CHECK_EQ(rec.record.rows[0].lastWrote, std::wstring(cudatest::kId5090));

    Case("AK32b where NO line claims it the recomputed name is still the only one there is");
    {
        cudatest::Fake m;
        m.gpus = cudatest::TwoCards();
        m.ids = cudatest::BothIds();
        cudatest::Rec none;
        const cd::CudaRowResult s = cudatest::Apply(
            today, cd::CudaTargetFor(cudatest::kKey4090, m.gpus, m.ids), cudatest::OpsOf(m), none);
        CHECK(s.outcome == cd::CudaOutcome::Written);
        CHECK_EQ(s.profileName, cd::CudaProfileNameFor(today));   // today's spelling: nothing else to use
        CHECK_EQ(m.profiles.count(cd::CudaProfileNameFor(today)), (size_t)1);
        CHECK_EQ(m.profiles.size(), (size_t)1);
    }
    {
        // 🔴 AND A LINE NAMING AN ENTRY THE DRIVER DOES NOT HOLD ADOPTS NOTHING. The lookup asks for
        // exactly what the line says; Absent is Absent, so one is made under today's name and no entry of
        // anybody else's is joined on the strength of a name that merely resembles it.
        cudatest::Fake m;
        m.gpus = cudatest::TwoCards();
        m.ids = cudatest::BothIds();
        cudatest::Rec stale;
        stale.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(made), cudatest::kId4090));
        const cd::CudaRowResult s = cudatest::Apply(
            today, cd::CudaTargetFor(cudatest::kKey4090, m.gpus, m.ids), cudatest::OpsOf(m), stale);
        CHECK(s.outcome == cd::CudaOutcome::Written);
        CHECK_EQ(s.profileName, cd::CudaProfileNameFor(today));
        CHECK_EQ(m.profiles.size(), (size_t)1);
        CHECK_EQ(m.creates, 1);
    }
}

// 🔴 Pre-publish review of v0.5.9: a driver that says a page holds more entries than the buffer it was
// handed. Both of gpu_cuda.cpp's readers (OtherAppCountOf, SettingIdsOf) walk the driver's pages through
// WalkCudaPages, so this drives the walk they really run, with a scripted driver in place of NVAPI.
void Test_AK33_AnOverReportedPageIsAFailedEnumeration() {
    struct Run {
        cd::CudaWalk walk = cd::CudaWalk::Refused;
        std::vector<unsigned long> starts;   // every `start` the walk asked the driver for
        size_t taken = 0;                    // entries it copied out of the pages
        bool outOfPage = false;              // an entry index at or past the page size was copied
    };
    // `script` is what the driver answers, one request at a time; past its end every page is full.
    const auto walk = [](unsigned long pageSize, unsigned long cap,
                         const std::vector<std::pair<cd::CudaPage, unsigned long> >& script) {
        Run run;
        size_t next = 0;
        run.walk = cd::WalkCudaPages(
            pageSize, cap,
            [&](unsigned long start, unsigned long& count) {
                run.starts.push_back(start);
                if (next >= script.size()) {
                    count = pageSize;
                    return cd::CudaPage::Filled;
                }
                count = script[next].second;
                return script[next++].first;
            },
            [&](unsigned long k) {
                ++run.taken;
                if (k >= pageSize) run.outOfPage = true;
            });
        return run;
    };
    typedef std::pair<cd::CudaPage, unsigned long> P;
    const cd::CudaPage F = cd::CudaPage::Filled;

    Case("AK33 🔴 a page the driver says is longer than its buffer fails the walk, before anything is copied");
    {
        const Run r = walk(4, 256, {P(F, 5)});
        CHECK(r.walk == cd::CudaWalk::Overran);
        CHECK_EQ(r.taken, (size_t)0);            // nothing copied out of the over-reported page
        CHECK_EQ(r.starts.size(), (size_t)1);    // and the walk did not advance past it
    }
    {
        // on a LATER page too: the first page is kept, and the answer is still a failure, not a short list
        const Run r = walk(4, 256, {P(F, 4), P(F, 9)});
        CHECK(r.walk == cd::CudaWalk::Overran);
        CHECK_EQ(r.taken, (size_t)4);
        CHECK_EQ(r.starts.size(), (size_t)2);
        CHECK(!r.outOfPage);
    }

    Case("AK33b the walks that DO end well still end well, and no copy ever leaves the page");
    {
        const Run r = walk(4, 256, {P(F, 4), P(F, 2)});            // a short page ends it
        CHECK(r.walk == cd::CudaWalk::Complete);
        CHECK_EQ(r.taken, (size_t)6);
        CHECK(r.starts.size() == 2 && r.starts[0] == 0 && r.starts[1] == 4);
    }
    {
        const Run r = walk(4, 256, {P(F, 4), P(cd::CudaPage::End, 0)});   // "that is all of them"
        CHECK(r.walk == cd::CudaWalk::Complete);
        CHECK_EQ(r.taken, (size_t)4);
    }
    {
        const Run r = walk(4, 256, {P(F, 4), P(F, 0)});            // an empty page ends it too
        CHECK(r.walk == cd::CudaWalk::Complete);
        CHECK_EQ(r.taken, (size_t)4);
    }
    {
        const Run r = walk(4, 256, {P(F, 4), P(cd::CudaPage::Refused, 4)});
        CHECK(r.walk == cd::CudaWalk::Refused);                     // never read as the end of the list
    }
    {
        const Run r = walk(4, 12, {});                              // full pages to the cap
        CHECK(r.walk == cd::CudaWalk::Capped);
        CHECK_EQ(r.taken, (size_t)12);
        CHECK(r.starts.size() == 3 && r.starts[2] == 8);
        CHECK(!r.outOfPage);
    }
}

// 🔴 Pre-publish review of v0.5.9: the adopt path took an INHERITED value for the entry's previous one.
// NvAPI_DRS_GetSetting answers from NVIDIA's general settings when the entry has none of its own, so a failed
// Apply's rollback wrote that value into the entry as a local setting that had never existed.
void Test_AK34_TheAdoptedPreviousValueIsTheEntrysOwn() {
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    // An entry of ours an earlier run left behind, claimed by the record, reading back kId4090; the Apply writes
    // kId5090 and its own save refuses, so the row is rolled back.
    const auto setUp = [&](cudatest::Fake& f, cudatest::Rec& rec) {
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::LeftBehind(f, exe);
        f.settings[ours] = cudatest::kId4090;
        rec.record.rows.push_back(cudatest::Line(ours, cd::CudaAppKeyFor(exe), cudatest::kId4090));
    };
    const auto wrote = [](const cudatest::Fake& f, const std::wstring& what) {
        for (size_t i = 0; i < f.written.size(); ++i)
            if (f.written[i] == what) return true;
        return false;
    };

    Case("AK34 🔴 an inherited value is no previous value: the rollback CLEARS, it never writes it in");
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec);
        f.inherited.insert(ours);             // the read answers, and the entry's own ids do not list it
        f.failSaveNumber = 1;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Refused);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.undone);
        CHECK(!r.hadPrevious);
        CHECK(!wrote(f, ours + L"=" + cudatest::kId4090));   // 🔴 the inherited value is never written as local
        CHECK(!f.written.empty() && f.written.back() == ours + L"=CLEAR");
    }
    {
        // control: a value that IS the entry's own is still put back exactly
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec);
        f.failSaveNumber = 1;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.undone);
        CHECK(r.hadPrevious);
        CHECK_EQ(r.previousValue, std::wstring(cudatest::kId4090));
        CHECK(!f.written.empty() && f.written.back() == ours + L"=" + cudatest::kId4090);
    }

    Case("AK34b ids nobody could list, or ids that contradict the read, refuse BEFORE the entry is adopted");
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec);
        f.settingsListFails = true;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK_EQ(f.creates, 0);                   // nothing was added to the entry
        CHECK(f.written.empty());
        CHECK(f.byExe.empty());
    }
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec);                            // the ids list our setting ...
        cd::CudaOps ops = cudatest::OpsOf(f);
        ops.readSetting = [](const std::wstring&, std::wstring& v) {   // ... and the read says there is none
            v.clear();
            return cd::CudaRead::Absent;
        };
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), ops, rec);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK_EQ(f.creates, 0);
        CHECK(f.written.empty());
    }
}

// 🔴 Pre-publish review of v0.5.9, the sibling path: PlanCudaForRow decided "already set" and "previous value"
// from the read alone. One rule now decides it for both paths, from the entry's own setting ids.
void Test_AK35_TheLocalSettingRuleIsShared() {
    const std::wstring exe = L"C:\\Apps\\chat\\chat.exe";
    const std::wstring ours = cd::CudaProfileNameFor(exe);
    const std::wstring entry = cd::CudaAppKeyFor(exe);
    const std::wstring v = cudatest::kId5090;
    std::vector<unsigned long> mine, others, none;
    mine.push_back(cd::CudaSettingId());
    mine.push_back(0x20D0F3E6ul);
    others.push_back(0x20D0F3E6ul);

    Case("AK35 JudgeCudaLocalSetting: Present, Absent and Undecidable");
    {
        CHECK(cd::JudgeCudaLocalSetting(true, mine, cd::CudaRead::Value, v) == cd::CudaLocalSetting::Present);
        // 🔴 a value the ids do not list is INHERITED, not the entry's own
        CHECK(cd::JudgeCudaLocalSetting(true, others, cd::CudaRead::Value, v) == cd::CudaLocalSetting::Absent);
        CHECK(cd::JudgeCudaLocalSetting(true, none, cd::CudaRead::Absent, L"") == cd::CudaLocalSetting::Absent);
        CHECK(cd::JudgeCudaLocalSetting(false, mine, cd::CudaRead::Value, v) == cd::CudaLocalSetting::Undecidable);
        CHECK(cd::JudgeCudaLocalSetting(true, mine, cd::CudaRead::Failed, L"") == cd::CudaLocalSetting::Undecidable);
        CHECK(cd::JudgeCudaLocalSetting(true, mine, cd::CudaRead::Absent, L"") == cd::CudaLocalSetting::Undecidable);
        CHECK(cd::JudgeCudaLocalSetting(true, others, cd::CudaRead::Value, L"autoselect") ==
              cd::CudaLocalSetting::Undecidable);
    }

    // An entry of ours an earlier clear-only Remove emptied (R6-7): the user's own setting keeps it standing,
    // it holds NO CUDA setting of its own, and the read answers with NVIDIA's general value.
    const auto setUp = [&](cudatest::Fake& f, cudatest::Rec& rec, const std::wstring& inheritedValue) {
        f.gpus = cudatest::TwoCards();
        f.ids = cudatest::BothIds();
        cudatest::Own(f, exe, ours, entry);
        f.otherSettings[ours] = 1;
        f.settings[ours] = inheritedValue;
        f.inherited.insert(ours);
        rec.record.rows.push_back(cudatest::Line(ours, entry, cd::CudaNothingWritten()));
    };

    Case("AK35b 🔴 an inherited value equal to the target is NOT 'already set': the entry's own setting is written");
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        const cd::CudaTarget target = cd::CudaTargetFor(cudatest::kKey4090, cudatest::TwoCards(), cudatest::BothIds());
        CHECK(target.act);
        setUp(f, rec, target.value);
        const cd::CudaRowPlan p = cd::PlanCudaForRow(exe, target, cudatest::OpsOf(f), rec.record);
        CHECK(p.act);
        CHECK(!p.alreadySet);
        CHECK(!p.hadPrevious);
        const cd::CudaRowResult r = cudatest::Apply(exe, target, cudatest::OpsOf(f), rec);
        CHECK(r.outcome == cd::CudaOutcome::Written);
        CHECK(f.written.size() == 1 && f.written[0] == ours + L"=" + target.value);
        CHECK(rec.record.rows.size() == 1 && rec.record.rows[0].lastWrote == target.value);
    }

    Case("AK35c and a failed Apply on it CLEARS on rollback, never writing the inherited value in as its own");
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec, cudatest::kId4090);
        f.failSaveNumber = 1;
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec);
        CHECK(r.refusal == cd::CudaRefusal::NotSaved);
        CHECK(r.undone);
        CHECK(!r.hadPrevious);
        CHECK(!f.written.empty() && f.written.back() == ours + L"=CLEAR");
    }

    Case("AK35d ids nobody could list, or ids naming our setting while the read says none, refuse on this path too");
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec, cudatest::kId4090);
        f.settingsListFails = true;
        const cd::CudaRowPlan p =
            cd::PlanCudaForRow(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), cudatest::OpsOf(f), rec.record);
        CHECK(!p.act);
        CHECK(p.refusal == cd::CudaRefusal::CouldNotRead);
    }
    {
        cudatest::Fake f;
        cudatest::Rec rec;
        setUp(f, rec, cudatest::kId4090);
        f.inherited.clear();                          // the ids list our setting ...
        cd::CudaOps ops = cudatest::OpsOf(f);
        ops.readSetting = [](const std::wstring&, std::wstring& got) {   // ... and the read says there is none
            got.clear();
            return cd::CudaRead::Absent;
        };
        const cd::CudaRowResult r =
            cudatest::Apply(exe, cd::CudaTargetFor(cudatest::kKey4090, f.gpus, f.ids), ops, rec);
        CHECK(r.refusal == cd::CudaRefusal::CouldNotRead);
        CHECK(f.written.empty());
    }
}

// ===========================================================================
// AL. The line a run shows while it walks its rows (v0.5.9)
// ===========================================================================
//
// Apply and Remove used to leave the tab silent for the whole run - per row a transacted registry
// write, a read-back, a whole-file rewrite of the .reg journal, and on an NVIDIA target a full
// driver-database save. This is the sentence that says where the run has got to.
void Test_AL1_TheRunProgressLine() {
    Case("AL1 the counter names the action and carries both numbers");
    CHECK_EQ(cd::FormatRunProgressLine(false, 7, 40, false), std::wstring(L"Assigning 7 of 40."));
    CHECK_EQ(cd::FormatRunProgressLine(true, 7, 40, false), std::wstring(L"Removing 7 of 40."));
    CHECK_EQ(cd::FormatRunProgressLine(false, 1, 1, false), std::wstring(L"Assigning 1 of 1."));
    CHECK_EQ(cd::FormatRunProgressLine(true, 1, 1, false), std::wstring(L"Removing 1 of 1."));

    Case("AL1b 🔴 founder decision 18: with no CUDA half the words NVIDIA and CUDA appear nowhere");
    const std::wstring off = cd::FormatRunProgressLine(false, 3, 9, false);
    CHECK(off.find(L"NVIDIA") == std::wstring::npos);
    CHECK(off.find(L"CUDA") == std::wstring::npos);
    const std::wstring offRemoving = cd::FormatRunProgressLine(true, 3, 9, false);
    CHECK(offRemoving.find(L"NVIDIA") == std::wstring::npos);
    CHECK(offRemoving.find(L"CUDA") == std::wstring::npos);

    Case("AL1c and with one, the same count carries the reason it is slow");
    const std::wstring on = cd::FormatRunProgressLine(false, 3, 9, true);
    CHECK_EQ(on.substr(0, off.size()), off);   // the count is untouched; the sentence is appended
    CHECK_NE(on, off);
    CHECK(on.find(L"NVIDIA's settings are saved once for each application, so this takes a moment.") !=
          std::wstring::npos);
    CHECK_EQ(cd::FormatRunProgressLine(true, 3, 9, true).find(L"Removing 3 of 9."), (size_t)0);
}

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
    Test_B8b_TheStructDefaultIsTheOneUsersInherit();
    Test_B8c_TheShippedProfileCarriesTheSameThreshold();
    Test_B8d_TheNewDefaultIsSelfConsistent();
    Test_B9_IsExcludedCaseInsensitive();
    Test_B10_LastUsedRoundTripsAndTheRetiredKeyIsNeverWritten();
    Test_B11_ProfilesForDisplay();
    Test_B12_ValidateAndRepairNewRules();
    Test_B12d_TheRetiredFlagCannotSurviveASaveThatSkipsTheRepair();
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
    Test_U1_StopBoxIsCheckedWhenTheServiceIsDisabled();
    Test_U2_StopBoxDependsOnNothingButTheServiceStartType();
    Test_X1_ServiceDisabledIsChecked();
    Test_X2_ServiceAutomaticIsUnchecked();
    Test_X3_ServiceManualIsUnchecked();
    Test_X4_UnreadableServiceIsUnchecked();
    Test_X5_RoundTripProperty();
    Test_Y1_AutostartExeFromQuotedCommandWithTray();
    Test_Y2_AutostartExeFromBareCommandWithTray();
    Test_Y3_AutostartExeEmptyAndUnclosedQuote();
    Test_Y4_SameExeDifferentCaseWithTray();
    Test_Y5_DifferentExePathsWithTray();
    Test_Y6_CurrentExeEmptyDifferentPaths();
    Test_Y7_FlaglessCommandSamePathsReturnsTrue();
    Test_U3_RestoreControlAppearsOnlyForUsersTheOldFeatureStranded();
    Test_U4_ParkedWarningNoLongerClaimsStoppingTheServiceIsUseless();
    Test_U5_StoppingDisablesAndClearingRestoresAmdsOwnDefault();
    Test_V1_RestoreDriverStartWithRecordedThree();
    Test_V2_RestoreDriverStartWithRecordedTwo();
    Test_V3_RestoreDriverStartWithNegativeOne();
    Test_V4_ServiceRestoreTypeIsAlwaysTwo();
    Test_V5_ServiceDisableTypeIsAlwaysFour();

    std::printf("\n== V. Naming a custom mask ==\n");
    Test_V1_EmptyAndWhitespaceOnlyNamesAreEmpty();
    Test_V2_CaseInsensitiveDuplicateOfExistingMask();
    Test_V3_NameDeriveMasksEmitsIsReserved();
    Test_V4_FreshNameIsAcceptedAndTrimmed();
    Test_V5_ProfilesReferencingMaskListsEachOnceInConfigOrder();
    Test_V6_TopologyChangedPreservedSentence();
    Test_V8_CanRemoveMaskOnlyForCustomMasks();
    Test_V9_DerivableNamesAreReservedEverywhere();
    Test_V10_AddMaskCaptionIsNotAValidMaskName();

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

    std::printf("\n== T. Per-warning startup preference ==\n");
    Test_T1_AgentRunningWithWarningEnabledShowsVCache();
    Test_T2_AgentRunningWithWarningSuppressedHidesVCache();
    Test_T3_AgentNotRunningWithWarningEnabledHidesVCache();
    Test_T4_VCacheSuppressionDoesNotSuppressGameMode();
    Test_T5_VCacheSuppressedAndGameModeOffDoesNotShow();
    Test_T6_VCacheWarningSuppressionRoundTrips();
    Test_T7_MissingVCacheWarningPreferenceDefaultsTrue();

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

    std::printf("\n== W. Interrupt readout - mask arithmetic and machine scope ==\n");
    Test_W1_IrqGoldenVector();
    Test_W2_IrqTranspositionIsAFailureInTheSuite();
    Test_W3_IrqDecoderLengths();
    Test_W4_IrqFormatCpuList();
    Test_W13_IrqRegistryReadGuards();
    Test_W5_IrqTemporalTargetSetDecode();
    Test_W6_IrqReferenceMachineIsInScope();
    Test_W7_IrqTwoGroupsRefused();
    Test_W8_IrqTooManyProcessorsRefused();
    Test_W9_IrqUnexpectedNumberingRefused();
    Test_W10_IrqBuildMaskWholeChain();
    Test_W11_IrqBuildMaskRefuses();
    Test_W12_IrqRefusalReasonsAreDistinct();

    std::printf("\n== X. Interrupt readout - the honesty of the readout ==\n");
    Test_X1_IrqTooFewSamplesIsNeverMeasuredAndNeverHot();
    Test_X2_IrqHotIsInterruptPlusDpc();
    Test_X3_IrqGroupMembershipIsExact();
    Test_X4_IrqHottestInGroupIgnoresTheRestOfTheMachine();
    Test_X5_IrqNothingMeasuredInTheGroup();
    Test_X6_IrqQuietCount();
    Test_X7_IrqReferenceMachineWarningFires();
    Test_X8_IrqAgreementCoversEveryState();
    Test_X9_IrqSwitchCompleteness();
    Test_X10_IrqWordingTripwire();
    Test_X11_IrqEveryStringIsNonEmpty();
    Test_X12_IrqPresentButUndecodablePolicyIsNeverAbsent();
    Test_X13_IrqUnmeasuredGroupMemberIsStillInTheGroup();

    std::printf("\n== Y. Interrupt readout - the sentences that carry numbers ==\n");
    Test_Y1_IrqHotCoreSentenceIsExact();
    Test_Y2_IrqNoHotCoreSentenceIsExact();
    Test_Y3_IrqOutsideGroupSentenceIsExact();
    Test_Y4_IrqNothingMeasuredSentencesAreExact();
    Test_Y5_IrqGameGroupSentenceIsExact();
    Test_Y6_IrqReproductionCommandIsExact();
    Test_Y7_IrqPolicyStateTextIsExact();
    Test_Y8_IrqDisagreementSentenceIsExact();
    Test_Y9_IrqNoSentenceHardcodesAGroupName();
    Test_Y10_IrqPercentFormatting();
    Test_Y11_IrqCardLineIsExact();

    std::printf("\n== Z. Extreme game mode - the blanket sweep ==\n");
    Test_Z1_DefaultIsOff();
    Test_Z2_RoundTripAndOldConfigs();
    Test_Z3_SweepActiveNeedsAMaskToSweepOnto();
    Test_Z4_EligibilityIsSevenHardGates();
    Test_Z5_TheRuleThroughComputeDesired();
    Test_Z6_NothingIsSweptWithoutAGoverningProfile();
    Test_Z7_AnEmptyHeavyMaskSweepsNothing();
    Test_Z8_AutoPinKeepsItsOwnLabel();
    Test_Z9_AutoPinGreysUnderExtremeMode();
    Test_Z10_BlockedLineGroupsAndCaps();
    Test_Z11_VCacheActiveRow();
    Test_Z12_InfoIconTooltipWording();

    std::printf("\n== AA. Rule 1 - the mask follows the game being played ==\n");
    Test_AA1_DwellIsDerivedFromThePollInterval();
    Test_AA2_ChooseProfileWithNothingToChoose();
    Test_AA3_FirstSelectionIsImmediateAndPrefersTheForeground();
    Test_AA4_AnInterruptedDwellNeverSwitches();
    Test_AA5_TwoLiveGamesTheForegroundOneWins();
    Test_AA6_ForegroundOnADescendantStillCounts();
    Test_AA7_AltTabbingToSomethingElseNeverUnpinsTheGame();
    Test_AA8_TheDwellThroughComputeDesired();
    Test_AA9_TheSelectedGameExitingReleasesAtOnce();
    Test_AA10_OneGameRunningIsByteIdenticalToBefore();
    Test_AA11_AMigratedLegacyProfileGovernsNothing();
    Test_AA12_EveryReasonHasItsOwnWords();
    Test_AA13_OneCandidateIgnoresTheForegroundEntirely();

    std::printf("\n== AC. Extreme game mode's readout ==\n");
    Test_AC1_SweptExesGroupCountAndSortByCount();
    Test_AC2_SweptProcessCountCountsProcessesNotApps();
    Test_AC3_SweepLineReportsBothCountsAndTheMask();
    Test_AC4_SweepLineCapsByCountAndByLength();
    Test_AC5_SweepLineIsSilentWithNothingToSay();
    Test_AC6_SweptNotAppliedCountsTheSettersOwnAnswer();
    Test_AC7_RefusalCountNeverIntroducesTheAppList();

    std::printf("\n== AD. The panel follows the governing profile ==\n");
    Test_AD1_ThePanelFollowsTheGameInFront();
    Test_AD2_FollowNeverStealsAnEditInProgress();
    Test_AD3_TheOperatorsOwnChoiceIsHonoured();
    Test_AD4_NothingGoverningNeverMovesTheSelection();
    Test_AD5_TheFollowPicksTheProfileTheEngineNAMES();

    std::printf("\n== AE. The restore journal is a promise, not an ordering ==\n");
    Test_AE1_TheFourJournalRules();
    Test_AE5_MissingIsNotUnreadable();
    Test_AE6_AFailedJournalWriteIsReported();
    Test_AE7_AnUnreadableJournalIsNeverOverwritten();
    Test_AE8_TheSetterChecksWhoItIsWriting();

    std::printf("\n== AF. The apply gate compares the mask's CONTENT, not its NAME ==\n");
    Test_AF1_TheGateComparesContentNotName();
    Test_AF2_AnEditedMaskReachesTheMachine();
    Test_AF3_OrderComesOffDiskAndIsNotAChange();
    Test_AF4_AnEditedMaskAfterABlockedAttemptIsStillJournalled();

    std::printf("\n== AB. The version label in the settings window ==\n");
    Test_AB1_TheShippedVersionFormatsAsTheOperatorNamesIt();
    Test_AB2_AFourthFieldIsShownOnlyWhenItSaysSomething();
    Test_AB3_AnUnreadableVersionDrawsNothing();
    Test_AB4_TheLabelIsNeverSomethingElse();


    std::printf("\n== AG. Auto-Isolate GPU ==\n");
    Test_AG1_AdapterKeyFromPnpId();
    Test_AG2_FormatAndParsePreferenceValue();
    Test_AG3_PlanGpuIsolation();
    Test_AG4_ClassifyGpuPref();
    Test_AG5_ConfigRoundTripPreservesUnknownKey();

    std::printf("\n== AH. GPU Assignment tab rows ==\n");
    Test_AH1_FormatRowGpu();
    Test_AH2_RowState();
    Test_AH3_SelectForBackground();
    Test_AH4_SelectAllTicks();
    Test_AJ_GpuPolicy();
    Test_AH5_StatusLine();
    Test_AH6_RowsNeedingRestart();

    std::printf("\n== AI. Orphaned GPU assignments ==\n");
    Test_AI1_FindOrphanedAssignments();
    Test_AI2_DriftStatusLine();

    std::printf("\n== AK. Which GPU CUDA uses ==\n");
    Test_AK1_ParseUniversalGpuId();
    Test_AK2_TheKeyCarriesTheDeviceId();
    Test_AK3_MatchUniversalId();
    Test_AK4_CudaPlanFor();
    Test_AK5_CudaTargetFor();
    Test_AK6_WriteCudaForRow();
    Test_AK7_RestoreCudaForRow();
    Test_AK8_TheRecordOnDisk();
    Test_AK9_OneLinePerApplication();
    Test_AK10_TheWordsTheDialogsUse();
    Test_AK11_TheSettingRoundTrips();
    Test_AK12_AFailedReadIsNeverNoSetting();
    Test_AK13_MembershipAndIdenticalCards();
    Test_AK14_AFailedSaveLeavesNothingBehind();
    Test_AK15_WhatRemoveSaysWhenThereWasNothingToPutBack();
    Test_AK16_AFailedLookupIsNeverNoProfile();
    Test_AK17_TwoAppliesThenRemove();
    Test_AK18_OnlyAnEntryWeOwn();
    Test_AK19_RemoveFailsClosedOnIdentity();
    Test_AK20_TheCudaGateDecidesTheRow();
    Test_AK21_OnlyOurOwnSettingIsRemoved();
    Test_AK22_TheAdoptPathPassesTheOwnershipCheck();
    Test_AK23_ACleanupThatFailedStopsTheRun();
    Test_AK24_TheSentencesThatMustNameSomething();
    Test_AK25_TheRecordAndTheDriverGuards();
    Test_AK26_SettingsAreIdentifiedNotCounted();
    Test_AK27_CapabilityIsHonestAtTheSeam();
    Test_AK28_TheRollbacksOwnSaveIsChecked();
    Test_AK29_MembershipIsJudgedOnUniqueEntries();
    Test_AK30_TheClearOnlyRestoreKeepsItsLine();
    Test_AK31_TheAdoptPathAndTwoWrongSentences();
    Test_AK32_TheAdoptLookupUsesTheRecordedName();
    Test_AK33_AnOverReportedPageIsAFailedEnumeration();
    Test_AK34_TheAdoptedPreviousValueIsTheEntrysOwn();
    Test_AK35_TheLocalSettingRuleIsShared();

    std::printf("\n== AL. What a run says while it walks its rows (v0.5.9) ==\n");
    Test_AL1_TheRunProgressLine();

    std::printf("\n");
    std::printf("TOTAL %d PASSED %d FAILED %d\n", g_total, g_total - g_failed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
