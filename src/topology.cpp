// Game Optimizer - CPU topology enumeration and mask derivation.
//
// See topology.h for the contract. Everything below the DetectTopology section is pure:
// no Win32 calls, no globals, no I/O. The unit-test harness drives those functions with
// synthetic Intel-hybrid / symmetric-CCD / single-domain topologies, so keep them that way.
//
// The single exception worth calling out is the vendor token in the signature, which needs
// the CPU brand. That is read with the __cpuid intrinsic - a CPU instruction, not a Win32
// call and not a global - so ClassifyTopology stays link-clean for the test harness.
#include "topology.h"

#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace cd {

// ---------------------------------------------------------------------------
// small local helpers
// ---------------------------------------------------------------------------
namespace {

// True when the CPU brand / vendor string contains "AMD". Uses __cpuid only.
bool CpuBrandIsAmd() {
#if defined(_M_X64) || defined(_M_IX86)
    int regs[4] = { 0, 0, 0, 0 };

    // Extended brand string, leaves 0x80000002..0x80000004.
    __cpuid(regs, (int)0x80000000);
    const unsigned maxExt = (unsigned)regs[0];
    if (maxExt >= 0x80000004u) {
        char brand[49];
        std::memset(brand, 0, sizeof(brand));
        for (unsigned i = 0; i < 3; ++i) {
            __cpuid(regs, (int)(0x80000002u + i));
            std::memcpy(brand + i * 16, regs, 16);
        }
        brand[48] = '\0';
        for (char* p = brand; *p; ++p) {
            *p = (char)std::toupper((unsigned char)*p);
        }
        if (std::strstr(brand, "AMD") != nullptr) {
            return true;
        }
    }

    // Vendor id, leaf 0 -> EBX, EDX, ECX ("AuthenticAMD").
    __cpuid(regs, 0);
    char vendor[13];
    std::memcpy(vendor + 0, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    vendor[12] = '\0';
    for (char* p = vendor; *p; ++p) {
        *p = (char)std::toupper((unsigned char)*p);
    }
    return std::strstr(vendor, "AMD") != nullptr;
#else
    return false;
#endif
}

// One labelled group of logical processors: the source of a derived mask, and of one
// clause in the human summary. Pure data.
struct LabeledGroup {
    std::wstring       label;
    std::vector<ULONG> ids;    // CPU Set Ids, ascending
    size_t             lpCount = 0;
    ULONGLONG          l3Bytes = 0;
};

// "96 MB" when the size is a whole number of MB, otherwise "98304 KB", otherwise empty.
std::wstring FormatCacheSize(ULONGLONG bytes) {
    if (bytes == 0) {
        return std::wstring();
    }
    const ULONGLONG mb = 1024ull * 1024ull;
    if ((bytes % mb) == 0) {
        return std::to_wstring((unsigned long long)(bytes / mb)) + L" MB";
    }
    if ((bytes % 1024ull) == 0) {
        return std::to_wstring((unsigned long long)(bytes / 1024ull)) + L" KB";
    }
    return std::to_wstring((unsigned long long)bytes) + L" bytes";
}

std::vector<ULONG> SortedUnique(std::vector<ULONG> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// Every CPU Set Id in the topology, ascending.
std::vector<ULONG> AllIds(const Topology& t) {
    std::vector<ULONG> ids;
    ids.reserve(t.entries.size());
    for (size_t i = 0; i < t.entries.size(); ++i) {
        ids.push_back(t.entries[i].Id);
    }
    return SortedUnique(ids);
}

// Ids of the entries whose LogicalProcessorIndex appears in `lps`.
std::vector<ULONG> IdsOfLpList(const Topology& t, const std::vector<ULONG>& lps) {
    std::vector<ULONG> ids;
    ids.reserve(lps.size());
    for (size_t i = 0; i < lps.size(); ++i) {
        const CpuSetEntry* e = FindByLp(t, lps[i]);
        if (e != nullptr) {
            ids.push_back(e->Id);
        }
    }
    return SortedUnique(ids);
}

// The distinct EfficiencyClass values present, ascending.
std::vector<BYTE> EfficiencyClasses(const Topology& t) {
    std::set<BYTE> s;
    for (size_t i = 0; i < t.entries.size(); ++i) {
        s.insert(t.entries[i].EfficiencyClass);
    }
    return std::vector<BYTE>(s.begin(), s.end());
}

// The lowest LogicalProcessorIndex in a domain; used to order symmetric CCDs.
ULONG LowestLp(const LlcDomain& d) {
    ULONG lo = 0xFFFFFFFFu;
    for (size_t i = 0; i < d.lps.size(); ++i) {
        if (d.lps[i] < lo) {
            lo = d.lps[i];
        }
    }
    return lo;
}

// The named groups a topology of this kind produces, in display order.
// Pure; shared by DeriveMasks and by the summary sentence so the two cannot drift.
// SingleDomain and Unknown produce nothing here - only "All" / "All no SMT" follow.
std::vector<LabeledGroup> BaseGroups(const Topology& t, TopologyKind kind) {
    std::vector<LabeledGroup> out;

    if (kind == TopologyKind::IntelHybrid) {
        const std::vector<BYTE> classes = EfficiencyClasses(t);
        if (classes.size() < 2) {
            return out;
        }
        const BYTE hi = classes.back();
        const BYTE lo = classes.front();

        LabeledGroup p;
        p.label = L"P-cores";
        LabeledGroup e;
        e.label = L"E-cores";
        for (size_t i = 0; i < t.entries.size(); ++i) {
            const CpuSetEntry& en = t.entries[i];
            if (en.EfficiencyClass == hi) {
                p.ids.push_back(en.Id);
                p.l3Bytes = (std::max)(p.l3Bytes, en.LastLevelCacheBytes);
            } else if (en.EfficiencyClass == lo) {
                e.ids.push_back(en.Id);
                e.l3Bytes = (std::max)(e.l3Bytes, en.LastLevelCacheBytes);
            }
        }
        p.ids = SortedUnique(p.ids);
        e.ids = SortedUnique(e.ids);
        p.lpCount = p.ids.size();
        e.lpCount = e.ids.size();
        if (!p.ids.empty()) {
            out.push_back(p);
        }
        if (!e.ids.empty()) {
            out.push_back(e);
        }
        return out;
    }

    if (kind == TopologyKind::AmdAsymmetricCache) {
        // Largest L3 is always "Cache"; every other domain is "Freq", "Freq 2", ...
        // Ordered by descending L3, then by lowest LP so the order is deterministic.
        std::vector<const LlcDomain*> ord;
        ord.reserve(t.domains.size());
        for (size_t i = 0; i < t.domains.size(); ++i) {
            ord.push_back(&t.domains[i]);
        }
        std::sort(ord.begin(), ord.end(), [](const LlcDomain* a, const LlcDomain* b) {
            if (a->l3Bytes != b->l3Bytes) {
                return a->l3Bytes > b->l3Bytes;
            }
            return LowestLp(*a) < LowestLp(*b);
        });

        int freqSeq = 0;
        for (size_t i = 0; i < ord.size(); ++i) {
            LabeledGroup g;
            if (i == 0) {
                g.label = L"Cache";
            } else {
                ++freqSeq;
                g.label = (freqSeq == 1) ? std::wstring(L"Freq")
                                         : (L"Freq " + std::to_wstring(freqSeq));
            }
            g.ids = IdsOfLpList(t, ord[i]->lps);
            g.lpCount = ord[i]->lps.size();
            g.l3Bytes = ord[i]->l3Bytes;
            out.push_back(g);
        }
        return out;
    }

    if (kind == TopologyKind::MultiCcdSymmetric) {
        std::vector<const LlcDomain*> ord;
        ord.reserve(t.domains.size());
        for (size_t i = 0; i < t.domains.size(); ++i) {
            ord.push_back(&t.domains[i]);
        }
        std::sort(ord.begin(), ord.end(), [](const LlcDomain* a, const LlcDomain* b) {
            return LowestLp(*a) < LowestLp(*b);
        });
        for (size_t i = 0; i < ord.size(); ++i) {
            LabeledGroup g;
            g.label = L"CCD" + std::to_wstring((unsigned long long)i);
            g.ids = IdsOfLpList(t, ord[i]->lps);
            g.lpCount = ord[i]->lps.size();
            g.l3Bytes = ord[i]->l3Bytes;
            out.push_back(g);
        }
        return out;
    }

    return out;  // SingleDomain / Unknown: only "All" and "All no SMT".
}

// The first name in `wanted` that DeriveMasks(t) actually emits, in preference order.
//
// A DEFAULT THAT NAMES A MASK NOBODY DERIVED IS WORSE THAN A BLUNT ONE: the profile it is
// written into looks correct in the ini and in the combo, and the engine then has nothing to
// look up when the game starts. So the answer is not composed, it is CHOSEN from the list
// that will exist. BaseGroups drops an empty group and DeriveMasks skips it too, which is the
// case that makes this more than a formality on a topology we have never seen.
//
// The last resorts, in order: the first mask that does exist (whatever it is called on this
// machine), then the raw first preference - by which point the machine reported no usable
// processors at all and there is nothing truthful left to say.
std::wstring FirstDerivedMask(const Topology& t, const std::vector<std::wstring>& wanted) {
    const std::vector<Mask> masks = DeriveMasks(t);
    for (size_t w = 0; w < wanted.size(); ++w) {
        for (size_t m = 0; m < masks.size(); ++m) {
            if (masks[m].name == wanted[w]) {
                return wanted[w];
            }
        }
    }
    if (!masks.empty()) {
        return masks[0].name;
    }
    return wanted.empty() ? std::wstring() : wanted[0];
}

// The default GAME mask for a topology whose first group is called `base`: the WHOLE group.
//
// It was `base + " no SMT"` until 2026-08-29 - see topology.h for the operator decision and
// the reasoning. The "no SMT" variant is kept as the first fallback rather than dropped,
// because on a machine where the plain group somehow does not exist the reduced one is still
// the same silicon and is a far better answer than "All".
std::wstring DefaultGameMaskFor(const Topology& t, const std::wstring& base) {
    std::vector<std::wstring> wanted;
    wanted.push_back(base);
    wanted.push_back(base + L" no SMT");
    wanted.push_back(L"All");
    return FirstDerivedMask(t, wanted);
}

// The default GAME mask for a machine with NO split at all, which is a different question and
// deliberately keeps the old answer.
//
// THE 2026-08-29 CHANGE DOES NOT REACH HERE, AND MUST NOT. Dropping "no SMT" from a machine
// that HAS a cache domain still leaves the game on that domain - the point of the product.
// Dropping it from a machine that has one domain leaves "All", which is every processor, which
// is byte-for-byte what having no assignment at all means: the shipped profile would be inert.
// firstrun.cpp says so to the user in as many words - "Only the SMT masks (\"All no SMT\")
// will change anything" - so defaulting to "All" here would also make the product contradict
// its own first-run text.
//
// "All no SMT" is omitted by DeriveMasks on a machine with no SMT, where it would equal its
// parent; the list below degrades to "All" for exactly that case, which is what the old
// PreferNoSmt did and is the only honest answer left on such a machine.
std::wstring DefaultGameMaskForWholeMachine(const Topology& t) {
    std::vector<std::wstring> wanted;
    wanted.push_back(L"All no SMT");
    wanted.push_back(L"All");
    return FirstDerivedMask(t, wanted);
}

}  // namespace

// ---------------------------------------------------------------------------
// DetectTopology - the only impure function in this file
// ---------------------------------------------------------------------------

bool DetectTopology(Topology& out, std::wstring* error) {
    out = Topology();

    // --- 1. GetSystemCpuSetInformation ------------------------------------
    ULONG cpuSetBytes = 0;
    GetSystemCpuSetInformation(nullptr, 0, &cpuSetBytes, nullptr, 0);
    if (cpuSetBytes == 0) {
        if (error != nullptr) {
            *error = L"GetSystemCpuSetInformation returned no data (error " +
                     std::to_wstring((unsigned long)GetLastError()) + L").";
        }
        return false;
    }

    std::vector<BYTE> cpuSetBuf(cpuSetBytes);
    if (!GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(cpuSetBuf.data()),
            cpuSetBytes, &cpuSetBytes, nullptr, 0)) {
        if (error != nullptr) {
            *error = L"GetSystemCpuSetInformation failed (error " +
                     std::to_wstring((unsigned long)GetLastError()) + L").";
        }
        return false;
    }

    {
        BYTE* p = cpuSetBuf.data();
        BYTE* end = p + cpuSetBytes;
        while (p + sizeof(ULONG) * 2 <= end) {
            PSYSTEM_CPU_SET_INFORMATION rec = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(p);
            if (rec->Size == 0 || p + rec->Size > end) {
                break;  // malformed; stop rather than walk off the buffer
            }
            if (rec->Type == CpuSetInformation) {
                CpuSetEntry e;
                e.Id                    = rec->CpuSet.Id;
                e.LogicalProcessorIndex = rec->CpuSet.LogicalProcessorIndex;
                e.CoreIndex             = rec->CpuSet.CoreIndex;
                e.LastLevelCacheIndex   = rec->CpuSet.LastLevelCacheIndex;
                e.NumaNodeIndex         = rec->CpuSet.NumaNodeIndex;
                e.Group                 = rec->CpuSet.Group;
                e.EfficiencyClass       = rec->CpuSet.EfficiencyClass;
                e.Parked                = (rec->CpuSet.Parked != 0);
                e.Allocated             = (rec->CpuSet.Allocated != 0);
                e.RealTime              = (rec->CpuSet.RealTime != 0);
                e.LastLevelCacheBytes   = 0;
                out.entries.push_back(e);
            }
            p += rec->Size;
        }
    }

    if (out.entries.empty()) {
        if (error != nullptr) {
            *error = L"No CPU Set records were reported by this system.";
        }
        return false;
    }

    std::sort(out.entries.begin(), out.entries.end(),
              [](const CpuSetEntry& a, const CpuSetEntry& b) {
                  if (a.Group != b.Group) {
                      return a.Group < b.Group;
                  }
                  return a.LogicalProcessorIndex < b.LogicalProcessorIndex;
              });

    // --- 2. GetLogicalProcessorInformationEx(RelationCache) ----------------
    DWORD lpiBytes = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &lpiBytes);
    if (lpiBytes == 0) {
        if (error != nullptr) {
            *error = L"GetLogicalProcessorInformationEx(RelationCache) returned no data (error " +
                     std::to_wstring((unsigned long)GetLastError()) + L").";
        }
        return false;
    }

