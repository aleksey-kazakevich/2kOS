#include <Cmd/SysDev.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SysDevice.h>
#include <Lib/StdIo.h>
#include <Return.h>
#include <Rgb.h>

// ============================================================================
// CONFIG
// ============================================================================

#define SYSDEV_MAX_DEPTH    16

// Colors for different types of devices
STATIC FramebufferColor TypeColor(SysDeviceType Type) {
    switch (Type) {
        case SYSDEV_TYPE_ROOT:        return RGB_WHITE;
        case SYSDEV_TYPE_BUS:         return RGB_ORANGE;
        case SYSDEV_TYPE_CONTROLLER:  return RGB_YELLOW;
        case SYSDEV_TYPE_STORAGE:     return RGB_CORNFLOWER_BLUE;
        case SYSDEV_TYPE_NETWORK:     return RGB_LIGHT_GREEN;
        case SYSDEV_TYPE_DISPLAY:     return RGB_PURPLE;
        case SYSDEV_TYPE_SOUND:       return RGB_PINK;
        case SYSDEV_TYPE_INPUT:       return RGB_CYAN;
        case SYSDEV_TYPE_SERIAL:      return RGB_LIGHT_BLUE;
        case SYSDEV_TYPE_TIMER:       return RGB_GOLD;
        case SYSDEV_TYPE_INTERRUPT:   return RGB_LIGHT_RED;
        case SYSDEV_TYPE_POWER:       return RGB_RED;
        case SYSDEV_TYPE_VIRTUAL:     return RGB_GRAY;
        default:                      return RGB_GRAY;
    }
}

STATIC FramebufferColor StateColor(SysDeviceState State) {
    switch (State) {
        case SYSDEV_STATE_ENABLED:   return RGB_GREEN;
        case SYSDEV_STATE_PRESENT:   return RGB_YELLOW;
        case SYSDEV_STATE_DISABLED:  return RGB_GRAY;
        case SYSDEV_STATE_ERROR:     return RGB_RED;
        case SYSDEV_STATE_REMOVED:   return RGB_DARK_GRAY;
        default:                     return RGB_GRAY;
    }
}

// ============================================================================
// HELP
// ============================================================================

STATIC VOID CmdSysDevHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("Usage: sysdev <command> [args]\n");
    StdOutPrintf("\n");
    StdOutPrintf("Commands:\n");
    StdOutPrintf("  list              List all devices (flat)\n");
    StdOutPrintf("  tree              Show device tree\n");
    StdOutPrintf("  info <name|id>    Show detailed info about a device\n");
    StdOutPrintf("  find <substr>     Search devices by name substring\n");
    StdOutPrintf("  type <type>       List devices of a given type\n");
    StdOutPrintf("  count             Show total device count\n");
    StdOutPrintf("\n");
    StdOutPrintf("Types: root, bus, controller, storage, network,\n");
    StdOutPrintf("       display, sound, input, serial, timer,\n");
    StdOutPrintf("       interrupt, power, virtual\n");
    StdOutPrintf("\n");
}

// ============================================================================
// LIST (flat)
// ============================================================================

STATIC VOID PrintFlatHeader(VOID) {
    StdOutPrintf("  %-4s %-22s %-10s %-10s %s\n",
        "ID", "NAME", "TYPE", "STATE", "DESC");
    StdOutPrintf("  %-4s %-22s %-10s %-10s %s\n",
        "----", "----------------------",
        "----------", "----------", "----");
}

STATIC VOID PrintFlatRow(SysDevice *Dev, VOID *UserData) {
    (VOID)UserData;

    // ID
    /* removed: BaseconSetColors */
    StdOutPrintf("  %-4u ", Dev->Id);

    // Name
    /* removed: BaseconSetColors */
    StdOutPrintf("%-22s ", Dev->Name);

    // Type
    /* removed: BaseconSetColors */
    StdOutPrintf("%-10s ",
                  SysDeviceTypeToString(Dev->Type));

    // State
    /* removed: BaseconSetColors */
    StdOutPrintf("%-10s ",
                  SysDeviceStateToString(Dev->State));

    // Description
    /* removed: BaseconSetColors */
    StdOutPrintf("%s\n", Dev->Description);
}

