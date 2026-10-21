#include <Utils/ExceptionLog.h>
#include <Lib/String.h>
#include <Cmd/ELog.h>
#include <Basecon.h>
#include <Return.h>

STATIC VOID CmdELogHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: elog <command>\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  list      Show all entries\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  last      Show last entry\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  clear     Clear log\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  count     Show entry count\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

INT CmdELogHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        ExceptionLogDump();
        return 0;
    }

    const CHAR *Sub = ArgV[1];

    if (StrCmp(Sub, "list") == 0 || StrCmp(Sub, "ls") == 0) {
        ExceptionLogDump();
        return 0;
    }
    if (StrCmp(Sub, "last") == 0) {
        ExceptionLogDumpLast();
        return 0;
    }
    if (StrCmp(Sub, "clear") == 0) {
        ExceptionLogClear();
        BaseconPrintf(BASECON_TYPE_SUCCESS, "elog: cleared\n");
        return 0;
    }
    if (StrCmp(Sub, "count") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "elog: %u entries\n",
                      ExceptionLogCount());
        return 0;
    }
    if (StrCmp(Sub, "help") == 0) {
        CmdELogHelp();
        return 0;
    }

    BaseconPrintf(BASECON_TYPE_ERROR, "elog: unknown subcommand: %s\n", Sub);
    CmdELogHelp();
    return -1;
}

INT CmdELogRegister(VOID) {
    return CmdRegisterBuiltin("elog", "Exception log", CmdELogHandler);
}