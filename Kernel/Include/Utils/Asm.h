#pragma once

#include <Return.h>
#include <Types.h>

#define ASM_MAX_SECTIONS        8
#define ASM_MAX_SYMBOLS         256
#define ASM_MAX_INSTRUCTIONS    4096
#define ASM_MAX_LINE_LEN        512
#define ASM_MAX_NAME_LEN        64

typedef enum {
    ASM_OK = 0,
    ASM_ERR_READ,
    ASM_ERR_PARSE,
    ASM_ERR_ENCODE,
    ASM_ERR_ELF,
    ASM_ERR_WRITE,
    ASM_ERR_MEMORY,
    ASM_ERR_NO_ENTRY,
} AsmErrorCode;

typedef struct {
    AsmErrorCode Code;
    CHAR Message[256];
    UINT32 Line;
} AsmResult;

// Compiles it .the asm file is in ELF64 and writes by OutputPath.
// If OutputPath == NULLPTR or "" is only a check, the file is not written.
AsmResult AsmCompileFile(const CHAR *InputPath, const CHAR *OutputPath);

// Compiles from a buffer in memory.
AsmResult AsmCompileBuffer(const CHAR *Source, USIZE SourceLen,
                           const CHAR *OutputPath);