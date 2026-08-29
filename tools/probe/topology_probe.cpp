// Ground-truth probe: dump Windows CPU Set + cache topology for this machine.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <vector>

static void dumpCpuSets() {
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, nullptr, 0);
    std::vector<BYTE> buf(size);
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), size, &size, nullptr, 0)) {
        printf("GetSystemCpuSetInformation FAILED err=%lu\n", GetLastError());
        return;
    }
    printf("=== CPU SETS (bytes=%lu) ===\n", size);
    printf("%-10s %-4s %-4s %-4s %-4s %-4s %-4s %-6s %-6s %-6s\n",
           "Id","LP","Core","LLC","NUMA","Grp","Eff","Parked","Alloc","RT");
    BYTE* p = buf.data();
    BYTE* end = p + size;
    int n = 0;
    while (p < end) {
        PSYSTEM_CPU_SET_INFORMATION i = (PSYSTEM_CPU_SET_INFORMATION)p;
        if (i->Type == CpuSetInformation) {
            printf("%-10lu %-4u %-4u %-4u %-4u %-4u %-4u %-6u %-6u %-6u\n",
                i->CpuSet.Id,
                i->CpuSet.LogicalProcessorIndex,
                i->CpuSet.CoreIndex,
                i->CpuSet.LastLevelCacheIndex,
                i->CpuSet.NumaNodeIndex,
                i->CpuSet.Group,
                i->CpuSet.EfficiencyClass,
                (unsigned)i->CpuSet.Parked,
                (unsigned)i->CpuSet.Allocated,
                (unsigned)i->CpuSet.RealTime);
            n++;
        }
        p += i->Size;
    }
    printf("total cpuset entries = %d\n\n", n);
}

static const char* relName(LOGICAL_PROCESSOR_RELATIONSHIP r) {
    switch (r) {
        case RelationProcessorCore: return "Core";
        case RelationNumaNode: return "Numa";
        case RelationCache: return "Cache";
        case RelationProcessorPackage: return "Package";
        case RelationGroup: return "Group";
        default: return "?";
    }
}

static void dumpLPI() {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &size);
    std::vector<BYTE> buf(size);
    if (!GetLogicalProcessorInformationEx(RelationAll,
            (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &size)) {
        printf("GetLogicalProcessorInformationEx FAILED err=%lu\n", GetLastError());
        return;
    }
    printf("=== LOGICAL PROCESSOR INFO EX ===\n");
    BYTE* p = buf.data();
    BYTE* end = p + size;
    while (p < end) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX i = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        if (i->Relationship == RelationCache) {
            CACHE_RELATIONSHIP& c = i->Cache;
            printf("Cache L%u type=%u size=%luKB line=%u  grp=%u mask=0x%016llx\n",
                (unsigned)c.Level, (unsigned)c.Type, (unsigned long)(c.CacheSize/1024), (unsigned)c.LineSize,
                (unsigned)c.GroupMask.Group,
                (unsigned long long)c.GroupMask.Mask);
        } else if (i->Relationship == RelationProcessorCore) {
            printf("Core flags=%u (SMT=%s) mask=0x%016llx\n",
                (unsigned)i->Processor.Flags,
                i->Processor.Flags == LTP_PC_SMT ? "yes" : "no",
                (unsigned long long)i->Processor.GroupMask[0].Mask);
        } else if (i->Relationship == RelationProcessorPackage) {
            printf("Package groups=%u mask=0x%016llx\n",
                (unsigned)i->Processor.GroupCount,
                (unsigned long long)i->Processor.GroupMask[0].Mask);
        } else {
            printf("%s\n", relName(i->Relationship));
        }
        p += i->Size;
    }
    printf("\n");
}

int main() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    printf("ActiveProcessorCount(ALL)=%u  GroupCount=%u\n",
        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), GetActiveProcessorGroupCount());
    dumpCpuSets();
    dumpLPI();
    return 0;
}
