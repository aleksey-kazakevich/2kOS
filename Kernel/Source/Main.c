#include <MainIncs.h>

INT LastCode = -0; // ;)
EXTERN(UINTPTR, __stack_chk_guard);

VOID MemoryInit(VOID) {
    PhysAllocInit(PhysAllocGet());
    PagingInit();
    MemoryAllocatorInit();
}

VOID InitStackProtector(VOID) {
    #define IA32_GS_BASE        0xC0000101
    #define IA32_KERNEL_GS_BASE 0xC0000102

    struct StackGuardTLS {
        UINT8 Reserved[40];
        UINT64 Canary;
    };

    static struct StackGuardTLS KernelTLS;

    KernelTLS.Canary = 0xDEADC0DEBAADF00DULL;
    WriteMSR(IA32_GS_BASE, (UINT64)&KernelTLS);
    WriteMSR(IA32_KERNEL_GS_BASE, (UINT64)&KernelTLS);
}

VOID KMain(VOID) {
    InitStackProtector();
    TssInit();
    KDriverInit();
    MemoryInit();
    InitFramebuffer();
    
    Framebuffer *FB = FramebufferGet();
    if (FB)
        BaseconInit(FB);
	BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconClear();
        BaseconPrintf(BASECON_TYPE_SUCCESS, "basecon initialized\n");
    
    BOOL LoadedInUefi = IsUefi();
    BaseconPrintf(BASECON_TYPE_INFO, "booted via limine:%s\n", LoadedInUefi ? "uefi" : "bios");

    InitIdt();
    
    if (InitAcpiAndApic() != SUCCESS) Fatal("failed to initialize apic and acpi");

    PciInit();
    
    InitTimer();

    SchedulerInit();

    SyscallInit();

    LocalInterruptsEnable();

    DiskMgrInit();
    DiskMgrInitDisks();

    Ps2Init();
    Ps2KeyboardInit();
    Ps2MouseInit();
    MouseCursorInit();

    BaseconSetPrompt("$> ");
    BaseconInputStart();

    for (;;);
}
