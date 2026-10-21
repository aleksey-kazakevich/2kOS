#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Asm/Cpu.h>
#include <Basecon.h> 
#include <PerCpu.h>
#include <Cmd.h>
#include <Rgb.h>

// ============================================================================
// HELP
// ============================================================================

STATIC VOID CmdSmpInfoHelp(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: smpinfo\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  Show SMP CPU information\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

// ============================================================================
// MAIN COMMAND
// ============================================================================

INT CmdSmpInfoHandler(INT ArgC, const CHAR *ArgV[]) {
    (VOID)ArgC;
    (VOID)ArgV;

    UINT32 Total  = PerCpuGetCount();
    UINT32 Online = PerCpuGetOnlineCount();

    if (Total == 0) {
        BaseconPrintf(BASECON_TYPE_WARNING, "smp: no cpus registered\n");
        return 0;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== SMP CPU Information ===\n");
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Total CPUs: %u, Online: %u\n\n",
                  Total, Online);

    // Table header
    BaseconSetColors(RGB_CYAN, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "%-4s %-8s %-8s %-6s %-6s %-10s\n",
                  "IDX", "LAPIC", "ACPIUID", "BSP", "STATE", "ROLE");
    BaseconSetColors(RGB_GRAY, RGB_BLACK);

    for (UINT32 I = 0; I < Total; I++) {
        PerCpu *P = &GPerCpu[I];

        CHAR LapicBuf[16];
        CHAR UidBuf[16];
        SnPrintf(LapicBuf, sizeof(LapicBuf), "%u", P->LapicId);
        SnPrintf(UidBuf, sizeof(UidBuf), "%u", P->AcpiUid);

        // The name of the color chart for online/offline
        FramebufferColor StateColor = P->Online ? RGB_GREEN : RGB_DARK_GRAY;
        const CHAR *StateStr = P->Online ? "online" : "offline";

        // Color for BSP
        const CHAR *BspStr = P->IsBsp ? "yes" : "no";
        FramebufferColor BspColor = P->IsBsp ? RGB_YELLOW : RGB_GRAY;

        // IDX
        BaseconSetColors(RGB_WHITE, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%-4u ", I);

        // LAPIC
        BaseconSetColors(RGB_LIGHT_BLUE, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%-8s ", LapicBuf);

        // ACPI UID
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%-8s ", UidBuf);

        // BSP
        BaseconSetColors(BspColor, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%-6s ", BspStr);

        // STATE
        BaseconSetColors(StateColor, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%-6s ", StateStr);

        // ROLE
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        if (P->IsBsp) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "boot");
        } else if (P->Online) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "idle");
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "-");
        }

        BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    }

    // Information about the current CPU
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    PerCpu *Self = GetPerCpu();
    if (Self) {
        BaseconSetColors(RGB_CYAN, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "Current CPU: index=%u lapic=%u bsp=%s\n",
                      Self->CpuIndex,
                      Self->LapicId,
                      Self->IsBsp ? "yes" : "no");
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

// ============================================================================
// REGISTER
// ============================================================================

INT CmdSmpInfoRegister(VOID) {
    return CmdRegisterBuiltin("smpinfo",
                              "Show SMP CPU information",
                              CmdSmpInfoHandler);
}