    std::vector<BYTE> lpiBuf(lpiBytes);
    if (!GetLogicalProcessorInformationEx(
            RelationCache,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(lpiBuf.data()),
            &lpiBytes)) {
        if (error != nullptr) {
            *error = L"GetLogicalProcessorInformationEx(RelationCache) failed (error " +
                     std::to_wstring((unsigned long)GetLastError()) + L").";
        }
        return false;
    }

    struct CacheRec {
        BYTE      level;
        ULONGLONG bytes;
        WORD      group;
        KAFFINITY mask;
    };
    std::vector<CacheRec> caches;
    BYTE highestLevel = 0;
    {
        BYTE* p = lpiBuf.data();
        BYTE* end = p + lpiBytes;
        while (p + sizeof(LOGICAL_PROCESSOR_RELATIONSHIP) + sizeof(DWORD) <= end) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
            if (rec->Size == 0 || p + rec->Size > end) {
                break;
            }
            if (rec->Relationship == RelationCache) {
                // SDK 10.0.19041.0 CACHE_RELATIONSHIP has a single GroupMask and NO
                // GroupCount member - do not reach for fields from newer SDKs here.
                const CACHE_RELATIONSHIP& c = rec->Cache;
                CacheRec cr;
                cr.level = c.Level;
                cr.bytes = (ULONGLONG)c.CacheSize;
                cr.group = c.GroupMask.Group;
                cr.mask  = c.GroupMask.Mask;
                caches.push_back(cr);
                if (c.Level > highestLevel) {
                    highestLevel = c.Level;
                }
            }
            p += rec->Size;
        }
    }

    // Level 3 when present, otherwise the highest level this machine reports.
    BYTE targetLevel = 0;
    for (size_t i = 0; i < caches.size(); ++i) {
        if (caches[i].level == 3) {
            targetLevel = 3;
            break;
        }
    }
    if (targetLevel == 0) {
        targetLevel = highestLevel;
    }

    // A cache GroupMask bit is a processor number *within its group*. Subtracting each
    // group's lowest LogicalProcessorIndex makes the mapping correct whether the OS
    // numbers LogicalProcessorIndex per group (bases are 0) or globally (bases are the
    // group's first LP). On a single-group machine both readings are identical.
    std::map<WORD, ULONG> groupBase;
    for (size_t i = 0; i < out.entries.size(); ++i) {
        const CpuSetEntry& e = out.entries[i];
        std::map<WORD, ULONG>::iterator it = groupBase.find(e.Group);
        if (it == groupBase.end()) {
            groupBase.insert(std::make_pair(e.Group, e.LogicalProcessorIndex));
        } else if (e.LogicalProcessorIndex < it->second) {
            it->second = e.LogicalProcessorIndex;
        }
    }
    // (group, LogicalProcessorIndex) -> index into out.entries
    std::map<std::pair<WORD, ULONG>, size_t> byGroupLp;
    for (size_t i = 0; i < out.entries.size(); ++i) {
        byGroupLp[std::make_pair(out.entries[i].Group, out.entries[i].LogicalProcessorIndex)] = i;
    }

    if (targetLevel != 0) {
        for (size_t i = 0; i < caches.size(); ++i) {
            const CacheRec& cr = caches[i];
            if (cr.level != targetLevel) {
                continue;
            }
            std::map<WORD, ULONG>::const_iterator gb = groupBase.find(cr.group);
            const ULONG base = (gb == groupBase.end()) ? 0u : gb->second;
            for (unsigned bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                if ((cr.mask & (((KAFFINITY)1) << bit)) == 0) {
                    continue;
                }
                const ULONG lp = base + (ULONG)bit;
                std::map<std::pair<WORD, ULONG>, size_t>::const_iterator it =
                    byGroupLp.find(std::make_pair(cr.group, lp));
                if (it == byGroupLp.end()) {
                    continue;
                }
                CpuSetEntry& e = out.entries[it->second];
                if (cr.bytes > e.LastLevelCacheBytes) {
                    e.LastLevelCacheBytes = cr.bytes;
                }
            }
        }
    }

    // --- 3./4. domains, grouped by LastLevelCacheIndex ---------------------
    {
        std::map<ULONG, LlcDomain> byLlc;
        for (size_t i = 0; i < out.entries.size(); ++i) {
            const CpuSetEntry& e = out.entries[i];
            LlcDomain& d = byLlc[e.LastLevelCacheIndex];
            d.index = e.LastLevelCacheIndex;
            d.lps.push_back(e.LogicalProcessorIndex);
            if (e.LastLevelCacheBytes > d.l3Bytes) {
                d.l3Bytes = e.LastLevelCacheBytes;
            }
        }
        out.domains.clear();
        out.domains.reserve(byLlc.size());
        for (std::map<ULONG, LlcDomain>::iterator it = byLlc.begin(); it != byLlc.end(); ++it) {
            std::sort(it->second.lps.begin(), it->second.lps.end());
            out.domains.push_back(it->second);
        }
        std::sort(out.domains.begin(), out.domains.end(),
                  [](const LlcDomain& a, const LlcDomain& b) { return a.index < b.index; });
    }

    // --- 5. counts ---------------------------------------------------------
    out.totalLogicalProcessors = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    out.groupCount = (USHORT)GetActiveProcessorGroupCount();
    if (out.groupCount == 0) {
        out.groupCount = 1;
    }

    // --- 6. classify -------------------------------------------------------
    ClassifyTopology(out);
    return true;
}