STATIC INT CmdSysDevList(VOID) {
    StdOutPrintf("\n");
    PrintFlatHeader();
    SysDeviceWalk(PrintFlatRow, NULLPTR);
    StdOutPrintf("\n");
    StdOutPrintf("Total: %u device(s)\n",
                  SysDeviceGetCount());
    StdOutPrintf("\n");
    return 0;
}

// ============================================================================
// TREE
// ============================================================================

STATIC VOID PrintIndent(INT Depth, BOOL IsLast) {
    for (INT I = 0; I < Depth - 1; I++) {
        /* removed: BaseconSetColors */
        StdOutPrintf("  ");
    }
    if (Depth > 0) {
        /* removed: BaseconSetColors */
        StdOutPrintf(IsLast ? "`- " : "|- ");
    }
}

STATIC VOID DumpTree(SysDevice *Dev, INT Depth, BOOL IsLast) {
    if (!Dev || Depth > SYSDEV_MAX_DEPTH) return;

    PrintIndent(Depth, IsLast);

    // Name (colored by type)
    /* removed: BaseconSetColors */
    StdOutPrintf("%s", Dev->Name);

    // Type + state
    /* removed: BaseconSetColors */
    StdOutPrintf(" [%s, ",
                  SysDeviceTypeToString(Dev->Type));

    /* removed: BaseconSetColors */
    StdOutPrintf("%s",
                  SysDeviceStateToString(Dev->State));

    /* removed: BaseconSetColors */
    StdOutPrintf("]");

    // Driver binding
    if (Dev->Driver) {
        /* removed: BaseconSetColors */
        StdOutPrintf(" <%s>", Dev->Driver->Name);
    }

    /* removed: BaseconSetColors */
    StdOutPrintf("\n");

    // Children
    ListHead *Pos;
    ListHead *Head = &Dev->Children;
    ListForEach(Pos, Head) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        BOOL ChildIsLast = (Pos->Next == Head);
        DumpTree(Child, Depth + 1, ChildIsLast);
    }
}

STATIC VOID WalkTopLevelForTree(SysDevice *Dev, VOID *UserData) {
    (VOID)UserData;
    if (Dev->Parent) return;   // top-level only
    DumpTree(Dev, 0, TRUE);
}

STATIC INT CmdSysDevTree(VOID) {
    StdOutPrintf("\n");
    SysDeviceWalk(WalkTopLevelForTree, NULLPTR);
    StdOutPrintf("\n");
    StdOutPrintf("Total: %u device(s)\n",
                  SysDeviceGetCount());
    StdOutPrintf("\n");
    return 0;
}

// ============================================================================
// INFO
// ============================================================================

STATIC VOID PrintResource(SysDeviceResource *R) {
    /* removed: BaseconSetColors */
    StdOutPrintf("    %-6s ",
                  SysDeviceResTypeToString(R->Type));

    /* removed: BaseconSetColors */
    StdOutPrintf("0x%08llX - 0x%08llX  (%llu)\n",
                  R->Base,
                  R->Base + R->Size - 1,
                  R->Size);
}

STATIC VOID PrintInfoRecursive(SysDevice *Dev, INT Depth) {
    if (!Dev) return;

    for (INT I = 0; I < Depth; I++) {
        StdOutPrintf("  ");
    }

    // Name
    /* removed: BaseconSetColors */
    StdOutPrintf("%s", Dev->Name);

    /* removed: BaseconSetColors */
    StdOutPrintf(" [%s, ",
                  SysDeviceTypeToString(Dev->Type));

    /* removed: BaseconSetColors */
    StdOutPrintf("%s",
                  SysDeviceStateToString(Dev->State));

    /* removed: BaseconSetColors */
    StdOutPrintf("] id=%u\n", Dev->Id);

    ListHead *Pos;
    ListForEach(Pos, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        PrintInfoRecursive(Child, Depth + 1);
    }
}

