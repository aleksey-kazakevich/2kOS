#include <Types.h>
#include <Lib/StdArg.h>
#include <Lib/StdIo.h>
#include <Lib/String.h>

typedef struct {
    CHAR *Str;
    USIZE Size;
    USIZE Written;
} PrintfState;

static inline VOID OutChar(PrintfState *State, CHAR C) {
    if (State->Written < State->Size) {
	State->Str[State->Written] = C;
    }
    State->Written++;
}

static VOID OutString(PrintfState *State, const CHAR *S, USIZE Len) {
    if (Len == (USIZE)-1) {
	while (*S) {
	    OutChar(State, *S++);
	}
    } else {
	for (USIZE I = 0; I < Len; I++) {
	    OutChar(State, S[I]);
	}
    }
}

static VOID OutReverse(CHAR *Start, CHAR *End) {
    while (Start < End) {
	CHAR Tmp = *Start;
	*Start++ = *End;
	*End-- = Tmp;
    }
}

static CHAR *IToABase(UINT64 Num, CHAR *Buf, INT Base, INT Upper) {
    const CHAR *DigitsLower = "0123456789abcdef";
    const CHAR *DigitsUpper = "0123456789ABCDEF";
    const CHAR *Digits = Upper ? DigitsUpper : DigitsLower;
    CHAR *P = Buf;
    if (Num == 0) {
	*P++ = '0';
    } else {
	while (Num > 0) {
	    *P++ = Digits[Num % Base];
	    Num /= Base;
	}
    }
    *P = '\0';
    CHAR *Start = Buf;
    CHAR *End = P - 1;
    while (Start < End) {
	CHAR Tmp = *Start;
	*Start++ = *End;
	*End-- = Tmp;
    }
    return Buf;
}

static VOID OutNum(PrintfState *State, UINT64 Num, INT Base, INT Width, BOOL ZeroPad, BOOL Upper, BOOL Negative) {
    CHAR Buf[64];
    CHAR *NumStr = IToABase(Num, Buf, Base, Upper);
    INT NumLen = (INT)StrLen(NumStr);
    INT SignLen = Negative ? 1 : 0;
    INT TotalLen = NumLen + SignLen;
    INT Pad = (Width > TotalLen) ? Width - TotalLen : 0;

    // 1. First we print a minus sign if the number is negative
    if (Negative) {
        OutChar(State, '-');
    }

    if (!ZeroPad) {
	for (INT I = 0; I < Pad; I++) {
	    OutChar(State, ' ');
	}
	OutString(State, NumStr, NumLen);
    } else {
	for (INT I = 0; I < Pad; I++) {
	    OutChar(State, '0');
	}
	OutString(State, NumStr, NumLen);
    }
}