// ---------------------------------------------------------------------------
// ClassifyTopology - pure
// ---------------------------------------------------------------------------

void ClassifyTopology(Topology& t) {
    const std::vector<BYTE> classes = EfficiencyClasses(t);
    const size_t domainCount = t.domains.size();

    bool l3Differs = false;
    if (domainCount >= 2) {
        const ULONGLONG first = t.domains[0].l3Bytes;
        for (size_t i = 1; i < domainCount; ++i) {
            if (t.domains[i].l3Bytes != first) {
                l3Differs = true;
                break;
            }
        }
    }

    // Decision table, first match wins. Order is load-bearing: an Intel hybrid part also
    // has several LLC domains, and must not be classified by cache size.
    if (classes.size() > 1) {
        t.kind = TopologyKind::IntelHybrid;
        t.confidence = Confidence::High;
    } else if (domainCount >= 2 && l3Differs) {
        t.kind = TopologyKind::AmdAsymmetricCache;
        t.confidence = Confidence::High;
    } else if (domainCount >= 2) {
        t.kind = TopologyKind::MultiCcdSymmetric;
        t.confidence = Confidence::Medium;
    } else if (domainCount == 1) {
        t.kind = TopologyKind::SingleDomain;
        t.confidence = Confidence::None;
    } else {
        t.kind = TopologyKind::Unknown;
        t.confidence = Confidence::None;
    }

    // --- signature ---------------------------------------------------------
    // "<vendor>:<domainCount>:<lps per domain,>:<L3 KB per domain,>", domains ascending
    // by index. Cheap to compare and stable across runs on the same machine.
    {
        std::wstring vendor;
        if (classes.size() > 1) {
            vendor = L"intel";
        } else if (CpuBrandIsAmd()) {
            vendor = L"amd";
        } else {
            vendor = L"cpu";
        }

        std::wstring lpPart;
        std::wstring kbPart;
        for (size_t i = 0; i < domainCount; ++i) {
            if (i != 0) {
                lpPart += L",";
                kbPart += L",";
            }
            lpPart += std::to_wstring((unsigned long long)t.domains[i].lps.size());
            kbPart += std::to_wstring((unsigned long long)(t.domains[i].l3Bytes / 1024ull));
        }
        t.signature = vendor + L":" + std::to_wstring((unsigned long long)domainCount) +
                      L":" + lpPart + L":" + kbPart;
    }

    // --- summary + defaults ------------------------------------------------
    const std::vector<LabeledGroup> groups = BaseGroups(t, t.kind);

    if (t.kind == TopologyKind::IntelHybrid && groups.size() >= 2) {
        t.summary = std::to_wstring((unsigned long long)groups.size()) + L" core types: ";
        for (size_t i = 0; i < groups.size(); ++i) {
            if (i != 0) {
                t.summary += L", ";
            }
            t.summary += std::to_wstring((unsigned long long)groups[i].lpCount);
            t.summary += (i == 0) ? L" logical processors are " : L" are ";
            t.summary += groups[i].label;
        }
        t.summary += L".";
        t.defaultGameMask  = DefaultGameMaskFor(t, groups[0].label);
        t.defaultHeavyMask = groups[1].label;
    } else if ((t.kind == TopologyKind::AmdAsymmetricCache ||
                t.kind == TopologyKind::MultiCcdSymmetric) && groups.size() >= 2) {
        t.summary = std::to_wstring((unsigned long long)groups.size()) + L" core groups: ";
        for (size_t i = 0; i < groups.size(); ++i) {
            if (i != 0) {
                t.summary += L", ";
            }
            t.summary += std::to_wstring((unsigned long long)groups[i].lpCount);
            if (i == 0) {
                t.summary += L" logical processors";
            }
            const std::wstring sz = FormatCacheSize(groups[i].l3Bytes);
            if (!sz.empty()) {
                t.summary += L" with " + sz + L" L3";
            }
            t.summary += L" (" + groups[i].label + L")";
        }
        t.summary += L".";
        t.defaultGameMask  = DefaultGameMaskFor(t, groups[0].label);
        t.defaultHeavyMask = groups[1].label;
    } else if (t.kind == TopologyKind::SingleDomain) {
        const std::wstring sz = FormatCacheSize(t.domains[0].l3Bytes);
        t.summary = L"1 core group: " +
                    std::to_wstring((unsigned long long)t.domains[0].lps.size()) +
                    L" logical processors";
        if (!sz.empty()) {
            t.summary += L" sharing " + sz + L" L3";
        }
        t.summary += L". No cache or core-type split was found on this CPU, "
                     L"so only SMT reduction is available.";
        t.defaultGameMask  = DefaultGameMaskForWholeMachine(t);
        t.defaultHeavyMask = L"All";
    } else {
        t.summary = L"No usable CPU topology was reported, so only the full processor set "
                    L"is available.";
        t.defaultGameMask  = DefaultGameMaskForWholeMachine(t);
        t.defaultHeavyMask = L"All";
    }
}

