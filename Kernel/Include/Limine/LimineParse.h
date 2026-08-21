// Why i was trying to make limine protocol support without limine.h?

#pragma once

#include <Types.h>
#include <Limine/limine.h>

extern struct limine_framebuffer_request      fb_req;
extern struct limine_memmap_request          memmap_req;
extern struct limine_hhdm_request            hhdm_req;
extern struct limine_executable_address_request  kernel_addr_req;
extern struct limine_firmware_type_request firmware_req;
extern struct limine_rsdp_request rsdp_req;
extern struct limine_mp_request smp_req;

// Framebuffer
BOOL LimineFramebufferAvailable(VOID);
UINT64 LimineGetFramebufferCount(VOID);
struct limine_framebuffer *LimineGetFramebuffer(VOID);

// HHDM
UINT64 LimineGetHHDMOffset(VOID);
BOOL LimineHHDMAvailable(VOID);

// Kernel / Executable Address
UINT64 LimineGetKernelPhysicalBase(VOID);
UINT64 LimineGetKernelVirtualBase(VOID);
BOOL LimineKernelAddressAvailable(VOID);

// Memory Map
UINT64 LimineGetMemoryMapEntryCount(VOID);
struct limine_memmap_entry **LimineGetMemoryMapEntries(VOID);
struct limine_memmap_entry *LimineFindUsableMemory(UINT64 Size);
struct limine_memmap_entry *LimineFindMemoryByType(UINT64 Type);
BOOL LimineMemoryMapAvailable(VOID);
const CHAR *LimineMemoryTypeToString(UINT64 Type);
UINT64 LimineGetTotalMemory(VOID);

BOOL IsUefi(VOID);
UINT64 LimineGetRsdp(VOID);
