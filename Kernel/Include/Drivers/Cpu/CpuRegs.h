#pragma once

#include <Types.h>

// ============================================================================
// CPUID leaves — common for x86
// ============================================================================

#define CPUID_VENDOR            0x00000000
#define CPUID_FEATURES          0x00000001
#define CPUID_CACHE_TLB         0x00000002
#define CPUID_SERIAL            0x00000003
#define CPUID_CACHE_PARAMS      0x00000004
#define CPUID_MONITOR_MWAIT     0x00000005
#define CPUID_THERMAL_POWER     0x00000006
#define CPUID_STRUCT_EXT_FEAT   0x00000007
#define CPUID_DIRECT_CACHE      0x00000018
#define CPUID_EXT_TOPOLOGY      0x0000001F

#define CPUID_EXTENDED_MAX      0x80000000
#define CPUID_EXT_FEATURES      0x80000001
#define CPUID_BRAND_STRING_1    0x80000002
#define CPUID_BRAND_STRING_2    0x80000003
#define CPUID_BRAND_STRING_3    0x80000004
#define CPUID_EXT_TOPOLOGY_2    0x8000001E
#define CPUID_EXT_APIC          0x80000008

// ============================================================================
// MSR — General (Intel/AMD)
// ============================================================================

#define MSR_IA32_PLATFORM_ID            0x00000017
#define MSR_IA32_APIC_BASE              0x0000001B
#define MSR_IA32_FEATURE_CONTROL        0x0000003A
#define MSR_IA32_MISC_ENABLE            0x000001A0
#define MSR_IA32_DEBUGCTL               0x000001D9
#define MSR_IA32_PERF_CAPABILITIES      0x00000345
#define MSR_IA32_PERF_STATUS            0x00000198
#define MSR_IA32_PERF_CTL               0x00000199
#define MSR_IA32_THERM_STATUS           0x0000019C
#define MSR_IA32_TEMPERATURE_TARGET     0x000001A2
#define MSR_IA32_PACKAGE_THERM_STATUS   0x000001B1
#define MSR_IA32_MPERF                  0x000000E7
#define MSR_IA32_APERF                  0x000000E8
#define MSR_IA32_ENERGY_PERF_BIAS       0x000001B0
#define MSR_IA32_ARCH_CAPABILITIES      0x0000010A
#define MSR_IA32_FLUSH_CMD              0x0000010B

// MTRR
#define MSR_MTRR_CAP                    0x000000FE
#define MSR_MTRR_DEF_TYPE               0x000002FF

// System Entries
#define MSR_SYSENTER_CS                 0x00000174
#define MSR_SYSENTER_ESP                0x00000175
#define MSR_SYSENTER_EIP                0x00000176

// Machine Check
#define MSR_MCG_CAP                     0x00000179
#define MSR_MCG_STATUS                  0x0000017A

// Turbo / P-State
#define MSR_IA32_PLATFORM_INFO          0x000000CE
#define MSR_IA32_PM_ENABLE              0x00000770
#define MSR_IA32_HWP_CAPABILITIES       0x00000771
#define MSR_IA32_HWP_REQUEST            0x00000774
#define MSR_IA32_HWP_STATUS             0x00000777

// ============================================================================
// CPUID.1:EDX — Standard Feature Flags
// ============================================================================

