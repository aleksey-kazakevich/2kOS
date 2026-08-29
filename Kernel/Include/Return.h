#pragma once

#include <Types.h>

#define SUCCESS 0
#define INCORRECT_VALUE -1
#define NO_OBJECT -2
#define NOT_FOUND -3
#define IO_ERROR -4
#define NO_MEMORY -5
#define BUSY -6
#define NOT_IMPLEMENTED -7
#define NOT_SUPPORTED -8
#define CHECK_ERROR -9
#define UNKNOWN -10
#define ALREADY_EXISTS -11
#define GENERAL_ERROR -12
#define TIMEOUT -13
#define DEVICE_ERROR -14
#define DEVICE_INVALID -15
#define NOT_READY - 16
#define PERMISSION_DENIED -17
#define OPERATION_DISABLED -18
#define CANCELED -19

// Do not use INCORRECT_ERROR_CODE in RETURN macro
#define INCORRECT_ERROR_CODE -999

extern INT LastCode;
static INT LeastCode = CANCELED;

typedef struct {
    BOOL IsError;
    INT Code;
} ErrorState;

#define RETURN(Code) do { \
        if (Code < LeastCode) return INCORRECT_ERROR_CODE; \
        LastCode = Code; \
        return Code; \
    } while (0)

static inline INT GetLastCode(VOID) {
    return LastCode;
}

static inline ErrorState IsError(INT Value) {
    if (Value < LeastCode) {
	ErrorState TrueState;
	TrueState.IsError = TRUE;
	TrueState.Code = INCORRECT_VALUE;
	return TrueState;
    }
    if (Value < 0) {
	ErrorState TrueState;
	TrueState.IsError = TRUE;
        TrueState.Code = Value;
	return TrueState;
    }
    ErrorState FalseState;
    FalseState.IsError = FALSE;
    FalseState.Code = 0;
    return FalseState;
}

static inline const CHAR* ReturnCode2String(INT Code) {
    if (Code > 0) return "";
    if (Code < LeastCode) return "";
    switch (Code) {
        case SUCCESS: return "SUCCESS";
        case INCORRECT_VALUE: return "INCORRECT_VALUE";
        case NO_OBJECT: return "NO_OBJECT";
        case NOT_FOUND: return "NOT_FOUND";
        case IO_ERROR: return "IO_ERROR";
        case NO_MEMORY: return "NO_MEMORY";
        case BUSY: return "BUSY";
        case NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case NOT_SUPPORTED: return "NOT_SUPPORTED";
        case CHECK_ERROR: return "CHECK_ERROR";
        case UNKNOWN: return "UNKNOWN";
        case ALREADY_EXISTS: return "ALREADY_EXISTS";
        case GENERAL_ERROR: return "GENERAL_ERROR";
        case TIMEOUT: return "TIMEOUT";
        case DEVICE_ERROR: return "DEVICE_ERROR";
        case PERMISSION_DENIED: return "PERMISSION_DENIED";
        case DEVICE_INVALID: return "DEVICE_INVALID";
    }
    return "";
}