STATIC VOID PrintDeviceInfo(SysDevice *Dev) {
    StdOutPrintf("\n");

    // Header
    /* removed: BaseconSetColors */
    StdOutPrintf("=== %s ===\n", Dev->Name);

    /* removed: BaseconSetColors */
    StdOutPrintf("  id:          %u\n", Dev->Id);
    StdOutPrintf("  description: %s\n", Dev->Description);

    /* removed: BaseconSetColors */
    StdOutPrintf("  type:        ");

    /* removed: BaseconSetColors */
    StdOutPrintf("%s\n",
                  SysDeviceTypeToString(Dev->Type));

    /* removed: BaseconSetColors */
    StdOutPrintf("  state:       ");

    /* removed: BaseconSetColors */
    StdOutPrintf("%s\n",
                  SysDeviceStateToString(Dev->State));

    // Parent
    /* removed: BaseconSetColors */
    if (Dev->Parent) {
        StdOutPrintf("  parent:      %s\n",
                      Dev->Parent->Name);
    } else {
        StdOutPrintf("  parent:      (none)\n");
    }

    // Driver
    if (Dev->Driver) {
        StdOutPrintf("  driver:      ");
        /* removed: BaseconSetColors */
        StdOutPrintf("%s\n", Dev->Driver->Name);
    } else {
        /* removed: BaseconSetColors */
        StdOutPrintf("  driver:      (none)\n");
    }

    // Children
    /* removed: BaseconSetColors */
    StdOutPrintf("  children:    %u\n", Dev->ChildCount);

    // Resources
    /* removed: BaseconSetColors */
    StdOutPrintf("  resources:   %u\n", Dev->ResourceCount);
    for (UINT32 I = 0; I < Dev->ResourceCount; I++) {
        PrintResource(&Dev->Resources[I]);
    }

    // Properties
    /* removed: BaseconSetColors */
    StdOutPrintf("  properties:  %u\n", Dev->PropCount);
    for (UINT32 I = 0; I < Dev->PropCount; I++) {
        SysDeviceProp *P = &Dev->Properties[I];
        /* removed: BaseconSetColors */
        StdOutPrintf("    %-16s = ",
                      P->Name);
        /* removed: BaseconSetColors */
        StdOutPrintf("%s\n", P->Value);
    }

    // Children (tree)
    if (Dev->ChildCount > 0) {
        /* removed: BaseconSetColors */
        StdOutPrintf("  subtree:\n");
        ListHead *Pos;
        ListForEach(Pos, &Dev->Children) {
            SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
            PrintInfoRecursive(Child, 2);
        }
    }

    StdOutPrintf("\n");
}

STATIC INT CmdSysDevInfo(const CHAR *Arg) {
    if (!Arg) {
        StdErrPrintf("usage: sysdev info <name|id>\n");
        return -1;
    }

    SysDevice *Dev = NULLPTR;

    // We try it as an ID (if it starts with a number)
    if (Arg[0] >= '0' && Arg[0] <= '9') {
        Dev = SysDeviceFindById((UINT32)AToI(Arg));
    }
    if (!Dev) {
        Dev = SysDeviceFind(Arg);
    }
    if (!Dev) {
        StdErrPrintf("device not found: %s\n", Arg);
        return -1;
    }

    PrintDeviceInfo(Dev);
    return 0;
}

// ============================================================================
// FIND
// ============================================================================

STATIC BOOL GFindFound;

STATIC VOID FindVisitor(SysDevice *Dev, VOID *UserData) {
    const CHAR *Substr = (const CHAR *)UserData;
    if (!Substr) return;

    // We are looking for a case-insensitive substring (simplified — direct occurrence)
    if (StrStr(Dev->Name, Substr)) {
        GFindFound = TRUE;
        PrintFlatRow(Dev, NULLPTR);
    }
}

STATIC INT CmdSysDevFind(const CHAR *Substr) {
    if (!Substr) {
        StdErrPrintf("usage: sysdev find <substring>\n");
        return -1;
    }

    StdOutPrintf("\n");
    PrintFlatHeader();

    GFindFound = FALSE;
    SysDeviceWalk(FindVisitor, (VOID *)Substr);

    if (!GFindFound) {
        StdOutPrintf("  (no matches)\n");
    }
    StdOutPrintf("\n");
    return 0;
}

// ============================================================================
// TYPE
// ============================================================================

