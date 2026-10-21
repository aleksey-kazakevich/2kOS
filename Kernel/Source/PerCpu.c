#include <Lib/String.h>
#include <PerCpu.h>
#include <Types.h>

PerCpu GPerCpu[MAX_CPUS];
UINT32 GCpuCount = 0;

PerCpu *PerCpuGetByLapicId(UINT32 LapicId) {
    for (UINT32 I = 0; I < GCpuCount; I++) {
        if (GPerCpu[I].LapicId == LapicId) {
            return &GPerCpu[I];
        }
    }
    return NULLPTR;
}

PerCpu *PerCpuGetByIndex(UINT32 Index) {
    if (Index >= GCpuCount) return NULLPTR;
    return &GPerCpu[Index];
}

UINT32 PerCpuGetCount(VOID) {
    return GCpuCount;
}

UINT32 PerCpuGetOnlineCount(VOID) {
    UINT32 N = 0;
    for (UINT32 I = 0; I < GCpuCount; I++) {
        if (GPerCpu[I].Online) N++;
    }
    return N;
}

PerCpu *PerCpuGetFirst(VOID) {
    if (GCpuCount == 0) return NULLPTR;
    return &GPerCpu[0];
}

PerCpu *PerCpuGetLeastLoaded(VOID) {
    PerCpu *Best = NULLPTR;
    UINT64 MinLoad = UINT64MAX;

    for (UINT32 I = 0; I < GCpuCount; I++) {
        PerCpu *P = &GPerCpu[I];
        if (!P->Online) continue;
        /* __atomic_load_n — Load is readable without lock */
        UINT64 L = __atomic_load_n(&P->Load, __ATOMIC_RELAXED);
        if (L < MinLoad) {
            MinLoad = L;
            Best = P;
        }
    }
    return Best;
}