#define CPUID_FEAT_EDX_FPU          (1u << 0)
#define CPUID_FEAT_EDX_VME          (1u << 1)
#define CPUID_FEAT_EDX_DE           (1u << 2)
#define CPUID_FEAT_EDX_PSE          (1u << 3)
#define CPUID_FEAT_EDX_TSC          (1u << 4)
#define CPUID_FEAT_EDX_MSR          (1u << 5)
#define CPUID_FEAT_EDX_PAE          (1u << 6)
#define CPUID_FEAT_EDX_MCE          (1u << 7)
#define CPUID_FEAT_EDX_CX8          (1u << 8)
#define CPUID_FEAT_EDX_APIC         (1u << 9)
#define CPUID_FEAT_EDX_SEP          (1u << 11)
#define CPUID_FEAT_EDX_MTRR         (1u << 12)
#define CPUID_FEAT_EDX_PGE          (1u << 13)
#define CPUID_FEAT_EDX_MCA          (1u << 14)
#define CPUID_FEAT_EDX_CMOV         (1u << 15)
#define CPUID_FEAT_EDX_PAT          (1u << 16)
#define CPUID_FEAT_EDX_PSE36        (1u << 17)
#define CPUID_FEAT_EDX_PSN          (1u << 18)
#define CPUID_FEAT_EDX_CLFLUSH      (1u << 19)
#define CPUID_FEAT_EDX_DS           (1u << 21)
#define CPUID_FEAT_EDX_ACPI         (1u << 22)
#define CPUID_FEAT_EDX_MMX          (1u << 23)
#define CPUID_FEAT_EDX_FXSR         (1u << 24)
#define CPUID_FEAT_EDX_SSE          (1u << 25)
#define CPUID_FEAT_EDX_SSE2         (1u << 26)
#define CPUID_FEAT_EDX_SS           (1u << 27)
#define CPUID_FEAT_EDX_HTT          (1u << 28)
#define CPUID_FEAT_EDX_TM           (1u << 29)
#define CPUID_FEAT_EDX_IA64         (1u << 30)
#define CPUID_FEAT_EDX_PBE          (1u << 31)

// ============================================================================
// CPUID.1:ECX — Standard Feature Flags
// ============================================================================

#define CPUID_FEAT_ECX_SSE3          (1u << 0)
#define CPUID_FEAT_ECX_PCLMUL        (1u << 1)
#define CPUID_FEAT_ECX_DTES64        (1u << 2)
#define CPUID_FEAT_ECX_MONITOR       (1u << 3)
#define CPUID_FEAT_ECX_DS_CPL        (1u << 4)
#define CPUID_FEAT_ECX_VMX           (1u << 5)
#define CPUID_FEAT_ECX_SMX           (1u << 6)
#define CPUID_FEAT_ECX_EST           (1u << 7)
#define CPUID_FEAT_ECX_TM2           (1u << 8)
#define CPUID_FEAT_ECX_SSSE3         (1u << 9)
#define CPUID_FEAT_ECX_CID           (1u << 10)
#define CPUID_FEAT_ECX_SDBG          (1u << 11)
#define CPUID_FEAT_ECX_FMA           (1u << 12)
#define CPUID_FEAT_ECX_CX16          (1u << 13)
#define CPUID_FEAT_ECX_XTPR          (1u << 14)
#define CPUID_FEAT_ECX_PDCM          (1u << 15)
#define CPUID_FEAT_ECX_PCID          (1u << 17)
#define CPUID_FEAT_ECX_DCA           (1u << 18)
#define CPUID_FEAT_ECX_SSE41         (1u << 19)
#define CPUID_FEAT_ECX_SSE42         (1u << 20)
#define CPUID_FEAT_ECX_X2APIC        (1u << 21)
#define CPUID_FEAT_ECX_MOVBE         (1u << 22)
#define CPUID_FEAT_ECX_POPCNT        (1u << 23)
#define CPUID_FEAT_ECX_TSC_DEADLINE  (1u << 24)
#define CPUID_FEAT_ECX_AESNI         (1u << 25)
#define CPUID_FEAT_ECX_XSAVE         (1u << 26)
#define CPUID_FEAT_ECX_OSXSAVE       (1u << 27)
#define CPUID_FEAT_ECX_AVX           (1u << 28)
#define CPUID_FEAT_ECX_F16C          (1u << 29)
#define CPUID_FEAT_ECX_RDRAND        (1u << 30)
#define CPUID_FEAT_ECX_HYPERVISOR    (1u << 31)