STATIC SysDeviceType ParseType(const CHAR *Str) {
    if (StrCaseCmp(Str, "root") == 0)       return SYSDEV_TYPE_ROOT;
    if (StrCaseCmp(Str, "bus") == 0)        return SYSDEV_TYPE_BUS;
    if (StrCaseCmp(Str, "controller") == 0) return SYSDEV_TYPE_CONTROLLER;
    if (StrCaseCmp(Str, "storage") == 0)    return SYSDEV_TYPE_STORAGE;
    if (StrCaseCmp(Str, "network") == 0)    return SYSDEV_TYPE_NETWORK;
    if (StrCaseCmp(Str, "display") == 0)    return SYSDEV_TYPE_DISPLAY;
    if (StrCaseCmp(Str, "sound") == 0)      return SYSDEV_TYPE_SOUND;
    if (StrCaseCmp(Str, "input") == 0)      return SYSDEV_TYPE_INPUT;
    if (StrCaseCmp(Str, "serial") == 0)     return SYSDEV_TYPE_SERIAL;
    if (StrCaseCmp(Str, "timer") == 0)      return SYSDEV_TYPE_TIMER;
    if (StrCaseCmp(Str, "interrupt") == 0)  return SYSDEV_TYPE_INTERRUPT;
    if (StrCaseCmp(Str, "power") == 0)      return SYSDEV_TYPE_POWER;
    if (StrCaseCmp(Str, "virtual") == 0)    return SYSDEV_TYPE_VIRTUAL;
    return (SysDeviceType)0xFF;
}

STATIC SysDeviceType GFilterType;
STATIC UINT32 GFilterCount;

STATIC VOID FilterByType(SysDevice *Dev, VOID *UserData) {
    (VOID)UserData;
    if (Dev->Type == GFilterType) {
        GFilterCount++;
        PrintFlatRow(Dev, NULLPTR);
    }
}

STATIC INT CmdSysDevType(const CHAR *TypeStr) {
    if (!TypeStr) {
        StdErrPrintf("usage: sysdev type <type>\n");
        return -1;
    }

    SysDeviceType Type = ParseType(TypeStr);
    if ((UINT8)Type == 0xFF) {
        StdErrPrintf("unknown type: %s\n", TypeStr);
        return -1;
    }

    StdOutPrintf("\n");
    PrintFlatHeader();

    GFilterType = Type;
    GFilterCount = 0;
    SysDeviceWalk(FilterByType, NULLPTR);

    StdOutPrintf("\n");
    StdOutPrintf("Total: %u %s device(s)\n",
                  GFilterCount, SysDeviceTypeToString(Type));
    StdOutPrintf("\n");
    return 0;
}

// ============================================================================
// COUNT
// ============================================================================

STATIC INT CmdSysDevCount(VOID) {
    StdOutPrintf("Total devices: %u\n", SysDeviceGetCount());

    if (SysDeviceGetRoot()) {
        StdOutPrintf("Root children: %u\n",
                      SysDeviceGetRoot()->ChildCount);
    }

    return 0;
}

// ============================================================================
// HANDLER
// ============================================================================

INT CmdSysDevHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        CmdSysDevHelp();
        return 0;
    }

    const CHAR *Cmd = ArgV[1];

    if (StrCmp(Cmd, "list") == 0 || StrCmp(Cmd, "ls") == 0) {
        return CmdSysDevList();
    }
    if (StrCmp(Cmd, "tree") == 0) {
        return CmdSysDevTree();
    }
    if (StrCmp(Cmd, "info") == 0) {
        return CmdSysDevInfo(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    if (StrCmp(Cmd, "find") == 0) {
        return CmdSysDevFind(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    if (StrCmp(Cmd, "type") == 0) {
        return CmdSysDevType(ArgC >= 3 ? ArgV[2] : NULLPTR);
    }
    if (StrCmp(Cmd, "count") == 0) {
        return CmdSysDevCount();
    }

    StdErrPrintf("unknown subcommand: %s\n", Cmd);
    StdOutPrintf("Try 'sysdev help'\n");
    return -1;
}

INT CmdSysDevRegister(VOID) {
    return CmdRegisterBuiltin("sysdev",
                              "Device manager",
                              CmdSysDevHandler);
}