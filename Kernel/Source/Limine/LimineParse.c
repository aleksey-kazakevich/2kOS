#include <Limine/LimineParse.h>
#include <Types.h>

BOOL LimineIsAvailable(VOID *Response) {
    return Response != NULLPTR;
}

/* === Framebuffer === */

BOOL LimineFramebufferAvailable(VOID) {
    // Reading response directly from the correct request structure
    return LimineIsAvailable(fb_req.response);
}

UINT64 LimineGetFramebufferCount(VOID) {
    if (!LimineFramebufferAvailable()) {
        return 0;
    }
    struct limine_framebuffer_response *Resp = 
        (struct limine_framebuffer_response *)fb_req.response;
    
    return Resp->framebuffer_count;
}

struct limine_framebuffer *LimineGetFramebuffer(VOID) {
    if (!LimineFramebufferAvailable()) {
        return NULLPTR;
    }
    struct limine_framebuffer_response *Resp = 
        (struct limine_framebuffer_response *)fb_req.response;
    
    if (Resp->framebuffer_count == 0) {
        return NULLPTR;
    }
    return Resp->framebuffers[0];
}

/* === HHDM === */

BOOL LimineHHDMAvailable(VOID) {
    return LimineIsAvailable(hhdm_req.response);
}

UINT64 LimineGetHHDMOffset(VOID) {
    if (!LimineHHDMAvailable()) {
        return 0;
    }
    struct limine_hhdm_response *Resp = (struct limine_hhdm_response *)hhdm_req.response;
    return Resp->offset;
}

/* === Executable Address (formerly Kernel Address) ===*/

BOOL LimineKernelAddressAvailable(VOID) {
    return LimineIsAvailable(kernel_addr_req.response);
}

UINT64 LimineGetKernelPhysicalBase(VOID) {
    if (!LimineKernelAddressAvailable()) {
        return 0;
    }
    // Resulting in the correct response type for executable_address
    struct limine_executable_address_response *Resp = 
        (struct limine_executable_address_response *)kernel_addr_req.response;
        
    return Resp->physical_base;
}

UINT64 LimineGetKernelVirtualBase(VOID) {
    if (!LimineKernelAddressAvailable()) {
        return 0;
    }
    struct limine_executable_address_response *Resp = 
        (struct limine_executable_address_response *)kernel_addr_req.response;
        
    return Resp->virtual_base;
}

/* === Memory Map === */

BOOL LimineMemoryMapAvailable(VOID) {
    return LimineIsAvailable(memmap_req.response);
}

UINT64 LimineGetMemoryMapEntryCount(VOID) {
    if (!LimineMemoryMapAvailable()) {
        return 0;
    }
    struct limine_memmap_response *Resp = (struct limine_memmap_response *)memmap_req.response;
    return Resp->entry_count;
}

struct limine_memmap_entry **LimineGetMemoryMapEntries(VOID) {
    if (!LimineMemoryMapAvailable()) {
        return NULLPTR;
    }
    struct limine_memmap_response *Resp = (struct limine_memmap_response *)memmap_req.response;
    return Resp->entries;
}

struct limine_memmap_entry *LimineFindMemoryByType(UINT64 Type) {
    if (!LimineMemoryMapAvailable()) {
        return NULLPTR;
    }
    
    struct limine_memmap_response *Resp = (struct limine_memmap_response *)memmap_req.response;
    struct limine_memmap_entry **Entries = Resp->entries;
    
    for (UINT64 I = 0; I < Resp->entry_count; I++) {
        if (Entries[I]->type == Type) {
            return Entries[I];
        }
    }
    return NULLPTR;
}

struct limine_memmap_entry *LimineFindUsableMemory(UINT64 Size) {
    if (!LimineMemoryMapAvailable()) {
        return NULLPTR;
    }
    
    struct limine_memmap_response *Resp = (struct limine_memmap_response *)memmap_req.response;
    struct limine_memmap_entry **Entries = Resp->entries;
    
    for (UINT64 I = 0; I < Resp->entry_count; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        // Checking the USABLE type and sufficient size
        if (Entry->type == LIMINE_MEMMAP_USABLE && Entry->length >= Size) {
            return Entry;
        }
    }
    return NULLPTR;
}

const CHAR *LimineMemoryTypeToString(UINT64 Type) {
    switch (Type) {
        case LIMINE_MEMMAP_USABLE: return "Usable";
        case LIMINE_MEMMAP_RESERVED: return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS: return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY: return "Bad Memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader Reclaimable";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "Executable & Modules";
        case LIMINE_MEMMAP_FRAMEBUFFER: return "Framebuffer";
        case LIMINE_MEMMAP_RESERVED_MAPPED: return "Reserved Mapped";
        default: return "Unknown";
    }
}

UINT64 LimineGetTotalMemory(VOID) {
    if (!LimineMemoryMapAvailable()) {
        return 0;
    }
    
    struct limine_memmap_response *Resp = (struct limine_memmap_response *)memmap_req.response;
    struct limine_memmap_entry **Entries = Resp->entries;
    UINT64 Total = 0;
    
    for (UINT64 I = 0; I < Resp->entry_count; I++) {
        if (Entries[I]->type == LIMINE_MEMMAP_USABLE) {
            Total += Entries[I]->length;
        }
    }
    return Total;
}

UINT64 LimineGetFirmwareType(VOID) {
    if (!firmware_req.response) {
        return 0xFFFFFFFFFFFFFFFFULL;
    }
    
    struct limine_firmware_type_response *Resp = 
        (struct limine_firmware_type_response *)firmware_req.response;
    
    return Resp->firmware_type;
}

BOOL IsUefi(VOID) {
    UINT64 Type = LimineGetFirmwareType();
    return (Type == LIMINE_FIRMWARE_TYPE_EFI32 || 
            Type == LIMINE_FIRMWARE_TYPE_EFI64);
}

UINT64 LimineGetRsdp(VOID) {
    if (!rsdp_req.response) {
        return 0;
    }
    struct limine_rsdp_response *Resp = 
        (struct limine_rsdp_response *)rsdp_req.response;
    return (UINT64)(UINTPTR)Resp->address;
}
