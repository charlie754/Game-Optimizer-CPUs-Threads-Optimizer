// Game Optimizer - CPU topology enumeration and mask derivation.
//
// This translation unit must contain NO Win32 UI and NO mutable globals: it is linked
// directly into the unit-test harness. Everything below ClassifyTopology() is pure and
// is driven from synthetic topologies in tests, which is how Intel hybrid, symmetric
// dual-CCD and single-domain paths get covered on hardware that has none of them.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace cd {

// One logical processor, as reported by GetSystemCpuSetInformation.
//
// MEASURED on a Ryzen 9 9950X3D, 2026-08-28 - the three facts the classifier depends on:
//   * Id starts at 256, NOT 0.  Id 256 <-> LogicalProcessorIndex 0. Never conflate them:
//     every CPU Sets API takes Id, every piece of UI shows LogicalProcessorIndex.
//   * EfficiencyClass is 0 on all 32 LPs, so it carries no CCD information on AMD.
//   * CoreIndex is the first LP of each physical core (0,0,2,2,4,4,...), so SMT siblings
//     share a CoreIndex. That is what ReduceToNoSmt keys on.
struct CpuSetEntry {
    ULONG  Id = 0;
    ULONG  LogicalProcessorIndex = 0;
    ULONG  CoreIndex = 0;
    ULONG  LastLevelCacheIndex = 0;
    ULONG  NumaNodeIndex = 0;
    USHORT Group = 0;
    BYTE   EfficiencyClass = 0;
    bool   Parked = false;
    bool   Allocated = false;
    bool   RealTime = false;
    ULONGLONG LastLevelCacheBytes = 0;   // from RelationCache; 0 when unknown
};

enum class TopologyKind {
    Unknown,
    IntelHybrid,          // more than one EfficiencyClass
    AmdAsymmetricCache,   // 2+ LLC domains with DIFFERENT L3 sizes  (X3D)
    MultiCcdSymmetric,    // 2+ LLC domains with the SAME L3 size
    SingleDomain          // one LLC domain; only SMT reduction is available
};

enum class Confidence { None, Medium, High };

struct LlcDomain {
    ULONG index = 0;                 // LastLevelCacheIndex
    ULONGLONG l3Bytes = 0;
    std::vector<ULONG> lps;          // LogicalProcessorIndex values, ascending
};

struct Mask {
    std::wstring name;
    std::vector<ULONG> ids;          // CPU Set Ids, ascending
    bool derived = true;             // false once the user has hand-edited it
};

struct Topology {
    std::vector<CpuSetEntry> entries;   // ascending by LogicalProcessorIndex
    std::vector<LlcDomain>   domains;   // ascending by index
    TopologyKind kind = TopologyKind::Unknown;
    Confidence   confidence = Confidence::None;
    USHORT       groupCount = 1;
    int          totalLogicalProcessors = 0;

    // Set by ClassifyTopology. Stable across runs on the same machine; a change means the
    // stored config's CPU Set Ids may no longer mean what they meant, so masks are re-derived.
    // Shape: "<vendor>:<domainCount>:<lpsPerDomain...>:<l3KB per domain...>"
    std::wstring signature;

    std::wstring summary;             // one human sentence for the wizard
    std::wstring defaultGameMask;     // recommended mask name for the game
    std::wstring defaultHeavyMask;    // recommended mask name for background apps
};

// Enumerate the live machine: GetSystemCpuSetInformation + GetLogicalProcessorInformationEx,
// then ClassifyTopology. Returns false only when an OS call fails; *error gets the reason.
bool DetectTopology(Topology& out, std::wstring* error);

// Pure. Fills kind / confidence / signature / summary / defaultGameMask / defaultHeavyMask
// from an already-populated entries + domains. This is the seam the unit tests drive.
//
// Decision table (first match wins):
//   more than one distinct EfficiencyClass      -> IntelHybrid,        High
//        highest EfficiencyClass = "P-cores", lowest = "E-cores"
//        default game "P-cores", heavy "E-cores"
//   2+ domains, L3 sizes differ                 -> AmdAsymmetricCache, High
//        largest L3 = "Cache"; others "Freq" (then "Freq 2", "Freq 3", ...)
//        default game "Cache", heavy "Freq"
//   2+ domains, L3 sizes equal                  -> MultiCcdSymmetric,  Medium
//        "CCD0", "CCD1", ... ordered by lowest LP index
//        default game "CCD0", heavy "CCD1"
//   exactly 1 domain                            -> SingleDomain,       None
//        only "All" / "All no SMT"; default game "All no SMT", heavy "All"
//
// THE GAME DEFAULT IS THE WHOLE FIRST GROUP, NOT ITS "no SMT" REDUCTION. Operator decision
// 2026-08-29, changed from "Cache no SMT" / "P-cores no SMT" / "CCD0 no SMT". Dropping half
// the threads of the cache domain is a TUNING choice with a real cost to any game that scales
// past eight cores, and a first run has measured nothing yet - so the default is the mask that
// can only help, and the "no SMT" variants stay one combo-box selection away.
// defaultHeavyMask is unchanged.
//
// SINGLEDOMAIN AND UNKNOWN ARE THE EXCEPTION AND KEEP "All no SMT". There, "All" is every
// processor, which is exactly what having no assignment at all means - the shipped profile
// would be inert, and the first-run wizard already tells the user in as many words that the
// SMT masks are the only thing that can do anything on such a machine. The change above is
// "stop halving a domain we already isolated"; it is not "stop doing anything".
//
// The chosen name is checked against DeriveMasks before it is stored, because a default
// naming a mask that DeriveMasks did not emit would leave the engine with nothing to apply.
void ClassifyTopology(Topology& t);

// Pure. Produces the named masks for a topology, in display order. "All" and "All no SMT"
// are always last. A "no SMT" variant is omitted when it would equal its parent.
std::vector<Mask> DeriveMasks(const Topology& t);

// Pure. Keeps the lowest LogicalProcessorIndex of each distinct CoreIndex present in `ids`.
// Input and output are both CPU Set Ids.
std::vector<ULONG> ReduceToNoSmt(const Topology& t, const std::vector<ULONG>& ids);

// Pure. The name of the mask in `masks` whose Id set is EXACTLY `ids`, or an empty string
// when none of them is.
//
// This is the inverse direction to everything else in this file, and it exists for ONE
// caller: the readback. GetProcessDefaultCpuSets hands back a bare list of Ids, and the only
// honest way to turn that into a word a user recognises is to ask which named mask it equals.
// Both sides are compared as SETS - sorted, duplicates collapsed - because the API promises
// no order. Nothing here is a claim that the mask is in EFFECT; see applier.h.
//
// A partial or overlapping set deliberately matches NOTHING: "mostly Cache" is not a state
// this product has, and reporting the nearest name would be exactly the guess the readback
// exists to avoid. When two masks hold identical ids the FIRST in display order wins, which
// is the one the combo boxes offer first.
std::wstring MaskNameForIds(const std::vector<Mask>& masks, const std::vector<ULONG>& ids);

// Lookups. Return nullptr when absent.
const CpuSetEntry* FindById(const Topology& t, ULONG id);
const CpuSetEntry* FindByLp(const Topology& t, ULONG lp);

// Convenience for the UI and the config writer.
std::vector<ULONG> IdsForLps(const Topology& t, const std::vector<ULONG>& lps);
std::vector<ULONG> LpsForIds(const Topology& t, const std::vector<ULONG>& ids);

const wchar_t* KindName(TopologyKind k);
const wchar_t* ConfidenceName(Confidence c);

}  // namespace cd
