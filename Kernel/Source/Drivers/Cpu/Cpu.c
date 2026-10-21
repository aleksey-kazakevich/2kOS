#include <Drivers/Cpu/CpuIntel.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Cpu/Cpu.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <SysDevice.h>
#include <Asm/Cpu.h>
#include <Basecon.h>
#include <Return.h>

// ============================================================================
// State
// ============================================================================

STATIC CpuInfo GCpuInfo = {0};
STATIC const CpuBackend *GBackends[8] = {0};
STATIC UINT32 GBackendCount = 0;
STATIC const CpuBackend *GActiveBackend = NULLPTR;

// ============================================================================
// CPUID helper
// ============================================================================

STATIC VOID ReadVendorString(CHAR *Out, UINT32 OutSize) {
    UINT32 Eax, Ebx, Ecx, Edx;
    CpuidLeaf(0, 0, &Eax, &Ebx, &Ecx, &Edx);

    UINT32 Parts[3] = { Ebx, Edx, Ecx };
    INT Pos = 0;
    for (INT P = 0; P < 3; P++) {
        for (INT I = 0; I < 4; I++) {
            if (Pos < (INT)OutSize - 1)
                Out[Pos++] = (CHAR)((Parts[P] >> (I * 8)) & 0xFF);
        }
    }
    Out[Pos] = '\0';
}

// ============================================================================
// Backend registry
// ============================================================================

VOID CpuRegisterBackend(const CpuBackend *B) {
    if (!B || GBackendCount >= 8) return;
    GBackends[GBackendCount++] = B;
}

STATIC const CpuBackend *FindBackend(const CHAR *VendorStr) {
    for (UINT32 I = 0; I < GBackendCount; I++) {
        if (StrCmp(GBackends[I]->VendorString, VendorStr) == 0)
            return GBackends[I];
    }
    return NULLPTR;
}

// ============================================================================
// SysDevice
// ============================================================================

STATIC SysDevice *RegisterSysDevice(CpuInfo *Info) {
    SysDevice *Root = SysDeviceGetRoot();
    if (!Root) return NULLPTR;

    CHAR Name[SYSDEV_NAME_MAX];
    SnPrintf(Name, sizeof(Name), "CPU_%s", Info->ShortName);

    SysDevice *Dev = SysDeviceCreate(Root, Name, Info->Brand,
                                     SYSDEV_TYPE_CPU);
    if (!Dev) return NULLPTR;

    SysDeviceSetProp(Dev, "vendor", CpuVendorName(Info->Vendor));
    SysDeviceSetProp(Dev, "brand",  Info->Brand);
    SysDeviceSetProp(Dev, "short_name", Info->ShortName);
    SysDeviceSetPropU32(Dev, "family",   Info->Version.Family);
    SysDeviceSetPropU32(Dev, "model",    Info->Version.Model);
    SysDeviceSetPropU32(Dev, "stepping", Info->Version.Stepping);

    SysDeviceSetPropU32(Dev, "logical_cores",    Info->Topology.LogicalCores);
    SysDeviceSetPropU32(Dev, "physical_cores",   Info->Topology.PhysicalCores);
    SysDeviceSetPropU32(Dev, "threads_per_core", Info->Topology.ThreadsPerCore);
    SysDeviceSetPropU32(Dev, "packages",         Info->Topology.Packages);

    SysDeviceSetPropHex(Dev, "feat_edx",     Info->Features.Edx);
    SysDeviceSetPropHex(Dev, "feat_ecx",     Info->Features.Ecx);
    SysDeviceSetPropHex(Dev, "feat_ext_edx", Info->Features.ExtEdx);
    SysDeviceSetPropHex(Dev, "feat_ext_ecx", Info->Features.ExtEcx);
    SysDeviceSetPropHex(Dev, "feat_7_ebx",   Info->Features.Ebx7);
    SysDeviceSetPropHex(Dev, "feat_7_ecx",   Info->Features.Ecx7);

    SysDeviceSetPropU32(Dev, "base_freq_khz", Info->BaseFreqKhz);
    SysDeviceSetPropU32(Dev, "max_freq_khz",  Info->MaxFreqKhz);
    SysDeviceSetPropU64(Dev, "tsc_freq_hz",   Info->TscFreqHz);
    SysDeviceSetPropU32(Dev, "apic_id",       Info->ApicId);
    SysDeviceSetPropU32(Dev, "cache_count",   Info->CacheCount);

    if (Info->IsHypervisor)
        SysDeviceSetProp(Dev, "hypervisor", Info->HypervisorVendor);

    for (UINT32 I = 0; I < Info->CoreCount; I++) {
        CpuCoreInfo *C = &Info->Cores[I];

        CHAR CoreName[SYSDEV_NAME_MAX];
        SnPrintf(CoreName, sizeof(CoreName), "CPU%u", I);

        CHAR CoreDesc[SYSDEV_NAME_MAX];
        SnPrintf(CoreDesc, sizeof(CoreDesc),
                 "Core %u (APIC ID %u)", I, C->ApicId);

        SysDevice *CoreDev = SysDeviceCreate(Dev, CoreName, CoreDesc,
                                             SYSDEV_TYPE_CORE);
        if (!CoreDev) continue;

        SysDeviceSetPropU32(CoreDev, "index", I);
        SysDeviceSetPropU32(CoreDev, "apic_id", C->ApicId);
        SysDeviceSetPropU32(CoreDev, "package_id", C->PackageId);
        SysDeviceSetPropU32(CoreDev, "core_id",    C->CoreId);
        SysDeviceSetPropU32(CoreDev, "thread_id",  C->ThreadId);
        SysDeviceSetPropU32(CoreDev, "enabled",    C->Enabled ? 1 : 0);
        SysDeviceSetPropU32(CoreDev, "is_primary", C->IsPrimary ? 1 : 0);
        if (C->AcpiUid != 0xFFFFFFFF) {
            SysDeviceSetPropU32(CoreDev, "acpi_uid", C->AcpiUid);
        }

        SysDeviceSetState(CoreDev, C->Enabled ? SYSDEV_STATE_ENABLED
                                              : SYSDEV_STATE_DISABLED);
        C->SysDev = CoreDev;
    }

    SysDeviceSetState(Dev, SYSDEV_STATE_ENABLED);
    return Dev;
}

