#pragma once

#include <Interrupt.h>
#include <Types.h>

// Types of exceptions
typedef enum {
    EXC_UNKNOWN = 0,
    EXC_DIVIDE_ERROR,          // #DE  — 0
    EXC_DEBUG,                 // #DB  — 1
    EXC_NMI,                   //      — 2
    EXC_BREAKPOINT,            // #BP  — 3
    EXC_OVERFLOW,              // #OF  — 4
    EXC_BOUND_RANGE,           // #BR  — 5
    EXC_INVALID_OPCODE,        // #UD  — 6
    EXC_DEVICE_NOT_AVAILABLE,  // #NM  — 7
    EXC_DOUBLE_FAULT,          // #DF  — 8
    EXC_INVALID_TSS,           // #TS  — 10
    EXC_SEGMENT_NOT_PRESENT,   // #NP  — 11
    EXC_STACK_SEGMENT_FAULT,   // #SS  — 12
    EXC_GENERAL_PROTECTION,    // #GP  — 13
    EXC_PAGE_FAULT,            // #PF  — 14
    EXC_FPU_ERROR,             // #MF  — 16
    EXC_ALIGNMENT_CHECK,       // #AC  — 17
    EXC_MACHINE_CHECK,         // #MC  — 18
    EXC_SIMD_ERROR,            // #XM  — 19
    EXC_VIRTUALIZATION,        // #VE  — 20
    EXC_CONTROL_PROTECTION,    // #CP  — 21
    EXC_HYPERVISOR_INJECTION,  // #HV  — 28
    EXC_VMM_COMMUNICATION,     // #VC  — 29
    EXC_SECURITY,              // #SX  — 30
    EXC_SYSCALL,               //      — no exception, for the log
} ExceptionType;

// One log entry
typedef struct {
    UINT64 Tick;               // moment (TimerTicks)
    INT    Pid;                // issue PID
    CHAR   Name[32];           // task name
    ExceptionType Type;        // type of exception
    UINT64 Rip;                // the address of the instruction
    UINT64 FaultAddr;          // CR2 for #PF, otherwise 0
    UINT64 ErrorCode;          // error code (for exceptions with a code)
    UINT64 Rsp;                // stack pointer at the time of the exception
    UINT64 Cs;                 // CS (to define the ring)
    UINT64 Rflags;
    BOOL   Valid;
} ExceptionLogEntry;

#define EXCEPTION_LOG_SIZE 64

// Initializing the subsystem
VOID ExceptionLogInit(VOID);

// Write the exception to the log.
// Called from exception handlers.
// If fromUser == FALSE (exception from ring0) — writes marked "kernel".
// Returns the index of the record.
INT ExceptionLogPush(ExceptionType Type, InterruptFrame *Frame);

// Clear the log
VOID ExceptionLogClear(VOID);

// Number of entries
UINT32 ExceptionLogCount(VOID);

// Get a record by index (0 is the oldest)
const ExceptionLogEntry* ExceptionLogGet(UINT32 Index);

// Output the log to Basecon
VOID ExceptionLogDump(VOID);

// Output the last record
VOID ExceptionLogDumpLast(VOID);

// String name of the exception type
const CHAR* ExceptionTypeToString(ExceptionType Type);

/*
 * Copy up to MaxEntries entries into a user buffer at UserBuf.
 * Each entry is written as the raw ExceptionLogEntry struct.
 * Returns the number of bytes written (>= 0) or -errno.
 *
 * Requires the current task to have a valid user PageTable.
 */
INT ExceptionLogCopyToUser(ExceptionLogEntry *UserBuf, UINT32 MaxEntries);