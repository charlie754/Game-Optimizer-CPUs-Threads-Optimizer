// Interrupt and DPC bench - the PURE half. No .cpp, no OS call, no state.
//
// Header-only on purpose. tools\build-tests.bat names its translation units explicitly and
// has one commit in its whole history, so a pure header is what the unit harness can reach
// with zero build-script edits - the same reason mask_edit.h, settings_warning.h and
// startup_warning.h are shaped this way.
//
// EVERYTHING MECHANICALLY CHECKABLE LIVES HERE: the mask decoding, the machine-scope
// refusals, the hot-processor predicate, the group-membership arithmetic, and EVERY
// user-facing sentence the bench can put on screen. The impure half - PDH, cfgmgr32, the
// registry reads - is in irq_probe.cpp, where no test can reach it and where nothing
// testable was left behind.
//
// THIS FEATURE READS. IT NEVER WRITES. There is no encoder here and no registry writer
// anywhere in it: the only mask arithmetic below turns bytes Windows already stored into a
// list of processor numbers a person can read. A decoder that cannot be called by a writer
// cannot transpose anything onto the machine.
//
// THE APPLICATION'S OWN NAME MUST NOT APPEAR IN THIS FILE - not in a string and not in a
// comment. Every sentence below says "this page". That is what lets the wording tripwire in
// tests\test_main.cpp ban the first six letters of the word "optimiser" outright: a file
// that named the product would fail its own gate on the very first run and would then be
// excused from that gate by exception forever.
#pragma once

#include <string>
#include <vector>

#include "topology.h"      // brings <windows.h>; the same shape mask_edit.h uses

