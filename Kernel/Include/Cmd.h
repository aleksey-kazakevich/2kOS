#pragma once

#include <Types.h>
#include <List.h>

typedef INT (*CmdHandler)(INT ArgC, const CHAR *ArgV[]);

// The structure of the built‑in command
typedef struct CmdCommand {
    ListHead Node;              // For a linked list
    const CHAR *Name;           // Command name (e.g., "help")
    const CHAR *Description;    // Brief description (for help)
    CmdHandler Handler;         // Handler function
    BOOL Builtin;              // TRUE = built‑in, FALSE = external (for the future)
} CmdCommand;

// Initialization of the command system (call at startup)
VOID CmdInit(VOID);

// Register the built‑in command
INT CmdRegisterBuiltin(const CHAR *Name, const CHAR *Description, CmdHandler Handler);

// Delete the built‑in command
INT CmdUnregisterBuiltin(const CHAR *Name);

// Find the team by name
CmdCommand* CmdFind(const CHAR *Name);

// Execute the command line by line
// Returns: the command completion code or -1 if the command is not found
INT CmdExecute(const CHAR *CmdLine);

// Get a list of all commands (for help)
CmdCommand* CmdGetFirst(VOID);
CmdCommand* CmdGetNext(CmdCommand *Cmd);

// The number of registered teams
UINT32 CmdGetCount(VOID);

// Print the list of commands in Basecon
VOID CmdPrintList(VOID);

// Input handler from Basecon
VOID CmdBaseconHandler(const CHAR *Input, VOID *UserData);