#include <MainIncs.h>

INT LastCode = -0; // ;)
EXTERN(UINTPTR, __stack_chk_guard);

VOID MemoryInit(VOID) {
    LowMemInit();
    PhysAllocInit(PhysAllocGet());
    UINT64 LowPhys = LowMemPhysBase();
    if (LowPhys) {
        PhysAllocReserveRange(PhysAllocGet(), LowPhys, LOWMEM_SIZE);
    }
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

    STATIC struct StackGuardTLS KernelTLS;

    KernelTLS.Canary = 0xDEADC0DEBAADF00DULL;
    WriteMSR(IA32_GS_BASE, (UINT64)&KernelTLS);
    WriteMSR(IA32_KERNEL_GS_BASE, (UINT64)&KernelTLS);
}

VOID InitCmds(VOID) {
    CmdEchoRegister();
    CmdPowerRegister();
    CmdDiskMgrRegister();
    CmdFormatRegister();
    CmdMountRegister();
    CmdUnmountRegister();
    CmdLsRegister();
    CmdMkdirRegister();
    CmdRmdirRegister();
    CmdTouchRegister();
    CmdRmRegister();
    CmdRamDiskRegister();
    CmdSysDevRegister();
    CmdRunRegister();
}

VOID InitFs(VOID) {
    VfsInit();
    ExfatInit();
}

VOID CheckBootFlags(VOID) {
    if (CmdLineIsFlagSet("quiet") || CmdLineIsFlagSet("QUIET")) {
        FramebufferDrawString("System in boot process. Please wait.", 0, 0, RGB_WHITE, RGB_BLACK);
    } else {
        BaseconSetEnabled(TRUE);
    }
}

VOID KMain(VOID) {
    InitStackProtector();
    TssInit();
    KDriverInit();
    MemoryInit();
    SysDeviceInit();
    SysDeviceCreateRoot("SYSROOT");
    InitFramebuffer();
    
    Framebuffer *FB = FramebufferGet();
    if (FB)
        BaseconInit(FB);
	BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconClear();
        BaseconSetEnabled(FALSE);

    CmdLineInit();
    CmdLineSet(LimineGetCmdLine());

    CheckBootFlags();

    BOOL LoadedInUefi = IsUefi();
    BaseconPrintf(BASECON_TYPE_INFO, "booted via limine:%s\n", LoadedInUefi ? "uefi" : "bios");

    InitIdt();
    
    if (InitAcpiAndApic() != SUCCESS) Fatal("failed to initialize apic and acpi");

    ComInitAll();

    PciInit();
    
    InitTimer();

    SchedulerInit();

    SyscallInit();

    PCacheInit();

    LocalInterruptsEnable();

    DiskMgrInit();
    DiskMgrInitDisks();

    InitFs();

    Ps2Init();
    Ps2KeyboardInit();
    Ps2MouseInit();
    MouseCursorInit();

    CmdInit();
    BaseconSetInputCallback(CmdBaseconHandler, NULLPTR);
    InitCmds();

    BaseconSetEnabled(TRUE);
    BaseconClear();

    BaseconSetPrompt("2KOS> ");
    BaseconSetPromptFixed(TRUE);
    BaseconInputStart();

    for (;;);
}
