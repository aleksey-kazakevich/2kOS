#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Return.h>
#include <Cmd.h>

STATIC ListHead GCmdList;
STATIC UINT32 GCmdCount = 0;
STATIC BOOL GInitialized = FALSE;

STATIC VOID CmdStripNewline(CHAR *Str) {
    if (!Str) return;
    USIZE Len = StrLen(Str);
    while (Len > 0 && (Str[Len-1] == '\n' || Str[Len-1] == '\r')) {
        Str[--Len] = '\0';
    }
}

STATIC INT CmdParseArguments(const CHAR *CmdLine, const CHAR **ArgV, INT MaxArgs) {
    if (!CmdLine || !ArgV || MaxArgs < 1) return -1;
    
    INT ArgC = 0;
    const CHAR *Pos = CmdLine;
    BOOL InArg = FALSE;
    const CHAR *ArgStart = NULLPTR;
    
    while (*Pos && ArgC < MaxArgs) {
        if (*Pos == ' ' || *Pos == '\t') {
            if (InArg) {
                // Completing the argument
                USIZE Len = Pos - ArgStart;
                CHAR *Arg = (CHAR*)MemoryAllocate(Len + 1);
                if (!Arg) return -1;
                MemCpy(Arg, ArgStart, Len);
                Arg[Len] = '\0';
                ArgV[ArgC++] = Arg;
                InArg = FALSE;
            }
        } else {
            if (!InArg) {
                ArgStart = Pos;
                InArg = TRUE;
            }
        }
        Pos++;
    }
    
    // The last argument
    if (InArg && ArgC < MaxArgs) {
        USIZE Len = Pos - ArgStart;
        CHAR *Arg = (CHAR*)MemoryAllocate(Len + 1);
        if (!Arg) return -1;
        MemCpy(Arg, ArgStart, Len);
        Arg[Len] = '\0';
        ArgV[ArgC++] = Arg;
    }
    
    return ArgC;
}

STATIC VOID CmdFreeArguments(INT ArgC, const CHAR **ArgV) {
    for (INT I = 0; I < ArgC; I++) {
        MemoryFree((VOID*)ArgV[I]);
    }
}

STATIC INT CmdRegisterInternal(const CHAR *Name, const CHAR *Description, CmdHandler Handler) {
    if (!Name || !Handler) RETURN(NO_OBJECT);
    if (CmdFind(Name)) RETURN(ALREADY_EXISTS);
    
    CmdCommand *Cmd = (CmdCommand*)MemoryAllocate(sizeof(CmdCommand));
    if (!Cmd) RETURN(NO_MEMORY);
    
    Cmd->Name = Name;
    Cmd->Description = Description;
    Cmd->Handler = Handler;
    Cmd->Builtin = TRUE;
    
    ListAddTail(&GCmdList, &Cmd->Node);
    GCmdCount++;
    
    return SUCCESS;
}

VOID CmdInit(VOID) {
    if (GInitialized) return;
    
    ListInit(&GCmdList);
    GCmdCount = 0;
    GInitialized = TRUE;
}

INT CmdRegisterBuiltin(const CHAR *Name, const CHAR *Description, CmdHandler Handler) {
    if (!GInitialized) return NO_OBJECT;
    return CmdRegisterInternal(Name, Description, Handler);
}

INT CmdUnregisterBuiltin(const CHAR *Name) {
    if (!GInitialized || !Name) RETURN(NO_OBJECT);
    
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GCmdList) {
        CmdCommand *Cmd = ListEntry(Pos, CmdCommand, Node);
        if (StrCmp(Cmd->Name, Name) == 0) {
            ListDel(&Cmd->Node);
            MemoryFree(Cmd);
            GCmdCount--;
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

CmdCommand* CmdFind(const CHAR *Name) {
    if (!GInitialized || !Name) return NULLPTR;
    
    ListHead *Pos;
    ListForEach(Pos, &GCmdList) {
        CmdCommand *Cmd = ListEntry(Pos, CmdCommand, Node);
        if (StrCmp(Cmd->Name, Name) == 0) {
            return Cmd;
        }
    }
    
    return NULLPTR;
}

INT CmdExecute(const CHAR *CmdLine) {
    if (!GInitialized || !CmdLine) RETURN(NO_OBJECT);
    if (!CmdLine[0]) RETURN(SUCCESS);
    
    // Copying the string for parsing
    CHAR Buffer[256];
    StrnCpy(Buffer, CmdLine, sizeof(Buffer) - 1);
    Buffer[sizeof(Buffer) - 1] = '\0';
    CmdStripNewline(Buffer);
    
    // Parsimony arguments
    const CHAR *ArgV[16];
    INT ArgC = CmdParseArguments(Buffer, ArgV, 16);
    if (ArgC <= 0) RETURN(INCORRECT_VALUE);
    
    // Looking for a command
    CmdCommand *Cmd = CmdFind(ArgV[0]);
    if (!Cmd) {
        BaseconPrintf(BASECON_TYPE_ERROR, "command not found: %s\n", ArgV[0]);
        CmdFreeArguments(ArgC, ArgV);
        RETURN(NOT_FOUND);
    }
    
    // Executing
    INT Result = Cmd->Handler(ArgC, ArgV);
    
    // Freeing up memory
    CmdFreeArguments(ArgC, ArgV);
    
    return Result;
}

CmdCommand* CmdGetFirst(VOID) {
    if (!GInitialized || ListEmpty(&GCmdList)) return NULLPTR;
    return ListEntry(GCmdList.Next, CmdCommand, Node);
}

CmdCommand* CmdGetNext(CmdCommand *Cmd) {
    if (!Cmd || Cmd->Node.Next == &GCmdList) return NULLPTR;
    return ListEntry(Cmd->Node.Next, CmdCommand, Node);
}

UINT32 CmdGetCount(VOID) {
    return GCmdCount;
}

VOID CmdBaseconHandler(const CHAR *Input, VOID *UserData) {
    (VOID)UserData;
    
    if (!Input || !Input[0]) return;
    
    CmdExecute(Input);
}