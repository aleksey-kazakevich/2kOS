#include <Limine/limine.h>
#include <Types.h>

__attribute__((section(".limine_reqs"), used))
struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = NULLPTR 
};

__attribute__((section(".limine_reqs"), used))
struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 1, // For memmap revision 1 is better
    .response = NULLPTR
};

__attribute__((section(".limine_reqs"), used))
struct limine_hhdm_request hhdm_req = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = NULLPTR
};

__attribute__((section(".limine_reqs"), used))
struct limine_executable_address_request kernel_addr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = NULLPTR
};

__attribute__((section(".limine_reqs"), used))
struct limine_firmware_type_request firmware_req = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID,
    .revision = 0,
    .response = NULLPTR
};

__attribute__((section(".limine_reqs"), used))
struct limine_rsdp_request rsdp_req = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
    .response = NULLPTR
};