INT VsnPrintf(CHAR *Str, USIZE Size, const CHAR *Fmt, VA_LIST Args) {
    PrintfState State;
    State.Str = Str;
    State.Size = Size;
    State.Written = 0;
    
    if (Size == 0) {
        return 0;
    }
    
    while (*Fmt && State.Written < Size) {
        if (*Fmt != '%') {
            OutChar(&State, *Fmt++);
            continue;
        }
        
        Fmt++; // Skip '%'
        
        // Parsing flags
        BOOL LeftAlign = FALSE;
        BOOL ZeroPad   = FALSE;
        INT  Width     = 0;
        BOOL IsLong    = FALSE;
        BOOL IsLongLong = FALSE;
        BOOL IsShort   = FALSE;
        BOOL IsChar    = FALSE;
        
        if (*Fmt == '-') { LeftAlign = TRUE; Fmt++; }
        if (*Fmt == '0') { ZeroPad = TRUE; Fmt++; }
        
        while (*Fmt >= '0' && *Fmt <= '9') {
            Width = Width * 10 + (*Fmt - '0');
            Fmt++;
        }

        INT Precision = -1;
        if (*Fmt == '.') {
            Fmt++;
            Precision = 0;
            while (*Fmt >= '0' && *Fmt <= '9') {
                Precision = Precision * 10 + (*Fmt - '0');
                Fmt++;
            }
        }
        
        // Length modifiers
        if (*Fmt == 'h') {
            Fmt++;
            if (*Fmt == 'h') { IsChar = TRUE; Fmt++; }
            else { IsShort = TRUE; }
        } else if (*Fmt == 'l') {
            Fmt++;
            if (*Fmt == 'l') { IsLongLong = TRUE; Fmt++; }
            else { IsLong = TRUE; }
        } else if (*Fmt == 'z') {
            IsLong = TRUE; // size_t on x86_64 is 64 bits
            Fmt++;
        }
        
        // Specifiers
        switch (*Fmt) {
            case 's': {
                CHAR *S = VaArg(Args, CHAR*);
                if (!S) S = "(null)";
                INT Len = (INT)StrLen(S);
                
                if (!LeftAlign) {
                    for (INT i = 0; i < Width - Len && State.Written < Size; i++) 
                        OutChar(&State, ' ');
                }
                OutString(&State, S, (USIZE)Len);
                if (LeftAlign) {
                    for (INT i = 0; i < Width - Len && State.Written < Size; i++) 
                        OutChar(&State, ' ');
                }
                Fmt++;
                break;
            }
            
            case 'c': {
                CHAR C = (CHAR)VaArg(Args, INT);
                if (!LeftAlign) {
                    for (INT i = 0; i < Width - 1 && State.Written < Size; i++) 
                        OutChar(&State, ' ');
                }
                OutChar(&State, C);
                if (LeftAlign) {
                    for (INT i = 0; i < Width - 1 && State.Written < Size; i++) 
                        OutChar(&State, ' ');
                }
                Fmt++;
                break;
            }
            
            case 'd':
            case 'i': {
                INT64 N;
                if (IsLongLong)      N = VaArg(Args, INT64);
                else if (IsLong)     N = VaArg(Args, LONG);
                else if (IsShort)    N = (SHORT)VaArg(Args, INT);
                else if (IsChar)     N = (INT8)VaArg(Args, INT);
                else                 N = VaArg(Args, INT);
                
                BOOL Negative = (N < 0);
                UINT64 UN = Negative ? -(UINT64)N : (UINT64)N;
                
                if (Negative) Width--;
                OutNum(&State, UN, 10, Width, ZeroPad, FALSE, Negative);
                Fmt++;
                break;
            }

            case 'f': {
                DOUBLE N = VaArg(Args, DOUBLE);
                INT IntPart = (INT)N;
                INT FracPart;
                INT Prec = (Precision < 0) ? 2 : Precision;
                DOUBLE Frac = N - IntPart;
                if (Frac < 0) Frac = -Frac;

                DOUBLE Multiplier = 1.0;
                for (INT i = 0; i < Prec; i++) Multiplier *= 10.0;
                FracPart = (INT)(Frac * Multiplier + 0.5);

                if (FracPart >= (INT)Multiplier) {
                    FracPart = 0;
                    IntPart++;
                }

                CHAR IntBuf[64];
                CHAR *IntStr = IToABase(IntPart < 0 ? -IntPart : IntPart, IntBuf, 10, FALSE);

                if (IntPart < 0) OutChar(&State, '-');
                OutString(&State, IntStr, StrLen(IntStr));
                OutChar(&State, '.');

                CHAR FracBuf[64];
                CHAR *FracStr = IToABase(FracPart, FracBuf, 10, FALSE);
                INT FracLen = (INT)StrLen(FracStr);

                for (INT i = 0; i < Prec - FracLen; i++) OutChar(&State, '0');
                OutString(&State, FracStr, StrLen(FracStr));
                Fmt++;
                break;
            }
            
            case 'u':
            case 'x':
            case 'X': {
                UINT64 UN;
                INT Base = (*Fmt == 'u') ? 10 : 16;
                if (IsLongLong)      UN = VaArg(Args, UINT64);
                else if (IsLong)     UN = VaArg(Args, UINTPTR);
                else if (IsShort)    UN = (UINT16)VaArg(Args, UINT32);
                else if (IsChar)     UN = (UINT8)VaArg(Args, UINT32);
                else                 UN = VaArg(Args, UINT32);
                
                OutNum(&State, UN, Base, Width, ZeroPad, (*Fmt == 'X'), FALSE);
                Fmt++;
                break;
            }
            
            case 'p': {
                UINTPTR N = (UINTPTR)VaArg(Args, VOID*);
                OutString(&State, "0x", 2);
                OutNum(&State, (UINT64)N, 16, 16, TRUE, FALSE, FALSE);
                Fmt++;
                break;
            }
            
            case '%': {
                OutChar(&State, '%');
                Fmt++;
                break;
            }
            
            default:
                OutChar(&State, '%');
                if (*Fmt) OutChar(&State, *Fmt++);
                break;
        }
    }
    
    // Null-terminate
    if (State.Written < Size) {
        Str[State.Written] = '\0';
    } else {
        Str[Size - 1] = '\0';
    }
    
    return State.Written;
}

INT SnPrintf(CHAR *Str, USIZE Size, const CHAR *Fmt, ...) {
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT Result = VsnPrintf(Str, Size, Fmt, Args);
    VaEnd(Args);
    return Result;
}

INT SbSPrintf(CHAR *Str, const CHAR *Fmt, ...) {
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT Result = VsnPrintf(Str, 0x7FFFFFFF, Fmt, Args);
    VaEnd(Args);
    return Result;
}