// ---------------------------------------------------------------------------
// DeriveMasks - pure
// ---------------------------------------------------------------------------

std::vector<Mask> DeriveMasks(const Topology& t) {
    std::vector<Mask> out;
    const std::vector<LabeledGroup> groups = BaseGroups(t, t.kind);

    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].ids.empty()) {
            continue;
        }
        Mask m;
        m.name = groups[i].label;
        m.ids = groups[i].ids;
        m.derived = true;
        out.push_back(m);

        const std::vector<ULONG> reduced = ReduceToNoSmt(t, groups[i].ids);
        if (reduced != groups[i].ids && !reduced.empty()) {
            Mask n;
            n.name = groups[i].label + L" no SMT";
            n.ids = reduced;
            n.derived = true;
            out.push_back(n);
        }
    }

    const std::vector<ULONG> all = AllIds(t);
    if (!all.empty()) {
        Mask m;
        m.name = L"All";
        m.ids = all;
        m.derived = true;
        out.push_back(m);

        const std::vector<ULONG> reduced = ReduceToNoSmt(t, all);
        if (reduced != all && !reduced.empty()) {
            Mask n;
            n.name = L"All no SMT";
            n.ids = reduced;
            n.derived = true;
            out.push_back(n);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// IsDerivableMaskName - pure
// ---------------------------------------------------------------------------

// Case-insensitive ordinal equality, the comparison util.cpp's IEquals makes. Spelled out here
// because this TU does not depend on util.h, and a new include for one call is how header
// cycles start.
static bool NameEqualsNoCase(const std::wstring& a, const wchar_t* b) {
    return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1, TRUE) ==
           CSTR_EQUAL;
}

// `prefix` (compared case-insensitively) followed by one or more ASCII decimal digits and
// nothing else. Shared by the "CCD" + index and "Freq " + index branches below. The trailing
// space is part of the Freq prefix and is NOT part of the CCD one, because BaseGroups builds
// the two names differently: L"CCD" + to_wstring(i) at :225 versus L"Freq " + to_wstring(seq)
// at :204. Passing the prefix in verbatim keeps that difference in one place.
static bool PrefixThenDigitsNoCase(const std::wstring& s, const wchar_t* prefix,
                                   size_t prefixLen) {
    if (s.size() <= prefixLen) return false;  // prefix alone is not a name DeriveMasks emits
    if (!NameEqualsNoCase(s.substr(0, prefixLen), prefix)) return false;
    for (size_t i = prefixLen; i < s.size(); ++i) {
        if (s[i] < L'0' || s[i] > L'9') return false;
    }
    return true;
}

bool IsDerivableMaskName(const std::wstring& name) {
    // DeriveMasks emits "<label> no SMT" beside every domain label, so strip that suffix first
    // and test the base. THE BASE VOCABULARY IS THE COMPLETE SET OF LITERALS BaseGroups AND
    // DeriveMasks CAN PRODUCE - every one of them, or the founder's "reserve all" ruling is
    // only half implemented:
    //
    //   "All"                topology.cpp:678  (DeriveMasks, every machine)
    //   "P-cores"            topology.cpp:155  (IntelHybrid)
    //   "E-cores"            topology.cpp:157  (IntelHybrid)
    //   "Cache"              topology.cpp:200  (AmdAsymmetricCache, the largest-L3 domain)
    //   "Freq", "Freq 2", ... topology.cpp:203-204  (AmdAsymmetricCache, every OTHER domain;
    //                        built with a ternary, so a grep for `label = L"` cannot see it -
    //                        which is exactly how this family was missed the first time)
    //   "CCD0", "CCD1", ...  topology.cpp:225  (MultiCcdSymmetric)
    //
    // plus the " no SMT" suffix appended to any of the above at topology.cpp:663 (domain
    // labels) and :681 ("All no SMT").
    static const wchar_t kNoSmt[] = L" no SMT";
    const size_t noSmtLen = 7;  // wcslen(kNoSmt)
    std::wstring base = name;
    if (base.size() > noSmtLen &&
        NameEqualsNoCase(base.substr(base.size() - noSmtLen), kNoSmt)) {
        base.erase(base.size() - noSmtLen);
    }
    if (NameEqualsNoCase(base, L"All") || NameEqualsNoCase(base, L"Cache") ||
        NameEqualsNoCase(base, L"P-cores") || NameEqualsNoCase(base, L"E-cores") ||
        NameEqualsNoCase(base, L"Freq")) {
        return true;
    }
    // "CCD" followed by at least one ASCII decimal digit and nothing else. "CCD" alone, "CCDx"
    // and "CCD 0" are not names DeriveMasks can produce.
    if (PrefixThenDigitsNoCase(base, L"CCD", 3)) return true;
    // "Freq " - with the single space to_wstring is appended after - followed by at least one
    // digit and nothing else. "Freq2", "Freq x", "Freqx" and "Freq " alone are not emitted.
    return PrefixThenDigitsNoCase(base, L"Freq ", 5);
}

// ---------------------------------------------------------------------------
// ReduceToNoSmt - pure
// ---------------------------------------------------------------------------

std::vector<ULONG> ReduceToNoSmt(const Topology& t, const std::vector<ULONG>& ids) {
    // CoreIndex -> the entry with the lowest LogicalProcessorIndex seen for that core.
    std::map<ULONG, const CpuSetEntry*> best;
    for (size_t i = 0; i < ids.size(); ++i) {
        const CpuSetEntry* e = FindById(t, ids[i]);
        if (e == nullptr) {
            continue;  // an id this topology does not know has no CoreIndex
        }
        std::map<ULONG, const CpuSetEntry*>::iterator it = best.find(e->CoreIndex);
        if (it == best.end()) {
            best.insert(std::make_pair(e->CoreIndex, e));
        } else if (e->LogicalProcessorIndex < it->second->LogicalProcessorIndex) {
            it->second = e;
        }
    }

    std::vector<ULONG> out;
    out.reserve(best.size());
    for (std::map<ULONG, const CpuSetEntry*>::const_iterator it = best.begin();
         it != best.end(); ++it) {
        out.push_back(it->second->Id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// MaskNameForIds - pure
// ---------------------------------------------------------------------------

std::wstring MaskNameForIds(const std::vector<Mask>& masks, const std::vector<ULONG>& ids) {
    if (ids.empty()) {
        return std::wstring();   // "no assignment" is a state, not a mask; the caller names it
    }
    // BOTH sides go through SortedUnique, not just the readback. A mask's ids are normally
    // ascending and unique because DeriveMasks made them so - but config.ini is documented as
    // hand-editable, so a mask that came off disk can be neither, and a size comparison used
    // as a fast reject would then miss a set that genuinely matches.
    const std::vector<ULONG> want = SortedUnique(ids);
    for (size_t i = 0; i < masks.size(); ++i) {
        if (SortedUnique(masks[i].ids) == want) {
            return masks[i].name;
        }
    }
    return std::wstring();
}

// ---------------------------------------------------------------------------
// Lookups and conversions - pure
// ---------------------------------------------------------------------------

const CpuSetEntry* FindById(const Topology& t, ULONG id) {
    for (size_t i = 0; i < t.entries.size(); ++i) {
        if (t.entries[i].Id == id) {
            return &t.entries[i];
        }
    }
    return nullptr;
}

const CpuSetEntry* FindByLp(const Topology& t, ULONG lp) {
    for (size_t i = 0; i < t.entries.size(); ++i) {
        if (t.entries[i].LogicalProcessorIndex == lp) {
            return &t.entries[i];
        }
    }
    return nullptr;
}

std::vector<ULONG> IdsForLps(const Topology& t, const std::vector<ULONG>& lps) {
    std::vector<ULONG> out;
    out.reserve(lps.size());
    for (size_t i = 0; i < lps.size(); ++i) {
        const CpuSetEntry* e = FindByLp(t, lps[i]);
        if (e != nullptr) {
            out.push_back(e->Id);
        }
    }
    return SortedUnique(out);
}

std::vector<ULONG> LpsForIds(const Topology& t, const std::vector<ULONG>& ids) {
    std::vector<ULONG> out;
    out.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        const CpuSetEntry* e = FindById(t, ids[i]);
        if (e != nullptr) {
            out.push_back(e->LogicalProcessorIndex);
        }
    }
    return SortedUnique(out);
}

const wchar_t* KindName(TopologyKind k) {
    switch (k) {
        case TopologyKind::IntelHybrid:        return L"Intel hybrid (P/E cores)";
        case TopologyKind::AmdAsymmetricCache: return L"AMD asymmetric cache (X3D)";
        case TopologyKind::MultiCcdSymmetric:  return L"Multi-CCD symmetric";
        case TopologyKind::SingleDomain:       return L"Single cache domain";
        case TopologyKind::Unknown:            return L"Unknown";
    }
    return L"Unknown";
}

const wchar_t* ConfidenceName(Confidence c) {
    switch (c) {
        case Confidence::High:   return L"High";
        case Confidence::Medium: return L"Medium";
        case Confidence::None:   return L"None";
    }
    return L"None";
}

}  // namespace cd
