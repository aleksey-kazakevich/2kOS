#include <Drivers/Cpu/CpuIntel.h>
#include <Drivers/Cpu/CpuRegs.h>
#include <Lib/String.h>
#include <Asm/Cpu.h>
#include <Basecon.h>
#include <Return.h>

// ============================================================================
// Private Intel backend data
// ============================================================================

typedef struct {
    UINT16 BaseFreqMhz;
    UINT16 MaxFreqMhz;
    UINT16 BusFreqMhz;
    UINT32 TdpWatts;
} IntelPrivate;

STATIC IntelPrivate GIntel = {0};

// ============================================================================
// Utilities
// ============================================================================

STATIC VOID ReadBrand(CHAR *Out, UINT32 OutSize) {
    UINT32 MaxExt, Eax, Ebx, Ecx, Edx;
    CpuidLeaf(CPUID_EXTENDED_MAX, 0, &MaxExt, &Ebx, &Ecx, &Edx);
    if (MaxExt < CPUID_BRAND_STRING_3) { Out[0] = '\0'; return; }

    UINT32 Buf[12];
    for (UINT32 L = CPUID_BRAND_STRING_1; L <= CPUID_BRAND_STRING_3; L++) {
        CpuidLeaf(L, 0, &Eax, &Ebx, &Ecx, &Edx);
        UINT32 Base = (L - CPUID_BRAND_STRING_1) * 4;
        Buf[Base + 0] = Eax;
        Buf[Base + 1] = Ebx;
        Buf[Base + 2] = Ecx;
        Buf[Base + 3] = Edx;
    }

    INT Pos = 0;
    for (INT I = 0; I < 48 && Pos < (INT)OutSize - 1; I++) {
        CHAR C = (CHAR)((Buf[I / 4] >> ((I % 4) * 8)) & 0xFF);
        if (C == '\0') continue;
        Out[Pos++] = C;
    }
    INT S = 0;
    while (S < Pos && Out[S] == ' ') S++;
    if (S) {
        for (INT I = 0; I < Pos - S; I++) Out[I] = Out[I + S];
        Pos -= S;
    }
    Out[Pos] = '\0';
}

// Extracts "i7-9750H" from "Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz"
STATIC VOID ExtractShortName(const CHAR *Brand, CHAR *Out, UINT32 OutSize) {
    STATIC const CHAR *Prefixes[] = {
        "i9-", "i7-", "i5-", "i3-",
        "E7-", "E5-", "E3-",
        "Platinum", "Gold", "Silver", "Bronze",
        "Celeron", "Pentium",
        NULLPTR
    };

    for (INT P = 0; Prefixes[P]; P++) {
        const CHAR *Hit = StrStr(Brand, Prefixes[P]);
        if (!Hit) continue;

        INT Pos = 0;

        // Copy to the space, '(' or the end
        while (Hit[Pos] && Hit[Pos] != ' ' && Hit[Pos] != '('
               && Pos < (INT)OutSize - 4) {
            Out[Pos] = Hit[Pos];
            Pos++;
        }

        // We check "v2"/"v3"/"v4" after the space
        const CHAR *P2 = Hit + Pos;
        while (*P2 == ' ') P2++;
        if (P2[0] == 'v' && P2[1] >= '0' && P2[1] <= '9') {
            Out[Pos++] = 'v';
            Out[Pos++] = P2[1];
            if (P2[2] >= '0' && P2[2] <= '9') {
                Out[Pos++] = P2[2];
            }
        }

        Out[Pos] = '\0';
        return;
    }

    // Fallback is the first word, but we stop at the space/parenthesis.
    INT Pos = 0;
    for (INT I = 0; Brand[I] && Pos < (INT)OutSize - 1; I++) {
        if (Brand[I] == ' ' || Brand[I] == '(') break;
        Out[Pos++] = Brand[I];
    }
    Out[Pos] = '\0';
}

STATIC VOID ReadVersionAndFeatures(CpuInfo *Info) {
    UINT32 Eax, Ebx, Ecx, Edx;
    CpuidLeaf(CPUID_FEATURES, 0, &Eax, &Ebx, &Ecx, &Edx);

    Info->Version.Stepping      = Eax & 0xF;
    Info->Version.BaseModel     = (Eax >> 4) & 0xF;
    Info->Version.BaseFamily    = (Eax >> 8) & 0xF;
    Info->Version.ProcessorType = (Eax >> 12) & 0x3;
    Info->Version.ExtModel      = (Eax >> 16) & 0xF;
    Info->Version.ExtFamily     = (Eax >> 20) & 0xFF;

    Info->Version.Family = Info->Version.BaseFamily;
    if (Info->Version.BaseFamily == 0xF)
        Info->Version.Family += Info->Version.ExtFamily;
    Info->Version.Model = Info->Version.BaseModel;
    if (Info->Version.BaseFamily == 6 || Info->Version.BaseFamily == 0xF)
        Info->Version.Model += (Info->Version.ExtModel << 4);

    Info->Features.Edx = Edx;
    Info->Features.Ecx = Ecx;
    Info->ApicId = (Ebx >> 24) & 0xFF;

    UINT32 MaxExt;
    CpuidLeaf(CPUID_EXTENDED_MAX, 0, &MaxExt, &Ebx, &Ecx, &Edx);
    if (MaxExt >= CPUID_EXT_FEATURES) {
        CpuidLeaf(CPUID_EXT_FEATURES, 0, &Eax, &Ebx, &Ecx, &Edx);
        Info->Features.ExtEdx = Edx;
        Info->Features.ExtEcx = Ecx;
    }

    UINT32 MaxLeaf;
    CpuidLeaf(0, 0, &MaxLeaf, &Ebx, &Ecx, &Edx);
    if (MaxLeaf >= CPUID_STRUCT_EXT_FEAT) {
        CpuidLeaf(CPUID_STRUCT_EXT_FEAT, 0, &Eax, &Ebx, &Ecx, &Edx);
        Info->Features.Ebx7 = Ebx;
        Info->Features.Ecx7 = Ecx;
    }
}

