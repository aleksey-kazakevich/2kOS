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
    UINT64 Seed = ReadTimeStampCounter();
    Seed ^= ReadCr3();
    Seed ^= (Seed << 13);
    Seed ^= (Seed >> 7);
    Seed ^= (Seed << 17);
    if (Seed == 0) Seed = 0xDEADC0DEBAADF00DULL;

    __stack_chk_guard = Seed;
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
    CmdCdRegister();
    CmdWriteRegister();
    CmdReadRegister();
    CmdAsmRegister();
    CmdELogRegister();
    CmdSLogRegister();
    CmdHexDumpRegister();
    CmdSmpInfoRegister();
}

VOID InitFs(VOID) {
    VfsInit();
    ExfatInit();
    Fat32Init();
    Iso9660Init();
}

VOID CheckBootFlags(VOID) {
    if (CmdLineIsFlagSet("quiet") || CmdLineIsFlagSet("QUIET")) {
        FramebufferDrawString("System in boot process. Please wait.", 0, 0, RGB_WHITE, RGB_BLACK);
    } else {
        BaseconSetEnabled(TRUE);
    }
    if (!CmdLineIsFlagSet("nocom") || !CmdLineIsFlagSet("NOCOM")) {
        ComInitAll();
    }
}

VOID KMain(VOID) {
    InitStackProtector();
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

    PciInit();

    CpuRegisterAllBackends();
    CpuInit();

    SchedulerInit();

    SmpInit();

    InitTimer();

    SyscallInit();

    PCacheInit();

    LocalInterruptsEnable();

    ExceptionLogInit();
	
    BaseconStartConsoleThread();

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

    BaseconSetPrompt("2KOS> ");

    DiskCfgInit();

    BaseconSetEnabled(TRUE);
    BaseconClear();

    BaseconSetPromptFixed(TRUE);
    BaseconInputStart();

    for (;;);
}
