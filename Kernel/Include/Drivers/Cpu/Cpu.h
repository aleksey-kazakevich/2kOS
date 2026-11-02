#pragma once

#include <SysDevice.h>
#include <Types.h>

// ============================================================================
// Vendors
// ============================================================================

typedef enum {
    CPU_VENDOR_UNKNOWN = 0,
    CPU_VENDOR_INTEL,
    CPU_VENDOR_AMD,
    CPU_VENDOR_VIA,
    CPU_VENDOR_HYGON,
    CPU_VENDOR_VIRTUAL,   // QEMU/KVM/VMware/Hyper-V
} CpuVendor;

// ============================================================================
// Version, features, topology, cache
// ============================================================================

typedef struct {
    UINT8  Stepping, BaseModel, BaseFamily, ProcessorType;
    UINT8  ExtModel, ExtFamily;
    UINT16 Family;
    UINT16 Model;
} CpuVersion;

typedef struct {
    UINT32 Edx, Ecx;         // CPUID.1
    UINT32 ExtEdx, ExtEcx;   // CPUID.80000001
    UINT32 Ebx7, Ecx7;       // CPUID.7
} CpuFeatures;

typedef struct {
    UINT32 LogicalCores;
    UINT32 PhysicalCores;
    UINT32 Packages;
    UINT32 ThreadsPerCore;
    UINT32 CoresPerPackage;
} CpuTopology;

typedef enum {
    CPU_CACHE_L1D = 1, CPU_CACHE_L1I = 2,
    CPU_CACHE_L2  = 3, CPU_CACHE_L3  = 4,
} CpuCacheLevel;

typedef struct {
    CpuCacheLevel Level;
    UINT32 Size;
    UINT32 Ways;
    UINT32 LineSize;
    UINT32 Sets;
    UINT32 Sharing;
} CpuCacheInfo;

typedef struct {
    UINT32 ApicId;         // LAPIC ID
    UINT32 AcpiUid;        // ACPI Processor UID (from MADT), if known
    BOOL   Enabled;        // is the kernel enabled (from MADT/CPUID)
    BOOL   IsPrimary;      // BSP (bootstrap processor)
    UINT32 PackageId;      // socket (0 if unknown)
    UINT32 CoreId;         // the physical core
    UINT32 ThreadId;       // logical thread inside the core (0 if there is no HT)
    SysDevice *SysDev;     // the SysDevice of this kernel
} CpuCoreInfo;

#define CPU_MAX_CACHES   8
#define CPU_BRAND_MAX    64
#define CPU_VENDOR_MAX   16
#define CPU_SHORTNAME_MAX 48
#define CPU_MAX_CORES 128

// ============================================================================
// General structure (filled in by the backend)
// ============================================================================

typedef struct {
    CpuVendor  Vendor;
    CHAR       VendorString[CPU_VENDOR_MAX];
    CHAR       Brand[CPU_BRAND_MAX];
    CHAR       ShortName[CPU_SHORTNAME_MAX];

    CpuVersion  Version;
    CpuFeatures Features;
    CpuTopology Topology;

    CpuCoreInfo Cores[CPU_MAX_CORES];
    UINT32      CoreCount;

    CpuCacheInfo Caches[CPU_MAX_CACHES];
    UINT32       CacheCount;

    UINT32 BaseFreqKhz;
    UINT32 MaxFreqKhz;
    UINT64 TscFreqHz;

    UINT32 TdpWatts;

    UINT32 ApicId;
    BOOL   IsHypervisor;
    CHAR   HypervisorVendor[16];

    VOID  *BackendData;       // private backend data

    SysDevice *SysDev;
} CpuInfo;

// ============================================================================
// The backend table is what each vendor implements.
// ============================================================================

typedef struct CpuBackend {
    const CHAR *VendorString;                    // "GenuineIntel", "AuthenticAMD", etc.

    // Initialization: fill in CpuInfo (version, features, topology, cache, brand).
    INT  (*Init)(CpuInfo *Info);

    // Optional features. There may be NULLPTRs.
    UINT32 (*GetFrequencyKhz)(const CpuInfo *Info);   // current frequency
    UINT32 (*GetTemperatureC)(const CpuInfo *Info);   // temperature
    UINT32 (*GetTdpWatts)(const CpuInfo *Info);
    BOOL   (*HasVmx)(const CpuInfo *Info);
    BOOL   (*HasTurboBoost)(const CpuInfo *Info);
} CpuBackend;

// ============================================================================
// Public API (the same for all vendors)
// ============================================================================

INT       CpuInit(VOID);
VOID      CpuRegisterAllBackends(VOID);
CpuInfo  *CpuGet(VOID);
const CpuBackend *CpuGetBackend(VOID);

const CHAR *CpuVendorName(CpuVendor V);

// Universal getters (pulling the backend):
UINT32 CpuGetFrequencyKhz(VOID);
UINT32 CpuGetTemperatureC(VOID);
UINT32 CpuGetTdpWatts(VOID);
BOOL   CpuHasVmx(VOID);
BOOL   CpuHasTurboBoost(VOID);

// Feature helpers (without backend — just read CpuInfo):
STATIC INLINE BOOL CpuHasEdx(UINT32 Bit) {
    return (CpuGet()->Features.Edx & Bit) != 0;
}
STATIC INLINE BOOL CpuHasEcx(UINT32 Bit) {
    return (CpuGet()->Features.Ecx & Bit) != 0;
}
STATIC INLINE BOOL CpuHasExtEdx(UINT32 Bit) {
    return (CpuGet()->Features.ExtEdx & Bit) != 0;
}
STATIC INLINE BOOL CpuHasEbx7(UINT32 Bit) {
    return (CpuGet()->Features.Ebx7 & Bit) != 0;
}

// Backend registration (called from CPUIntel.c / CpuAmd.c at the start)
VOID CpuRegisterBackend(const CpuBackend *Backend);

VOID CpuEnumerateCores(CpuInfo *Info);