// ============================================================================
// Init
// ============================================================================

INT CpuInit(VOID) {
    MemSet(&GCpuInfo, 0, sizeof(GCpuInfo));

    // 1. Vendor
    ReadVendorString(GCpuInfo.VendorString, sizeof(GCpuInfo.VendorString));
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "cpu: vendor '%s'\n", GCpuInfo.VendorString);

    // 2. Find the backend
    GActiveBackend = FindBackend(GCpuInfo.VendorString);
    if (!GActiveBackend) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "cpu: no backend for vendor '%s'\n",
                      GCpuInfo.VendorString);
        GCpuInfo.Vendor = CPU_VENDOR_UNKNOWN;
        RETURN(NOT_SUPPORTED);
    }
    GCpuInfo.Vendor = (GActiveBackend == CpuIntelBackend())
                    ? CPU_VENDOR_INTEL
                    : CPU_VENDOR_UNKNOWN;

    // 3. The backend fills up CpuInfo
    INT Rc = GActiveBackend->Init(&GCpuInfo);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "cpu: backend init failed (%d)\n", Rc);
        RETURN(Rc);
    }

    CpuEnumerateCores(&GCpuInfo);

    // 4. We register in SysDevice
    GCpuInfo.SysDev = RegisterSysDevice(&GCpuInfo);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "cpu: %s (family %u, model %u, stepping %u)\n",
                  GCpuInfo.Brand,
                  GCpuInfo.Version.Family,
                  GCpuInfo.Version.Model,
                  GCpuInfo.Version.Stepping);

    RETURN(SUCCESS);
}

// ============================================================================
// Public getters
// ============================================================================

CpuInfo *CpuGet(VOID) { return &GCpuInfo; }
const CpuBackend *CpuGetBackend(VOID) { return GActiveBackend; }

const CHAR *CpuVendorName(CpuVendor V) {
    switch (V) {
        case CPU_VENDOR_INTEL:   return "Intel";
        case CPU_VENDOR_AMD:     return "AMD";
        case CPU_VENDOR_VIA:     return "VIA";
        case CPU_VENDOR_HYGON:   return "Hygon";
        case CPU_VENDOR_VIRTUAL: return "Virtual";
        default:                 return "Unknown";
    }
}

UINT32 CpuGetFrequencyKhz(VOID) {
    if (GActiveBackend && GActiveBackend->GetFrequencyKhz)
        return GActiveBackend->GetFrequencyKhz(&GCpuInfo);
    return 0;
}

UINT32 CpuGetTemperatureC(VOID) {
    if (GActiveBackend && GActiveBackend->GetTemperatureC)
        return GActiveBackend->GetTemperatureC(&GCpuInfo);
    return 0;
}

UINT32 CpuGetTdpWatts(VOID) {
    if (GActiveBackend && GActiveBackend->GetTdpWatts)
        return GActiveBackend->GetTdpWatts(&GCpuInfo);
    return 0;
}

BOOL CpuHasVmx(VOID) {
    if (GActiveBackend && GActiveBackend->HasVmx)
        return GActiveBackend->HasVmx(&GCpuInfo);
    return FALSE;
}

BOOL CpuHasTurboBoost(VOID) {
    if (GActiveBackend && GActiveBackend->HasTurboBoost)
        return GActiveBackend->HasTurboBoost(&GCpuInfo);
    return FALSE;
}

VOID CpuRegisterAllBackends(VOID) {
    CpuRegisterBackend(CpuIntelBackend());
}

VOID CpuEnumerateCores(CpuInfo *Info) {
    Info->CoreCount = 0;
    UINT32 BspApicId = Info->ApicId;

    UINT32 AcpiCount = AcpiGetProcessorCount();
    if (AcpiCount > 0) {
        for (UINT32 I = 0; I < AcpiCount && Info->CoreCount < CPU_MAX_CORES; I++) {
            const AcpiProcessor *Proc = AcpiGetProcessor(I);
            if (!Proc) continue;

            CpuCoreInfo *C = &Info->Cores[Info->CoreCount++];
            MemSet(C, 0, sizeof(*C));
            C->ApicId    = Proc->ApicId;
            C->AcpiUid   = Proc->AcpiProcessorUid;
            C->Enabled   = Proc->Enabled;
            C->IsPrimary = (Proc->ApicId == BspApicId);
            C->CoreId    = I;
        }
        return;
    }

    // Fallback — CPUID
    UINT32 N = Info->Topology.LogicalCores;
    if (N == 0) N = 1;
    for (UINT32 I = 0; I < N && Info->CoreCount < CPU_MAX_CORES; I++) {
        CpuCoreInfo *C = &Info->Cores[Info->CoreCount++];
        MemSet(C, 0, sizeof(*C));
        C->ApicId    = I;
        C->AcpiUid   = 0xFFFFFFFF;
        C->Enabled   = TRUE;
        C->IsPrimary = (I == BspApicId);
        C->CoreId    = I;
    }
}