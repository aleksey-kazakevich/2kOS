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
    CmdDiskMgrRegister();
    CmdFormatRegister();
    CmdMountRegister();
    CmdLsRegister();
    CmdRamDiskRegister();
    CmdSysDevRegister();
    CmdRunRegister();
    CmdAsmRegister();
    CmdUnmountRegister();
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

STATIC VOID RegisterUserIso(VOID) {
    if (CmdLineIsFlagSet("nomountuser") || CmdLineIsFlagSet("NOMOUNTUSER")) {
        return;
    }

    /* Try a few likely module strings. */
    struct limine_file *F = NULLPTR;

    STATIC const CHAR *Candidates[] = {
        "USER.iso",
        "user.iso",
        NULLPTR
    };

    for (INT I = 0; Candidates[I]; I++) {
        /* Limine module "string" field is the exact string from limine.conf. */
        F = LimineFindModuleByString(Candidates[I]);
        if (F) break;
        /* Also try matching by path (basename may be embedded). */
        F = LimineFindModuleByPath(Candidates[I]);
        if (F) break;
    }

    if (!F) {
        StdWarnPrintf("useriso: USER.iso module not found, skipping\n");
        return;
    }

    if (!F->address || F->size == 0) {
        StdWarnPrintf("useriso: USER.iso module is empty, skipping\n");
        return;
    }

    INT Rc = IsoRegisterFromMemory(F->address, F->size, "USER");
    if (Rc != SUCCESS) {
        StdErrPrintf("useriso: mount failed (%d)\n", Rc);
        return;
    }
}

VOID KMain(VOID) {
    UINT64 Cr4 = ReadCr4();
    Cr4 |= (1ULL << 9) | (1ULL << 10);
    WriteCr4(Cr4);

    InitStackProtector();
    KDriverInit();
    MemoryInit();
    SysDeviceInit();
    SysDeviceCreateRoot("SYSROOT");
    InitFramebuffer();
    
    Framebuffer *FB = FramebufferGet();
    if (FB)
        BaseconInit(FB);
        BaseconClear();
        BaseconSetEnabled(FALSE);

    KObjInit();
    StdIoInit();
    KObjInitNull();
    KObjInitZero();

    CmdLineInit();
    CmdLineSet(LimineGetCmdLine());

    CheckBootFlags();

    BOOL LoadedInUefi = IsUefi();
    StdOutPrintf("booted via limine:%s\n", LoadedInUefi ? "uefi" : "bios");

    InitIdt();
    
    if (InitAcpiAndApic() != SUCCESS) Fatal("failed to initialize apic and acpi");

    CpuRegisterAllBackends();
    CpuInit();

    InitTimer();

    SmpInit();

    SchedulerInit();

    SyscallInit();

    PCacheInit();

    PowerInit();

    PciInit();

    LocalInterruptsEnable();

    ExceptionLogInit();
	
    BaseconStartConsoleThread();

    DiskMgrInit();
    DiskMgrInitDisks();

    InitFs();

    EnvInit();

    Ps2Init();
    Ps2KeyboardInit();
    Ps2MouseInit();
    MouseCursorInit();

    CmdInit();
    BaseconSetInputCallback(CmdBaseconHandler, NULLPTR);
    InitCmds();

    BaseconSetPrompt("2KOS> ");

    DiskCfgInit();
    EnvLoadRoot();

    RegisterUserIso();

    BaseconSetEnabled(TRUE);
    BaseconSetPromptFixed(TRUE);
    BaseconClear();
    BaseconInputStart();

    for (;;);
}