namespace cd {

// ---- Tunables. Every one of these is printed on screen, so the user can see what the
// ---- words on this page were measured against rather than taking them on trust.
//
// [A] kIrqHotPercent is a JUDGEMENT CALL, not a measured threshold. It is chosen against the
// reference machine's own capture, where the loaded processor read 36.87% DPC + 13.97% ISR
// and fourteen of the sixteen processors beside it read 0.00%. Nothing has been shown to
// happen at 5%; it is simply far above the floor and far below the one real reading.
constexpr double kIrqHotPercent   = 5.0;
// A processor at or under this reads as "quiet" in the sentence that counts them.
constexpr double kIrqQuietPercent = 1.0;
// Below this many VALID samples a processor is reported as not measured. It is never
// reported as 0.00%: a failed counter read and a genuinely idle processor are different
// states and folding them together is the one lie this page is built to avoid.
constexpr int    kIrqMinSamples   = 3;
// Collections per capture. The first primes the rate counters and contributes no sample by
// contract, so the usable count is one less than this.
constexpr int    kIrqCollects     = 8;

// ---- Mask arithmetic -------------------------------------------------------
// [M] KAFFINITY addresses ONE processor group. sizeof is asserted, never assumed.
static_assert(sizeof(KAFFINITY) == sizeof(ULONG_PTR), "KAFFINITY is not pointer-sized");
constexpr size_t kIrqMaskBytes = sizeof(ULONG_PTR);
constexpr ULONG  kIrqMaxLogicalProcessors = 64;

// Why this page can refuse a machine outright even though it only reads: the values it
// decodes address one processor group and cannot name a processor above 63, so on a machine
// outside that shape a decoded processor list would be WRONG rather than merely incomplete.
enum class IrqRefusal {
    None = 0,
    MultipleProcessorGroups,
    TooManyProcessors,
    UnexpectedProcessorNumbering,
    CountersUnavailable
};

inline bool IrqRefusalIsHard(IrqRefusal r) { return r != IrqRefusal::None; }

// Non-empty and PAIRWISE DISTINCT for every value, including None - a value added later
// without a sentence fails the suite rather than rendering as a blank banner.
inline const wchar_t* IrqRefusalReason(IrqRefusal r) {
    switch (r) {
        case IrqRefusal::None:
            return L"This machine is in scope for the readings below.";
        case IrqRefusal::MultipleProcessorGroups:
            return L"This machine reports more than one processor group. The values this page "
                   L"decodes address one group only, so it cannot describe this machine "
                   L"correctly.";
        case IrqRefusal::TooManyProcessors:
            return L"This machine has more than 64 logical processors. The values this page "
                   L"decodes cannot name a processor above 63, so it cannot describe this "
                   L"machine correctly.";
        case IrqRefusal::UnexpectedProcessorNumbering:
            return L"This machine numbers its processors in a way this page has never been "
                   L"tested against, so it cannot be sure which bit means which processor. It "
                   L"will not guess.";
        case IrqRefusal::CountersUnavailable:
            return L"Windows did not give this page the per-processor counters it needs, so "
                   L"there is nothing to measure. Running lodctr /R from an administrator "
                   L"command prompt rebuilds them.";
    }
    return L"This machine is out of scope for a reason this page has no sentence for.";
}

// Machine scope. THREE INDEPENDENT TESTS ON PURPOSE.
//
// [M] src\topology.cpp silently rewrites a failed GetActiveProcessorGroupCount() from 0 to 1,
// so groupCount alone cannot carry a refusal gate - a machine whose group query failed looks
// single-group. The per-entry Group check and the processor count are read from a different
// API and cannot fail in the same direction.
//
// An EMPTY topology returns UnexpectedProcessorNumbering: there is then no processor 0, which
// is exactly the case the numbering gate exists to refuse rather than guess at.
inline IrqRefusal IrqEvaluateMachine(const Topology& t) {
    if (t.groupCount > 1) return IrqRefusal::MultipleProcessorGroups;
    for (size_t i = 0; i < t.entries.size(); ++i)
        if (t.entries[i].Group != 0) return IrqRefusal::MultipleProcessorGroups;
    if (t.totalLogicalProcessors > (int)kIrqMaxLogicalProcessors)
        return IrqRefusal::TooManyProcessors;
    if (t.entries.size() > (size_t)kIrqMaxLogicalProcessors)
        return IrqRefusal::TooManyProcessors;
    if (t.entries.empty()) return IrqRefusal::UnexpectedProcessorNumbering;
    ULONG minLp = t.entries[0].LogicalProcessorIndex;
    for (size_t i = 1; i < t.entries.size(); ++i)
        if (t.entries[i].LogicalProcessorIndex < minLp)
            minLp = t.entries[i].LogicalProcessorIndex;
    if (minLp != 0) return IrqRefusal::UnexpectedProcessorNumbering;
    return IrqRefusal::None;
}

inline int IrqPopCount(ULONG_PTR mask) {
    int n = 0;
    while (mask) { mask &= (mask - 1); ++n; }
    return n;
}

// THE ONLY DECODER IN THIS PRODUCT, and it is little-endian BY CONSTRUCTION rather than by
// the machine: a VALUE shift, so the answer is the same whatever the host stores. No memcpy,
// no reinterpret_cast<const ULONG_PTR*>, no union - each of those would make the result
// depend on the host's own byte order, and the reference bytes 00 00 ff ff 00 00 00 00 mean
// CPUs 16-31 on every host that ever writes them.
//
// Deliberately MORE PERMISSIVE than any writer would be: 4 bytes are accepted and
// zero-extended, because another tool may have written a 4-byte value and the user is
// entitled to see the state they are actually in. Any other length returns false.
inline bool IrqRegBytesToMask(const BYTE* data, size_t bytes, ULONG_PTR& outMask) {
    if (!data) return false;
    if (bytes != 4 && bytes != kIrqMaskBytes) return false;
    ULONG_PTR m = 0;
    for (size_t i = 0; i < bytes; ++i)
        m |= (ULONG_PTR)data[i] << (8 * i);
    outMask = m;
    return true;
}

// The same decode taken from the buffer a registry read filled, and THE ONLY FORM A CALLER
// SHOULD USE. An empty buffer is refused HERE rather than at the call site, because the raw
// form above needs &bytes[0] and taking that address on an empty vector is undefined
// behaviour before this function is ever entered. [M] Council review 2026-09-08 found exactly
// that call in src\irq_probe.cpp.
inline bool IrqRegBytesToMask(const std::vector<BYTE>& bytes, ULONG_PTR& outMask) {
    if (bytes.empty()) return false;
    return IrqRegBytesToMask(&bytes[0], bytes.size(), outMask);
}

// A REGISTRY VALUE IS READ TWICE - once for its size, once for its bytes - AND IT CAN CHANGE
// BETWEEN THE TWO. This is the decision that says the second read is usable, and it lives in
// the pure half so a test can reach it: the caller is in irq_probe.cpp, which no test links.
// `allocated` is the buffer sized from the FIRST query; `returned` is what the SECOND
// reported.
//
// ZERO IS THE ONE THAT MATTERS: a value that shrank to nothing between the two queries left
// the caller holding an EMPTY vector that it then indexed. A size larger than the buffer is
// refused because it cannot have been written into it. A value that merely shrank to a
// smaller non-zero size IS usable - those bytes really were read - and the caller shrinks its
// buffer to match rather than decoding the stale tail.
inline bool IrqRegSecondReadIsUsable(LSTATUS rc, size_t allocated, DWORD returned) {
    if (rc != ERROR_SUCCESS) return false;
    if (returned == 0) return false;
    if ((size_t)returned > allocated) return false;
    return true;
}

// Windows writes "Affinity Policy - Temporal\TargetSet" as a REG_DWORD, not as bytes, so it
// needs no byte-order reasoning at all - the DWORD is already a value. It is a separate
// function rather than a cast so that the two shapes cannot be confused at a call site.
inline ULONG_PTR IrqTargetSetToMask(DWORD targetSet) { return (ULONG_PTR)targetSet; }

// "16-31", "0, 4, 8", "0, 3-5, 9". Empty mask -> empty string.
inline std::wstring FormatCpuList(ULONG_PTR mask) {
    std::wstring out;
    ULONG bit = 0;
    while (bit < kIrqMaxLogicalProcessors) {
        if (!(mask & ((ULONG_PTR)1 << bit))) { ++bit; continue; }
        const ULONG runStart = bit;
        while (bit + 1 < kIrqMaxLogicalProcessors &&
               (mask & ((ULONG_PTR)1 << (bit + 1))))
            ++bit;
        if (!out.empty()) out += L", ";
        out += std::to_wstring(runStart);
        if (bit > runStart) { out += L"-"; out += std::to_wstring(bit); }
        ++bit;
    }
    return out;
}

// "CPUs 16-31 (16 processors)". The count is spelled out because a mask is a shape a person
// cannot count at a glance, and the whole point of this page is that the number is checkable.
inline std::wstring FormatCpuListWithCount(ULONG_PTR mask) {
    const int n = IrqPopCount(mask);
    if (n == 0) return L"no processors";
    std::wstring s = L"CPUs " + FormatCpuList(mask) + L" (" + std::to_wstring(n);
    s += (n == 1) ? L" processor)" : L" processors)";
    return s;
}

// The game group's own processors, as a mask, so the overlap with a device's target set can
// be shown in the same vocabulary. Refuses rather than truncates: an unknown or out-of-range
// processor would silently drop a bit and the shown list would then be a different set from
// the one the mask names.
inline bool BuildIrqMask(const Topology& t, const std::vector<ULONG>& lps, ULONG_PTR& outMask) {
    if (lps.empty()) return false;
    ULONG_PTR m = 0;
    for (size_t i = 0; i < lps.size(); ++i) {
        const ULONG lp = lps[i];
        if (lp >= kIrqMaxLogicalProcessors) return false;
        if (!FindByLp(t, lp)) return false;
        m |= ((ULONG_PTR)1 << lp);
    }
    outMask = m;
    return true;
}

// ---- Measurement -----------------------------------------------------------
// One logical processor's share of a capture. `samples` counts VALID readings only: an item
// whose CStatus was not valid contributes NOTHING and is not a 0.0%.
struct CoreDpcStat {
    ULONG  lp = 0;
    int    samples = 0;
    double meanDpcPct = 0.0, minDpcPct = 0.0, maxDpcPct = 0.0;
    double meanIsrPct = 0.0, minIsrPct = 0.0, maxIsrPct = 0.0;
    bool   inGameGroup = false;
};

// Interrupt service plus deferred procedure calls. Both preempt every thread on the
// processor, so the pair is what a thread-placement product cannot reach, and the pair is
// what "hot" is measured against.
inline double IrqCombinedPct(const CoreDpcStat& c) { return c.meanIsrPct + c.meanDpcPct; }

inline bool IrqCoreIsMeasured(const CoreDpcStat& c) { return c.samples >= kIrqMinSamples; }

inline bool IrqCoreIsHot(const CoreDpcStat& c) {
    return IrqCoreIsMeasured(c) && IrqCombinedPct(c) >= kIrqHotPercent;
}

// GROUP MEMBERSHIP IS DECIDED BY THE GROUP'S OWN PROCESSOR LIST AND NEVER BY WHETHER A
// SAMPLE EXISTS. A processor the counters never named is still in the group it is in; asking
// the RESULTS instead would let "N of M processors in that group" describe a different set
// from the mask this page had just printed. [M] Council review 2026-09-08, finding H8.
inline bool IrqLpIsInGameGroup(ULONG lp, const std::vector<ULONG>& groupLps) {
    for (size_t i = 0; i < groupLps.size(); ++i)
        if (groupLps[i] == lp) return true;
    return false;
}

// Marks membership from the game group's own processor list. Takes the LIST and not a mask,
// so it stays correct for a caller that has processors this page would refuse to encode.
inline void IrqMarkGameGroup(std::vector<CoreDpcStat>& cores,
                             const std::vector<ULONG>& groupLps) {
    for (size_t i = 0; i < cores.size(); ++i)
        cores[i].inGameGroup = IrqLpIsInGameGroup(cores[i].lp, groupLps);
}

// Index of the measured in-group processor carrying the most interrupt and DPC time, or -1
// when nothing in the group was measured at all. Ties go to the lower index so two runs on
// one machine name the same processor.
inline int IrqHottestInGroupIndex(const std::vector<CoreDpcStat>& cores) {
    int best = -1;
    for (size_t i = 0; i < cores.size(); ++i) {
        if (!cores[i].inGameGroup || !IrqCoreIsMeasured(cores[i])) continue;
        if (best < 0 || IrqCombinedPct(cores[i]) > IrqCombinedPct(cores[(size_t)best]))
            best = (int)i;
    }
    return best;
}

// The same question over every processor, in scope or not. A hot processor OUTSIDE the game's
// group is not the warning this page exists for, but reporting nothing at all would leave the
// user believing the machine was quiet.
inline int IrqHottestOverallIndex(const std::vector<CoreDpcStat>& cores) {
    int best = -1;
    for (size_t i = 0; i < cores.size(); ++i) {
        if (!IrqCoreIsMeasured(cores[i])) continue;
        if (best < 0 || IrqCombinedPct(cores[i]) > IrqCombinedPct(cores[(size_t)best]))
            best = (int)i;
    }
    return best;
}

// THE M IN "N of M processors in that group". Counted from the group's own list against
// every processor this page has a ROW for, so every processor in the count is one the user
// can look at - measured or not measured. It is deliberately NOT counted from the sampled
// results: a processor PDH never returned would then leave the group entirely, and the count
// beside the warning would name a smaller set than the mask above it.
inline int IrqCoresInGroup(const std::vector<ULONG>& allLps,
                           const std::vector<ULONG>& groupLps) {
    int n = 0;
    for (size_t i = 0; i < allLps.size(); ++i)
        if (IrqLpIsInGameGroup(allLps[i], groupLps)) ++n;
    return n;
}

// Measured in-group processors sitting at or under kIrqQuietPercent. Counted separately from
// the group size because an unmeasured processor is neither quiet nor loud.
inline int IrqQuietCoresInGroup(const std::vector<CoreDpcStat>& cores) {
    int n = 0;
    for (size_t i = 0; i < cores.size(); ++i)
        if (cores[i].inGameGroup && IrqCoreIsMeasured(cores[i]) &&
            IrqCombinedPct(cores[i]) <= kIrqQuietPercent)
            ++n;
    return n;
}

// THE WARNING THIS PAGE EXISTS FOR: a processor carrying interrupt and DPC time that is also
// inside the group this machine's own profile put the game on.
inline bool IrqHotCoreIsInGameGroup(const std::vector<CoreDpcStat>& cores) {
    const int idx = IrqHottestInGroupIndex(cores);
    return idx >= 0 && IrqCoreIsHot(cores[(size_t)idx]);
}

// ---- What the registry says, and what Windows says ------------------------
// Two keys, and they can disagree. `Affinity Policy` is what somebody asked for;
// `Affinity Policy - Temporal` is what Windows itself recorded. [M] On the reference machine
// one graphics adapter had a policy naming CPUs 16-31 while the temporal target set named a
// striped half of the machine that included the loaded processor - so showing only the policy
// would have reported the INPUT and called it the OUTCOME.
struct IrqPolicyReadout {
    bool      readFailed    = false;
    bool      policyPresent = false;   ULONG_PTR policyMask = 0;
    // THE VALUE IS THERE AND COULD NOT BE DECODED - a third state, because two were not
    // enough. With present/absent alone, an AssignmentSetOverride of a length this page
    // cannot read folded into "not set", which is the one fold the comment on IrqPolicyState
    // forbids. [M] Council review 2026-09-08, C2 / H3.
    bool      policyUnreadable = false;
    bool      targetPresent = false;   ULONG_PTR targetMask = 0;
};

enum class IrqAgreement { Unreadable, NeitherPresent, PolicyOnly, TargetOnly, Agree, Disagree };

inline IrqAgreement IrqClassifyAgreement(const IrqPolicyReadout& r) {
    // UNREADABLE FIRST, AND A POLICY THAT WOULD NOT DECODE COUNTS AS UNREADABLE. It must not
    // fall through to NeitherPresent or to TargetOnly: both of those sentences say, in as
    // many words, that this device carries no interrupt affinity policy.
    if (r.readFailed || r.policyUnreadable) return IrqAgreement::Unreadable;
    if (!r.policyPresent && !r.targetPresent) return IrqAgreement::NeitherPresent;
    if (r.policyPresent && !r.targetPresent) return IrqAgreement::PolicyOnly;
    if (!r.policyPresent && r.targetPresent) return IrqAgreement::TargetOnly;
    return r.policyMask == r.targetMask ? IrqAgreement::Agree : IrqAgreement::Disagree;
}

inline std::wstring IrqAgreementText(IrqAgreement a, const IrqPolicyReadout& r) {
    switch (a) {
        case IrqAgreement::Unreadable:
            return L"This page could not read this device's interrupt keys. That is not the "
                   L"same as there being nothing in them - it means the read failed.";
        case IrqAgreement::NeitherPresent:
            return L"Nothing on this device names a processor set. Windows places its "
                   L"interrupts by its own default.";
        case IrqAgreement::PolicyOnly:
            return L"A policy on this device names " + FormatCpuListWithCount(r.policyMask) +
                   L". Windows has recorded no target set of its own for it.";
        case IrqAgreement::TargetOnly:
            return L"This device carries no interrupt affinity policy, and Windows has "
                   L"recorded a target set of its own naming " +
                   FormatCpuListWithCount(r.targetMask) +
                   L". That set was written by Windows, not by any policy on this device.";
        case IrqAgreement::Agree:
            return L"The policy on this device and the target set Windows recorded for it "
                   L"name the same processors: " + FormatCpuListWithCount(r.policyMask) + L".";
        case IrqAgreement::Disagree:
            return L"THESE TWO DISAGREE. The policy on this device names " +
                   FormatCpuListWithCount(r.policyMask) +
                   L", and the target set Windows recorded for it names " +
                   FormatCpuListWithCount(r.targetMask) +
                   L". The policy is what somebody asked for; the target set is what Windows "
                   L"wrote down. This page cannot tell you which of them the hardware is "
                   L"following.";
    }
    return L"This page has no sentence for the state these two keys are in.";
}

// The static policy column, on its own. Unreadable is NEVER folded into "not set": an
// unreadable value must not read as absent any more than it may read as present.
enum class IrqPolicyState { NotConfigured, Configured, Unreadable };

// ONE decision, so the device row, the detail sentence and the agreement classifier cannot
// drift apart. The temporal TargetSet path already routed a failed decode to Unreadable
// through readFailed; this is the static path saying the same thing the same way.
inline IrqPolicyState IrqPolicyStateOf(const IrqPolicyReadout& r) {
    if (r.readFailed || r.policyUnreadable) return IrqPolicyState::Unreadable;
    if (r.policyPresent) return IrqPolicyState::Configured;
    return IrqPolicyState::NotConfigured;
}

inline std::wstring IrqPolicyStateText(IrqPolicyState s,
                                       const std::wstring& cpuListWithCount) {
    switch (s) {
        case IrqPolicyState::NotConfigured:
            return L"Not set. Windows chooses this device's interrupt processors.";
        case IrqPolicyState::Configured:
            return L"Set to " + cpuListWithCount +
                   L". That is what the registry holds; it is not a statement about where "
                   L"interrupts land.";
        case IrqPolicyState::Unreadable:
            return L"This page could not read this device's interrupt policy. That is not the "
                   L"same as there being none - it means the read failed.";
    }
    return L"This page has no sentence for this device's interrupt policy.";
}

// ---- Naming the group honestly ---------------------------------------------
// [M] On the reference machine the derived names are "Cache" and "Freq", NOT "CCD0" -
// src\topology.cpp emits those for an asymmetric-cache part. NO STRING HERE MAY HARDCODE A
// GROUP NAME; the name is always passed in.
enum class GameGroupSource { LiveProfile, MachineDefault, Unknown };

inline const wchar_t* GameGroupPhrase(GameGroupSource s) {
    switch (s) {
        case GameGroupSource::LiveProfile:    return L"the group your game is assigned to";
        case GameGroupSource::MachineDefault:
            return L"this machine's default game group - no game is running right now";
        case GameGroupSource::Unknown:        return L"a group this page could not identify";
    }
    return L"a group this page has no phrase for";
}

// ---- Number formatting shared by every sentence ---------------------------
// Two decimals everywhere, so a figure quoted on screen matches a figure pasted from the
// clipboard and both match what Get-Counter prints.
inline std::wstring IrqFormatPct(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", v);
    return std::wstring(buf);
}

// ---- Every remaining user-facing literal -----------------------------------
inline const wchar_t* IrqBenchIntroText() {
    return
        L"Windows CPU Sets move THREADS. A driver's deferred procedure call is not a thread - "
        L"it runs above every thread and interrupts whatever is on its processor. So when a "
        L"processor inside your game's group is spending its time on DPCs and on interrupt "
        L"service, moving processes around cannot reach it.\r\n"
        L"\r\n"
        L"This page only measures. It reads the per-processor counters Windows already "
        L"publishes, and it reads each PCI device's interrupt keys out of the registry. It "
        L"writes nothing, it changes nothing, and it asks for no administrator approval.\r\n"
        L"\r\n"
        L"Three things it cannot do. It cannot name the driver that owns a DPC - Windows does "
        L"not expose that to an ordinary program, and naming one needs a kernel trace, which "
        L"LatencyMon and Windows Performance Recorder can take. It cannot measure your game; "
        L"nothing on this page looks at a game at all. And it has never been shown - not here, "
        L"and not by the measurements this page was built on - that a high DPC percentage on "
        L"one processor changes anything you would notice while playing. A number on this page "
        L"is a number about your machine, not a verdict about your session.\r\n"
        L"\r\n"
        L"On the machine this was developed on, an interrupt affinity policy was written by "
        L"hand onto one graphics adapter and the DPC time did not move. The readout below is "
        L"why that is worth seeing: Windows keeps its OWN target set beside the policy, and on "
        L"that machine the two named different processors. So this page shows you both, and "
        L"says so when they disagree.";
}

// The card on the Settings General page. These three live HERE rather than in settings.cpp
// for one reason: the wording gate in tests\test_main.cpp pins the strings in THIS file, and
// a sentence written next to the button that opens the page would sit outside it.
inline const wchar_t* IrqCardHeadingText() { return L"Interrupts and DPCs"; }

// THIS SENTENCE IS NARROW ON PURPOSE, AND THE WIDE ONE IT REPLACED WAS MEASURED WRONG. It
// used to say "no processor assignment can move one" and "there is a kind of stutter nothing
// on this window can reach", and Council review struck both on 2026-09-08:
//
//   [M] The first is contradicted by this project's own measurement. On the machine this was
//   developed on the interrupt and DPC load MOVED, from CPU 1 to CPU 3, when Windows' own
//   Affinity Policy - Temporal\TargetSet changed. A processor assignment demonstrably did
//   move one, so no sentence here may say that none can.
//   [M] The second asserts a symptom. NO FRAME RATE HAS EVER BEEN MEASURED BY THIS PRODUCT,
//   not here and not anywhere else in it, so stutter is not this page's to claim - and the
//   bench intro one click away says a high DPC percentage has never been shown to change
//   anything a player would notice.
//
// What survives is the narrow claim, and it is the one the intro already makes: a CPU Set is
// a THREAD-scheduling preference, a DPC is not a thread, so the assignment this window makes
// cannot reach one. Pinned character for character by Test_Y11 - the wording deny-list in
// Test_X10 cannot catch a sentence that is merely too strong, because it says no banned word.
inline const wchar_t* IrqCardLineText() {
    return L"CPU Sets move threads. A driver's deferred procedure call is not a thread - it "
           L"runs above every thread on its processor - so the processor assignment this "
           L"window makes cannot move one. This opens a read-only readout of where interrupt "
           L"and DPC time is landing on this machine. It measures; it changes nothing.";
}

inline const wchar_t* IrqCardButtonCaption() { return L"Interrupt readout..."; }

inline const wchar_t* IrqDeviceHeadingText() {
    return L"PCI devices, read-only. This page does not know which device owns the interrupt "
           L"time above and does not rank them - a ranking would be a guess dressed as a "
           L"measurement.";
}

inline const wchar_t* IrqNoDevicesText() {
    return L"No present PCI device could be enumerated, so there is nothing to read. The "
           L"per-processor measurements above do not depend on this list.";
}

inline const wchar_t* IrqWindowsChoosesText() { return L"Windows chooses"; }
inline const wchar_t* IrqUnreadableColumnText() { return L"could not be read"; }
inline const wchar_t* IrqNotRecordedText() { return L"not recorded"; }
inline const wchar_t* IrqNotSetText() { return L"not set"; }

inline const wchar_t* IrqNotStartedText() {
    return L"Collecting. The first collection primes the counters and is thrown away, so the "
           L"numbers appear a few seconds after this page opens.";
}

// THE HOT-PROCESSOR WARNING. Names the processor, its two figures, the spread the figure came
// from, how many samples it rests on, the group it is in, and how many of that group's
// processors were quiet - so every part of the claim is checkable on screen.
inline std::wstring IrqHotCoreSentence(const CoreDpcStat& hottest, int quietCoresInGroup,
                                       int coresInGroup, GameGroupSource src,
                                       const std::wstring& groupMaskName) {
    std::wstring s = L"CPU " + std::to_wstring(hottest.lp) + L" spent " +
                     IrqFormatPct(hottest.meanDpcPct) + L"% of the capture on DPCs and " +
                     IrqFormatPct(hottest.meanIsrPct) + L"% on interrupt service (DPC range " +
                     IrqFormatPct(hottest.minDpcPct) + L"-" + IrqFormatPct(hottest.maxDpcPct) +
                     L"% over " + std::to_wstring(hottest.samples) + L" samples). It is in ";
    if (!groupMaskName.empty()) s += L"\"" + groupMaskName + L"\", ";
    s += GameGroupPhrase(src);
    s += L". " + std::to_wstring(quietCoresInGroup) + L" of the " +
         std::to_wstring(coresInGroup) + L" processors in that group stayed at or under " +
         IrqFormatPct(kIrqQuietPercent) + L"%.";
    return s;
}

inline std::wstring IrqNoHotCoreSentence(const CoreDpcStat& highest) {
    return L"Nothing in the game's group is above " + IrqFormatPct(kIrqHotPercent) +
           L"% of interrupt and DPC time together. The highest is CPU " +
           std::to_wstring(highest.lp) + L" at " + IrqFormatPct(IrqCombinedPct(highest)) + L"%.";
}

// The hot processor is real but sits OUTSIDE the group the game was put on. Reported rather
// than hidden, and deliberately NOT reported as the warning above.
inline std::wstring IrqHotCoreOutsideGroupSentence(const CoreDpcStat& hottest,
                                                   GameGroupSource src,
                                                   const std::wstring& groupMaskName) {
    std::wstring s = L"CPU " + std::to_wstring(hottest.lp) + L" spent " +
                     IrqFormatPct(hottest.meanDpcPct) + L"% of the capture on DPCs and " +
                     IrqFormatPct(hottest.meanIsrPct) + L"% on interrupt service. It is NOT in ";
    if (!groupMaskName.empty()) s += L"\"" + groupMaskName + L"\", ";
    s += GameGroupPhrase(src);
    s += L", so it is outside the processors this page would warn about.";
    return s;
}

inline std::wstring IrqNoGroupMeasuredSentence(GameGroupSource src,
                                               const std::wstring& groupMaskName) {
    std::wstring s = L"No processor in ";
    if (!groupMaskName.empty()) s += L"\"" + groupMaskName + L"\", ";
    s += GameGroupPhrase(src);
    s += L", returned enough valid samples to report. That is not the same as those "
         L"processors being idle.";
    return s;
}

inline std::wstring IrqNotMeasuredSentence(ULONG lp) {
    return L"CPU " + std::to_wstring(lp) + L" - not measured. Fewer than " +
           std::to_wstring(kIrqMinSamples) +
           L" valid samples were returned for it. That is not the same as 0.00%.";
}

// Generated from the capture parameters this page actually used, so a user who runs it gets
// the same shape of number rather than a command that was true when it was typed.
inline std::wstring IrqReproductionCommand() {
    return L"Check this yourself: Get-Counter '\\Processor Information(*)\\% DPC Time',"
           L"'\\Processor Information(*)\\% Interrupt Time' -SampleInterval 1 -MaxSamples " +
           std::to_wstring(kIrqCollects) +
           L" - an instance named \"0,3\" is CPU 3 in processor group 0. This page throws away "
           L"the first collection, because a rate counter has nothing to compare against yet, "
           L"and averages the remaining " + std::to_wstring(kIrqCollects - 1) + L".";
}

inline std::wstring IrqGameGroupSentence(GameGroupSource src, const std::wstring& groupMaskName,
                                         ULONG_PTR groupMask) {
    std::wstring s = L"Measured against ";
    if (!groupMaskName.empty()) s += L"\"" + groupMaskName + L"\", ";
    s += GameGroupPhrase(src);
    s += L": " + FormatCpuListWithCount(groupMask) + L".";
    return s;
}

}  // namespace cd
