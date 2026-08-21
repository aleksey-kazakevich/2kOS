#pragma once

#include <Interrupt.h>
#include <Scheduler.h>
#include <Fatal.h>

extern Task InitTask;

// Checking if the exception came from user mode
static inline BOOL ExceptionFromUser(InterruptFrame *Frame) {
    if (!Frame) return FALSE;
    // CS contains DPL (bits 0-1)
    // Ring 3 = 0x1B (0x18 | 3), Ring 0 = 0x08
    return (Frame->CS & 0x3) == 3;
}

// Trying to kill the current task if it is from a user
// Returns TRUE if the task is killed, FALSE if it is a kernel
static inline BOOL TryKillCurrentTask(InterruptFrame *Frame, const CHAR *Reason) {
    if (!Frame) return FALSE;
    
    // If the exception is from the kernel - we don’t kill it, you need to panic
    if (!ExceptionFromUser(Frame)) {
        return FALSE;
    }
    
    // Checking if there is a current task
    Task *Current = GetCurrentTask();
    if (!Current || Current == &InitTask) {
        // Strange situation - user mode without task
        return FALSE;
    }
    
    SchedulerKill(Current->Pid);
    
    return TRUE;
}

// Macro for handlers: kills task or causes panic
#define HANDLE_EXCEPTION(Frame, Reason) \
    do { \
        if (!TryKillCurrentTask(Frame, Reason)) { \
            FatalF(Reason, Frame); \
        } \
    } while(0)

