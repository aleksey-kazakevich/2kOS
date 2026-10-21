#include <Utils/SyscallLog.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <Cmd.h>

STATIC VOID CmdSLogHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: slog <command>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  list      Show all entries\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  clear     Clear log\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  count     Show entry count\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  on/off    Enable/disable logging\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

INT CmdSLogHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) { SyscallLogDump(); return 0; }

    const CHAR *Sub = ArgV[1];
    if (StrCmp(Sub, "list") == 0 || StrCmp(Sub, "ls") == 0) {
        SyscallLogDump(); return 0;
    }
    if (StrCmp(Sub, "clear") == 0) {
        SyscallLogClear();
        BaseconPrintf(BASECON_TYPE_SUCCESS, "slog: cleared\n");
        return 0;
    }
    if (StrCmp(Sub, "count") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "slog: %u entries\n", SyscallLogCount());
        return 0;
    }
    if (StrCmp(Sub, "on") == 0) {
        SyscallLogSetEnabled(TRUE);
        BaseconPrintf(BASECON_TYPE_SUCCESS, "slog: enabled\n");
        return 0;
    }
    if (StrCmp(Sub, "off") == 0) {
        SyscallLogSetEnabled(FALSE);
        BaseconPrintf(BASECON_TYPE_SUCCESS, "slog: disabled\n");
        return 0;
    }
    if (StrCmp(Sub, "help") == 0) { CmdSLogHelp(); return 0; }

    BaseconPrintf(BASECON_TYPE_ERROR, "slog: unknown: %s\n", Sub);
    CmdSLogHelp();
    return -1;
}

INT CmdSLogRegister(VOID) {
    return CmdRegisterBuiltin("slog", "Syscall log", CmdSLogHandler);
}