#include <Limine/LimineParse.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <CmdLine.h>
#include <Basecon.h>
#include <Return.h>

// Static State
static CHAR GCmdLineBuffer[4096] = {0};  // Raw command line
static BOOL GInitialized = FALSE;

static BOOL IsWhitespace(CHAR C) {
    return C == ' ' || C == '\t' || C == '\n' || C == '\r';
}

static VOID SkipWhitespace(const CHAR **Str) {
    while (**Str && IsWhitespace(**Str)) {
        (*Str)++;
    }
}

static const CHAR* FindParamValue(const CHAR *Param, const CHAR *CmdLine) {
    if (!Param || !CmdLine) return NULLPTR;
    
    USIZE ParamLen = StrLen(Param);
    if (ParamLen == 0) return NULLPTR;
    
    const CHAR *Pos = CmdLine;
    
    while (*Pos) {
        // Skip whitespace
        SkipWhitespace(&Pos);
        if (!*Pos) break;
        
        // Check if this parameter matches
        if (StrnCmp(Pos, Param, ParamLen) == 0) {
            const CHAR *Next = Pos + ParamLen;
            
            // Must be followed by '=' or whitespace or end of string
            if (*Next == '=') {
                // Value after '='
                return Next + 1;
            } else if (IsWhitespace(*Next) || *Next == '\0') {
                // Flag without value (e.g., "quiet")
                return Pos;  // Special marker: points to param itself
            }
        }
        
        // Move to next parameter
        while (*Pos && !IsWhitespace(*Pos)) {
            Pos++;
        }
    }
    
    return NULLPTR;
}

static const CHAR* FindParamValueWithType(const CHAR *Param, const CHAR **EndPtr) {
    const CHAR *CmdLine = GCmdLineBuffer;
    if (!CmdLine || !*CmdLine) return NULLPTR;
    
    const CHAR *Value = FindParamValue(Param, CmdLine);
    if (!Value) return NULLPTR;
    
    // If Value == Param, it's a flag (no '=' sign)
    if (Value == Param) {
        if (EndPtr) *EndPtr = Value + StrLen(Param);
        return Value;
    }
    
    // Skip whitespace after '='
    while (*Value && IsWhitespace(*Value)) {
        Value++;
    }
    
    if (EndPtr) {
        const CHAR *End = Value;
        while (*End && !IsWhitespace(*End)) {
            End++;
        }
        *EndPtr = End;
    }
    
    return Value;
}

const CHAR* CmdLineGetRaw(VOID) {
    return GCmdLineBuffer;
}

BOOL CmdLineHasParam(const CHAR *Param) {
    if (!GInitialized) return FALSE;
    return FindParamValue(Param, GCmdLineBuffer) != NULLPTR;
}

BOOL CmdLineIsFlagSet(const CHAR *Param) {
    if (!GInitialized) return FALSE;
    
    const CHAR *CmdLine = GCmdLineBuffer;
    if (!CmdLine || !*CmdLine) return FALSE;
    
    const CHAR *Pos = CmdLine;
    USIZE ParamLen = StrLen(Param);
    
    while (*Pos) {
        SkipWhitespace(&Pos);
        if (!*Pos) break;
        
        if (StrnCmp(Pos, Param, ParamLen) == 0) {
            const CHAR *Next = Pos + ParamLen;
            // Must be followed by whitespace or end of string (no '=' sign)
            if (*Next == '\0' || IsWhitespace(*Next)) {
                return TRUE;
            }
        }
        
        while (*Pos && !IsWhitespace(*Pos)) {
            Pos++;
        }
    }
    
    return FALSE;
}

BOOL CmdLineGetBool(const CHAR *Param, BOOL Default) {
    if (!GInitialized) return Default;
    
    const CHAR *Value = FindParamValue(Param, GCmdLineBuffer);
    if (!Value) return Default;
    
    // Check if it's a flag without value
    if (Value == Param) {
        return TRUE;  // "debug" without value = true
    }
    
    // Value exists
    CHAR FirstChar = *Value;
    
    // Numeric: 1 = true, 0 = false
    if (FirstChar >= '0' && FirstChar <= '9') {
        return AToI(Value) != 0;
    }
    
    // String values
    if (StrCaseCmp(Value, "true") == 0 || 
        StrCaseCmp(Value, "yes") == 0 || 
        StrCaseCmp(Value, "on") == 0 ||
        StrCaseCmp(Value, "1") == 0) {
        return TRUE;
    }
    
    if (StrCaseCmp(Value, "false") == 0 || 
        StrCaseCmp(Value, "no") == 0 || 
        StrCaseCmp(Value, "off") == 0 ||
        StrCaseCmp(Value, "0") == 0) {
        return FALSE;
    }
    
    // Any non-empty value = true
    return TRUE;
}

INT CmdLineGetInt(const CHAR *Param, INT Default) {
    if (!GInitialized) return Default;
    
    const CHAR *End = NULLPTR;
    const CHAR *Value = FindParamValueWithType(Param, &End);
    if (!Value || Value == Param) return Default;
    if (!End || End == Value) return Default;
    
    // Copy value to temporary buffer
    CHAR Temp[64];
    USIZE Len = End - Value;
    if (Len >= sizeof(Temp)) Len = sizeof(Temp) - 1;
    StrnCpy(Temp, Value, Len);
    Temp[Len] = '\0';
    
    return AToI(Temp);
}

const CHAR* CmdLineGetString(const CHAR *Param, const CHAR *Default) {
    if (!GInitialized) return Default;
    
    const CHAR *End = NULLPTR;
    const CHAR *Value = FindParamValueWithType(Param, &End);
    if (!Value || Value == Param) return Default;
    if (!End || End == Value) return Default;
    
    // Return pointer to the value in the command line
    // (we can't modify it, but we can return it directly)
    return Value;
}

UINT64 CmdLineGetHex(const CHAR *Param, UINT64 Default) {
    if (!GInitialized) return Default;
    
    const CHAR *End = NULLPTR;
    const CHAR *Value = FindParamValueWithType(Param, &End);
    if (!Value || Value == Param) return Default;
    if (!End || End == Value) return Default;
    
    // Copy value to temporary buffer
    CHAR Temp[64];
    USIZE Len = End - Value;
    if (Len >= sizeof(Temp)) Len = sizeof(Temp) - 1;
    StrnCpy(Temp, Value, Len);
    Temp[Len] = '\0';
    
    UINT64 Result = 0;
    const CHAR *P = Temp;
    
    // Skip "0x" prefix
    if (P[0] == '0' && (P[1] == 'x' || P[1] == 'X')) {
        P += 2;
    }
    
    while (*P) {
        CHAR C = *P++;
        UINT64 Digit;
        if (C >= '0' && C <= '9') {
            Digit = C - '0';
        } else if (C >= 'a' && C <= 'f') {
            Digit = C - 'a' + 10;
        } else if (C >= 'A' && C <= 'F') {
            Digit = C - 'A' + 10;
        } else {
            break;
        }
        Result = (Result << 4) | Digit;
    }
    
    return Result;
}


VOID CmdLineSet(const CHAR *CmdLine) {
    if (!CmdLine) {
        GCmdLineBuffer[0] = '\0';
        return;
    }
    
    StrnCpy(GCmdLineBuffer, CmdLine, sizeof(GCmdLineBuffer) - 1);
    GCmdLineBuffer[sizeof(GCmdLineBuffer) - 1] = '\0';
}

VOID CmdLineInit(VOID) {
    if (GInitialized) return;
    
    const CHAR *CmdLine = "";
    
    CmdLineSet(CmdLine);
    GInitialized = TRUE;
}