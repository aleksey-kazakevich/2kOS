#include <Mem/Allocator.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <Scheduler.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Basecon.h>
#include <Cmd/Run.h>
#include <Return.h>
#include <Cmd.h>
#include <Elf.h>
#include <Rgb.h>
#include <Env.h>

// ============================================================================
// HELP
// ============================================================================

STATIC VOID CmdRunHelp(VOID) {
    StdOutPrintf("\n");
}

// ============================================================================
// WAIT FOR PROGRAM
// ============================================================================

// We are waiting for the task with the given PID to be completed.
// Returns the completion code or -1 in case of an error.
STATIC INT CmdRunWait(INT Pid) {
    if (Pid < 0) return -1;

    // If the task is already dead, don't wait.
    if (!TaskIsAlive(Pid)) {
        return 0;
    }

    Basecon *Con = BaseconGet();

    // Switching Basecon to standby mode
    if (Con) {
        Con->WaitingForProgram = TRUE;
        Con->WaitingPid = Pid;
    }

    // We are waiting for completion.
    // We use TimerSleep in order not to spin the CPU at 100%.
    // We check it every 10 ms.
    while (TaskIsAlive(Pid)) {
        TimerSleep(10);
    }

    // We take the zombies and free up the resources.
    ReapZombies();

    // Returning Basecon to normal mode.
    if (Con) {
        Con->WaitingForProgram = FALSE;
        Con->WaitingPid = -1;
    }

    return 0;
}

// ============================================================================
// HANDLER
// ============================================================================

INT CmdRunHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        CmdRunHelp();
        return -1;
    }

    CHAR ResolvedPath[PATH_MAX];
    const CHAR *Path = ArgV[1];

    /*
     * If the name has no path separator and no mount prefix,
     * resolve it through PATH (from /etc/env.conf).
     * Explicit paths ("SATA1::/bin/x", "/bin/x", "./x") are used as-is.
     */
    BOOL LooksLikePath = (StrChr(Path, '/') != NULLPTR) ||
                         (StrStr(Path, "::") != NULLPTR);
    if (!LooksLikePath) {
        if (EnvResolveExec(Path, ResolvedPath, sizeof(ResolvedPath)) == SUCCESS) {
            Path = ResolvedPath;
        }
    }

    VfsFile *File = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, Path, VFS_O_READ, &File);
    if (Rc != SUCCESS) {
        StdErrPrintf("cannot open '%s'\n", Path);
        return -1;
    }

    Elf64Ehdr Ehdr;
    UINT32 Rd = 0;
    VfsSeek(File, 0, SEEK_SET);
    Rc = VfsRead(File, &Ehdr, sizeof(Ehdr), &Rd);
    if (Rc != SUCCESS || Rd != sizeof(Ehdr)) {
        StdErrPrintf("cannot read ELF header\n");
        VfsClose(File);
        return -1;
    }

    if (*(const UINT32 *)Ehdr.EIdent != ELF_MAGIC ||
        Ehdr.EIdent[4] != ELFCLASS64 ||
        Ehdr.EIdent[5] != ELFDATA2LSB ||
        Ehdr.EMachine != EM_X86_64) {
        StdErrPrintf("'%s' is not a valid ELF64\n", Path);
        VfsClose(File);
        return -1;
    }

    UINT64 *Pml4 = NULLPTR;
    UINT64 Entry = 0;
    UINT64 Sp = 0;
    UINT64 HeapStart = 0;

    INT ProgArgC = ArgC - 1;
    const CHAR **ProgArgV = &ArgV[1];

    Rc = ElfLoadFromFile(File, &Pml4, &Entry, &Sp, &HeapStart, ProgArgC, ProgArgV);
    VfsClose(File);

    if (Rc != SUCCESS) {
        StdErrPrintf("run: ELF load failed (%d): %s\n",
                      Rc, ReturnCode2String(Rc));
        return -1;
    }

    INT Pid = UserTaskCreateFromPageTable(Pml4, Entry, Sp, HeapStart, Path);
    if (Pid < 0) {
        StdErrPrintf("run: cannot create task (%d)\n", Pid);
        PagingDestroyUserAddressSpace(Pml4);
        return -1;
    }

    CmdRunWait(Pid);
    return 0;
}

// ============================================================================
// REGISTER
// ============================================================================

INT CmdRunRegister(VOID) {
    return CmdRegisterBuiltin("run", "Run ELF executable", CmdRunHandler);
}