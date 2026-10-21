#include <Cmd/SysDev.h>
#include <Lib/String.h>
#include <SysDevice.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
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
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Usage: sysdev <command> [args]\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Commands:\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  list              List all devices (flat)\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  tree              Show device tree\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  info <name|id>    Show detailed info about a device\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  find <substr>     Search devices by name substring\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  type <type>       List devices of a given type\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "  count             Show total device count\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Types: root, bus, controller, storage, network,\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "       display, sound, input, serial, timer,\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "       interrupt, power, virtual\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

// ============================================================================
// LIST (flat)
// ============================================================================

STATIC VOID PrintFlatHeader(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "  %-4s %-22s %-10s %-10s %s\n",
        "ID", "NAME", "TYPE", "STATE", "DESC");
    BaseconPrintf(BASECON_TYPE_NORMAL,
        "  %-4s %-22s %-10s %-10s %s\n",
        "----", "----------------------",
        "----------", "----------", "----");
}

STATIC VOID PrintFlatRow(SysDevice *Dev, VOID *UserData) {
    (VOID)UserData;

    // ID
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  %-4u ", Dev->Id);

    // Name
    BaseconSetColors(RGB_WHITE, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%-22s ", Dev->Name);

    // Type
    BaseconSetColors(TypeColor(Dev->Type), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%-10s ",
                  SysDeviceTypeToString(Dev->Type));

    // State
    BaseconSetColors(StateColor(Dev->State), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%-10s ",
                  SysDeviceStateToString(Dev->State));

    // Description
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s\n", Dev->Description);
}

STATIC INT CmdSysDevList(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    PrintFlatHeader();
    SysDeviceWalk(PrintFlatRow, NULLPTR);
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %u device(s)\n",
                  SysDeviceGetCount());
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

// ============================================================================
// TREE
// ============================================================================

STATIC VOID PrintIndent(INT Depth, BOOL IsLast) {
    for (INT I = 0; I < Depth - 1; I++) {
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "  ");
    }
    if (Depth > 0) {
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, IsLast ? "`- " : "|- ");
    }
}

STATIC VOID DumpTree(SysDevice *Dev, INT Depth, BOOL IsLast) {
    if (!Dev || Depth > SYSDEV_MAX_DEPTH) return;

    PrintIndent(Depth, IsLast);

    // Name (colored by type)
    BaseconSetColors(TypeColor(Dev->Type), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s", Dev->Name);

    // Type + state
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, " [%s, ",
                  SysDeviceTypeToString(Dev->Type));

    BaseconSetColors(StateColor(Dev->State), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s",
                  SysDeviceStateToString(Dev->State));

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "]");

    // Driver binding
    if (Dev->Driver) {
        BaseconSetColors(RGB_LIGHT_BLUE, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, " <%s>", Dev->Driver->Name);
    }

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");

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
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    SysDeviceWalk(WalkTopLevelForTree, NULLPTR);
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %u device(s)\n",
                  SysDeviceGetCount());
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

// ============================================================================
// INFO
// ============================================================================

STATIC VOID PrintResource(SysDeviceResource *R) {
    BaseconSetColors(RGB_YELLOW, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "    %-6s ",
                  SysDeviceResTypeToString(R->Type));

    BaseconSetColors(RGB_WHITE, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "0x%08llX - 0x%08llX  (%llu)\n",
                  R->Base,
                  R->Base + R->Size - 1,
                  R->Size);
}

STATIC VOID PrintInfoRecursive(SysDevice *Dev, INT Depth) {
    if (!Dev) return;

    for (INT I = 0; I < Depth; I++) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  ");
    }

    // Name
    BaseconSetColors(TypeColor(Dev->Type), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s", Dev->Name);

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, " [%s, ",
                  SysDeviceTypeToString(Dev->Type));

    BaseconSetColors(StateColor(Dev->State), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s",
                  SysDeviceStateToString(Dev->State));

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "] id=%u\n", Dev->Id);

    ListHead *Pos;
    ListForEach(Pos, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        PrintInfoRecursive(Child, Depth + 1);
    }
}

