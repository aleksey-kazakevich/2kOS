#pragma once

#include <Types.h>

/* Available cmdline parameters nobootscreen */

// Get the raw command line string (from Limine or other source)
const CHAR* CmdLineGetRaw(VOID);

// Check if a parameter exists
BOOL CmdLineHasParam(const CHAR *Param);

// Get boolean value: 1/0, true/false, on/off, yes/no
// Returns default value if parameter not found
BOOL CmdLineGetBool(const CHAR *Param, BOOL Default);

// Get integer value
// Returns default value if parameter not found or invalid
INT CmdLineGetInt(const CHAR *Param, INT Default);

// Get string value
// Returns default string if parameter not found
const CHAR* CmdLineGetString(const CHAR *Param, const CHAR *Default);

// Get hex value (e.g., "0x1000" or "1000")
// Returns default value if parameter not found or invalid
UINT64 CmdLineGetHex(const CHAR *Param, UINT64 Default);

// Check if a parameter is present without value (e.g., "quiet")
BOOL CmdLineIsFlagSet(const CHAR *Param);

// Set the command line string
VOID CmdLineSet(const CHAR *CmdLine);

// Parse command line and store parsed values
VOID CmdLineInit(VOID);