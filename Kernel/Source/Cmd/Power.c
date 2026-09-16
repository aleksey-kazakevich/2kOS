#include <Cmd/Power.h>
#include <Drivers/Power.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <Return.h>

INT CmdPowerHandler(INT ArgC, const CHAR *ArgV[]) {
    // Проверяем аргументы
    if (ArgC < 2) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: power -s (shutdown) | -r (reboot)\n");
        BaseconPrintf(BASECON_TYPE_NORMAL, "  -s, --shutdown  Shutdown the system\n");
        BaseconPrintf(BASECON_TYPE_NORMAL, "  -r, --reboot    Reboot the system\n");
        return -1;
    }
    
    // Проверяем первый аргумент
    const CHAR *Option = ArgV[1];
    
    if (StrCmp(Option, "-s") == 0 || StrCmp(Option, "--shutdown") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "Shutting down...\n");
        PowerShutdown();
        return 0;
    }
    
    if (StrCmp(Option, "-r") == 0 || StrCmp(Option, "--reboot") == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "Rebooting...\n");
        PowerReboot();
        return 0;
    }
    
    // Неизвестная опция
    BaseconPrintf(BASECON_TYPE_ERROR, "Unknown option: %s\n", Option);
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: power -s | -r\n");
    return -1;
}

INT CmdPowerRegister(VOID) {
    return CmdRegisterBuiltin("power", "Shutdown or reboot  the system", CmdPowerHandler);
}