STATIC VOID PrintDeviceInfo(SysDevice *Dev) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");

    // Header
    BaseconSetColors(TypeColor(Dev->Type), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "=== %s ===\n", Dev->Name);

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  id:          %u\n", Dev->Id);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  description: %s\n", Dev->Description);

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  type:        ");

    BaseconSetColors(TypeColor(Dev->Type), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s\n",
                  SysDeviceTypeToString(Dev->Type));

    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  state:       ");

    BaseconSetColors(StateColor(Dev->State), RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "%s\n",
                  SysDeviceStateToString(Dev->State));

    // Parent
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    if (Dev->Parent) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  parent:      %s\n",
                      Dev->Parent->Name);
    } else {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  parent:      (none)\n");
    }

    // Driver
    if (Dev->Driver) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  driver:      ");
        BaseconSetColors(RGB_LIGHT_BLUE, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%s\n", Dev->Driver->Name);
    } else {
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "  driver:      (none)\n");
    }

    // Children
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  children:    %u\n", Dev->ChildCount);

    // Resources
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  resources:   %u\n", Dev->ResourceCount);
    for (UINT32 I = 0; I < Dev->ResourceCount; I++) {
        PrintResource(&Dev->Resources[I]);
    }

    // Properties
    BaseconSetColors(RGB_GRAY, RGB_BLACK);
    BaseconPrintf(BASECON_TYPE_NORMAL, "  properties:  %u\n", Dev->PropCount);
    for (UINT32 I = 0; I < Dev->PropCount; I++) {
        SysDeviceProp *P = &Dev->Properties[I];
        BaseconSetColors(RGB_CYAN, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "    %-16s = ",
                      P->Name);
        BaseconSetColors(RGB_WHITE, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "%s\n", P->Value);
    }

    // Children (tree)
    if (Dev->ChildCount > 0) {
        BaseconSetColors(RGB_GRAY, RGB_BLACK);
        BaseconPrintf(BASECON_TYPE_NORMAL, "  subtree:\n");
        ListHead *Pos;
        ListForEach(Pos, &Dev->Children) {
            SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
            PrintInfoRecursive(Child, 2);
        }
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

STATIC INT CmdSysDevInfo(const CHAR *Arg) {
    if (!Arg) {
        BaseconPrintf(BASECON_TYPE_ERROR, "usage: sysdev info <name|id>\n");
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
        BaseconPrintf(BASECON_TYPE_ERROR, "device not found: %s\n", Arg);
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
        BaseconPrintf(BASECON_TYPE_ERROR, "usage: sysdev find <substring>\n");
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    PrintFlatHeader();

    GFindFound = FALSE;
    SysDeviceWalk(FindVisitor, (VOID *)Substr);

    if (!GFindFound) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  (no matches)\n");
    }
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
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
        BaseconPrintf(BASECON_TYPE_ERROR, "usage: sysdev type <type>\n");
        return -1;
    }

    SysDeviceType Type = ParseType(TypeStr);
    if ((UINT8)Type == 0xFF) {
        BaseconPrintf(BASECON_TYPE_ERROR, "unknown type: %s\n", TypeStr);
        return -1;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    PrintFlatHeader();

    GFilterType = Type;
    GFilterCount = 0;
    SysDeviceWalk(FilterByType, NULLPTR);

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "Total: %u %s device(s)\n",
                  GFilterCount, SysDeviceTypeToString(Type));
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    return 0;
}

// ============================================================================
// COUNT
// ============================================================================

STATIC INT CmdSysDevCount(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "Total devices: %u\n", SysDeviceGetCount());

    if (SysDeviceGetRoot()) {
        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "Root children: %u\n",
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

    BaseconPrintf(BASECON_TYPE_ERROR, "unknown subcommand: %s\n", Cmd);
    BaseconPrintf(BASECON_TYPE_NORMAL, "Try 'sysdev help'\n");
    return -1;
}

INT CmdSysDevRegister(VOID) {
    return CmdRegisterBuiltin("sysdev",
                              "Device manager",
                              CmdSysDevHandler);
}