STATIC VOID ReadTscFrequency(CpuInfo *Info) {
    UINT32 MaxLeaf, Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &MaxLeaf, &Ebx, &Ecx, &Edx);
    if (MaxLeaf < 0x15) return;

    CpuidLeaf(0x15, 0, &Eax, &Ebx, &Ecx, &Edx);
    UINT32 Denom = Eax;
    UINT32 Numer = Ebx;
    UINT32 CrystalHz = Ecx;

    if (Denom != 0 && Numer != 0 && CrystalHz != 0) {
        Info->TscFreqHz = ((UINT64)CrystalHz * Numer) / Denom;
    }
}

STATIC VOID ReadCacheInfo(CpuInfo *Info) {
    UINT32 Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &Eax, &Ebx, &Ecx, &Edx);
    UINT32 MaxBasic = Eax;

    if (MaxBasic < CPUID_CACHE_PARAMS) return;

    for (UINT32 Index = 0; ; Index++) {
        CpuidLeaf(CPUID_CACHE_PARAMS, Index, &Eax, &Ebx, &Ecx, &Edx);
        UINT32 Type = Eax & 0x1F;
        if (Type == 0) break;
        if (Info->CacheCount >= CPU_MAX_CACHES) break;

        CpuCacheInfo *C = &Info->Caches[Info->CacheCount++];

        UINT32 Level = (Eax >> 5) & 0x7;
        switch (Level) {
            case 1: C->Level = (Type == 2) ? CPU_CACHE_L1I : CPU_CACHE_L1D; break;
            case 2: C->Level = CPU_CACHE_L2; break;
            case 3: C->Level = CPU_CACHE_L3; break;
            default: C->Level = (CpuCacheLevel)Level; break;
        }

        UINT32 LineSize   = (Ebx & 0xFFF) + 1;
        UINT32 Partitions = ((Ebx >> 12) & 0x3FF) + 1;
        UINT32 Ways       = ((Ebx >> 22) & 0x3FF) + 1;
        UINT32 Sets       = Ecx + 1;

        C->LineSize = LineSize;
        C->Ways     = Ways;
        C->Sets     = Sets;
        C->Size     = Ways * Partitions * LineSize * Sets;
        C->Sharing  = ((Eax >> 14) & 0xFFF) + 1;
    }
}

STATIC VOID ReadTopology(CpuInfo *Info) {
    UINT32 Eax, Ebx, Ecx, Edx;
    CpuidLeaf(CPUID_FEATURES, 0, &Eax, &Ebx, &Ecx, &Edx);
    UINT32 MaxLogical = (Ebx >> 16) & 0xFF;
    if (MaxLogical == 0) MaxLogical = 1;

    Info->Topology.LogicalCores = MaxLogical;
    Info->Topology.ThreadsPerCore = (Info->Features.Edx & CPUID_FEAT_EDX_HTT) ? 2 : 1;
    Info->Topology.PhysicalCores = MaxLogical / Info->Topology.ThreadsPerCore;
    if (Info->Topology.PhysicalCores == 0) Info->Topology.PhysicalCores = 1;
    Info->Topology.Packages = 1;
    Info->Topology.CoresPerPackage = Info->Topology.PhysicalCores;
}

STATIC VOID ReadFrequencies(CpuInfo *Info) {
    UINT32 MaxLeaf, Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &MaxLeaf, &Ebx, &Ecx, &Edx);
    if (MaxLeaf < 0x16) return;

    CpuidLeaf(0x16, 0, &Eax, &Ebx, &Ecx, &Edx);
    GIntel.BaseFreqMhz = Eax & 0xFFFF;
    GIntel.MaxFreqMhz  = Ebx & 0xFFFF;
    GIntel.BusFreqMhz  = Ecx & 0xFFFF;

    Info->BaseFreqKhz = (UINT32)GIntel.BaseFreqMhz * 1000;
    Info->MaxFreqKhz  = (UINT32)GIntel.MaxFreqMhz * 1000;
}