// ============================================================================
// CPUID.80000001:EDX — Extended Feature Flags
// ============================================================================

#define CPUID_EXT_EDX_SYSCALL   (1u << 11)
#define CPUID_EXT_EDX_NX        (1u << 20)
#define CPUID_EXT_EDX_PAGE1GB   (1u << 26)
#define CPUID_EXT_EDX_RDTSCP    (1u << 27)
#define CPUID_EXT_EDX_LM        (1u << 29)
#define CPUID_EXT_EDX_3DNOW     (1u << 31)

// ============================================================================
// CPUID.7:EBX — Structured Extended Feature Flags
// ============================================================================

#define CPUID_7_EBX_FSGSBASE    (1u << 0)
#define CPUID_7_EBX_TSC_ADJUST  (1u << 1)
#define CPUID_7_EBX_SGX         (1u << 2)
#define CPUID_7_EBX_BMI1        (1u << 3)
#define CPUID_7_EBX_HLE         (1u << 4)
#define CPUID_7_EBX_AVX2        (1u << 5)
#define CPUID_7_EBX_SMEP        (1u << 7)
#define CPUID_7_EBX_BMI2        (1u << 8)
#define CPUID_7_EBX_ERMS        (1u << 9)
#define CPUID_7_EBX_INVPCID     (1u << 10)
#define CPUID_7_EBX_RTM         (1u << 11)
#define CPUID_7_EBX_MPX         (1u << 14)
#define CPUID_7_EBX_AVX512F     (1u << 16)
#define CPUID_7_EBX_SMAP        (1u << 20)
#define CPUID_7_EBX_CLFLUSHOPT  (1u << 23)
#define CPUID_7_EBX_CLWB        (1u << 24)
#define CPUID_7_EBX_SHA         (1u << 29)

// ============================================================================
// CPUID.7:ECX — Structured Extended Feature Flags
// ============================================================================

#define CPUID_7_ECX_PREFETCHWT1 (1u << 0)
#define CPUID_7_ECX_UMIP        (1u << 2)
#define CPUID_7_ECX_PKU         (1u << 3)
#define CPUID_7_ECX_OSPKE       (1u << 4)
#define CPUID_7_ECX_WAITPKU     (1u << 5)
#define CPUID_7_ECX_CET_IBT     (1u << 20)
#define CPUID_7_ECX_SHSTK       (1u << 29)

// ============================================================================
// CPUID.6:EAX — Thermal / Power Management
// ============================================================================

#define CPUID_6_EAX_DTS         (1u << 0)   // Digital Thermal Sensor
#define CPUID_6_EAX_TURBO       (1u << 1)   // Turbo Boost
#define CPUID_6_EAX_ARAT        (1u << 2)   // Always Running APIC Timer
#define CPUID_6_EAX_PLN         (1u << 4)   // Power Limit Notification
#define CPUID_6_EAX_ECMD        (1u << 5)   // Clock Modulation
#define CPUID_6_EAX_PTM         (1u << 6)   // Package Thermal Management
#define CPUID_6_EAX_HWP         (1u << 7)   // Hardware P-States
#define CPUID_6_EAX_HWP_NOT     (1u << 8)
#define CPUID_6_EAX_HWP_ACT     (1u << 9)
#define CPUID_6_EAX_HWP_EPP     (1u << 10)
#define CPUID_6_EAX_HDC         (1u << 13)
#define CPUID_6_EAX_TURBO3      (1u << 14)
#define CPUID_6_EAX_HWP_CAP     (1u << 15)
#define CPUID_6_EAX_HWP_PLR     (1u << 16)

// ============================================================================
// Utilities for parsing fields
// ============================================================================

#define CPUID_BIT(Reg, Bit)   (((Reg) >> (Bit)) & 1u)
#define CPUID_FIELD(Reg, Lo, Hi)  (((Reg) >> (Lo)) & ((1u << ((Hi) - (Lo) + 1)) - 1u))  