STATIC VOID ReadTdp(CpuInfo *Info) {
    UINT32 MaxLeaf, Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &MaxLeaf, &Ebx, &Ecx, &Edx);

    if (MaxLeaf >= CPUID_THERMAL_POWER) {
        CpuidLeaf(CPUID_THERMAL_POWER, 0, &Eax, &Ebx, &Ecx, &Edx);
        if (Eax & (1u << 14)) {
            UINT32 Tdp = Eax & 0x7F;
            Info->TdpWatts = Tdp;
            GIntel.TdpWatts = Tdp;
        }
    }
}

// ============================================================================
// Intel backend implementation
// ============================================================================

STATIC INT IntelProbe(CpuInfo *Info) {
    if (StrCmp(Info->VendorString, "GenuineIntel") != 0)
        RETURN(NOT_SUPPORTED);
    RETURN(SUCCESS);
}

STATIC INT IntelInit(CpuInfo *Info) {
    INT Rc = IntelProbe(Info);
    if (Rc != SUCCESS) return Rc;

    UINT32 Eax, Ebx, Ecx, Edx;
    CpuidLeaf(CPUID_FEATURES, 0, &Eax, &Ebx, &Ecx, &Edx);
    if (Ecx & CPUID_FEAT_ECX_HYPERVISOR) {
        Info->IsHypervisor = TRUE;
        CpuidLeaf(0x40000000, 0, &Eax, &Ebx, &Ecx, &Edx);
        UINT32 Parts[3] = { Ebx, Ecx, Edx };
        INT Pos = 0;
        for (INT P = 0; P < 3; P++) {
            for (INT I = 0; I < 4; I++) {
                if (Pos < 15)
                    Info->HypervisorVendor[Pos++] = (CHAR)((Parts[P] >> (I*8)) & 0xFF);
            }
        }
        Info->HypervisorVendor[Pos] = '\0';
    }

    ReadVersionAndFeatures(Info);
    ReadBrand(Info->Brand, sizeof(Info->Brand));
    ExtractShortName(Info->Brand, Info->ShortName, sizeof(Info->ShortName));
    ReadTopology(Info);
    ReadCacheInfo(Info);
    ReadTscFrequency(Info);
    ReadFrequencies(Info);
    ReadTdp(Info);

    Info->BackendData = &GIntel;

    if (Info->TscFreqHz == 0 && Info->MaxFreqKhz) {
        Info->TscFreqHz = (UINT64)Info->MaxFreqKhz * 1000ULL;
    }

    RETURN(SUCCESS);
}

STATIC UINT32 IntelGetFrequencyKhz(const CpuInfo *Info) {
    (VOID)Info;
    UINT64 Msr = ReadMSR(MSR_IA32_PERF_STATUS);
    UINT32 Multiplier = (UINT32)((Msr >> 8) & 0xFF);

    if (Multiplier == 0 || GIntel.BusFreqMhz == 0) {
        return Info->MaxFreqKhz;
    }
    return Multiplier * GIntel.BusFreqMhz * 1000;
}

STATIC UINT32 IntelGetTemperatureC(const CpuInfo *Info) {
    (VOID)Info;
    UINT64 Msr = ReadMSR(MSR_IA32_THERM_STATUS);
    if (!(Msr & (1ULL << 31))) return 0;

    UINT32 TjMax = 100;
    UINT64 Target = ReadMSR(MSR_IA32_TEMPERATURE_TARGET);
    UINT32 TargetValue = (Target >> 16) & 0xFF;
    if (TargetValue) TjMax = TargetValue;

    UINT32 Reading = (Msr >> 16) & 0x7F;
    if (Reading > TjMax) return 0;
    return TjMax - Reading;
}

STATIC UINT32 IntelGetTdpWatts(const CpuInfo *Info) {
    (VOID)Info;
    return GIntel.TdpWatts;
}

STATIC BOOL IntelHasVmx(const CpuInfo *Info) {
    return (Info->Features.Ecx & CPUID_FEAT_ECX_VMX) != 0;
}

STATIC BOOL IntelHasTurboBoost(const CpuInfo *Info) {
    (VOID)Info;
    UINT32 MaxLeaf, Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &MaxLeaf, &Ebx, &Ecx, &Edx);
    if (MaxLeaf < CPUID_THERMAL_POWER) return FALSE;
    CpuidLeaf(CPUID_THERMAL_POWER, 0, &Eax, &Ebx, &Ecx, &Edx);
    return (Eax & (1u << 1)) != 0;
}

// ============================================================================
// Entry point
// ============================================================================

STATIC const CpuBackend GIntelBackend = {
    .VendorString    = "GenuineIntel",
    .Init            = IntelInit,
    .GetFrequencyKhz = IntelGetFrequencyKhz,
    .GetTemperatureC = IntelGetTemperatureC,
    .GetTdpWatts     = IntelGetTdpWatts,
    .HasVmx          = IntelHasVmx,
    .HasTurboBoost   = IntelHasTurboBoost,
};

const CpuBackend *CpuIntelBackend(VOID) {
    return &GIntelBackend;
}