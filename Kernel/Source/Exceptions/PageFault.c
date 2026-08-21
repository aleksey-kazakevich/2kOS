#include <Types.h>
#include <Asm/Cpu.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Mem/Paging.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Lib/String.h>

static inline UINT64 GetPageFaultAddress(VOID) {
    UINT64 Addr;
    asm volatile("mov %%cr2, %0" : "=r"(Addr));
    return Addr;
}

#define PF_PRESENT   (1 << 0)  // 0 = page not found, 1 = security violation
#define PF_WRITE     (1 << 1)  // 0 = read, 1 = write
#define PF_USER      (1 << 2)  // 0 = supervisor, 1 = user
#define PF_RESERVED  (1 << 3)  // Reserved bit set
#define PF_INSTR     (1 << 4)  // Instruction

static BOOL IsUserStackAddress(UINT64 Addr) {
    // User stack
    UINT64 StackBottom = USER_STACK_TOP - USER_STACK_DEFAULT - (16 * PAGE_SIZE);
    return (Addr >= StackBottom && Addr < USER_STACK_TOP);
}

static BOOL IsUserCodeAddress(UINT64 Addr) {
    // User code: starting with USER_CODE_VADDR
    return (Addr >= USER_CODE_VADDR && Addr < USER_CODE_VADDR + (16 * PAGE_SIZE));
}

static INT HandleUserPageFault(Task *Current, UINT64 FaultAddr, UINT64 ErrorCode) {
    if (!Current || !Current->PageTable) {
        return -1;
    }
    
    // Verify that the address is in user space
    if (FaultAddr >= 0x00007FFFFFFFFFFFULL) {
        return -1;
    }
    
    // === CASE 1: User Stack ===
    if (IsUserStackAddress(FaultAddr)) {
        // Align the address to the page
        UINT64 PageAddr = FaultAddr & ~(PAGE_SIZE - 1);
        
        // Selecting a physical page
        VOID *PhysPage = PhysAllocAllocatePage(PhysAllocGet());
        if (!PhysPage) {
            return -1;
        }
        
        // Mapping the page
        UINT64 Flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NO_EXEC;
        INT Result = PagingMapPage(Current->PageTable, PageAddr, (UINT64)PhysPage, Flags);
        
        if (Result != 0) {
            PhysAllocFreePage(PhysAllocGet(), PhysPage);
            return -1;
        }
        
        return 0;  // Successfully recovered
    }
    
    // === CASE 2: Anonymous page (heap, bss, etc) ===
    if (!(ErrorCode & PF_PRESENT)) {
        UINT64 PageAddr = FaultAddr & ~(PAGE_SIZE - 1);
        
        // Checking that the address is not busy
        UINT64 ExistingPhys = PagingLookupVirt(Current->PageTable, PageAddr);
        if (ExistingPhys) {
            return -1;
        }
        
        // Selecting a physical page
        VOID *PhysPage = PhysAllocAllocatePage(PhysAllocGet());
        if (!PhysPage) {
            return -1;
        }
        
        // Resetting the page
        MemSet((VOID*)PhysToVirt((UINT64)PhysPage), 0, PAGE_SIZE);
        
        // Defining flags
        UINT64 Flags = PTE_PRESENT | PTE_USER;
        if (ErrorCode & PF_WRITE) {
            Flags |= PTE_WRITABLE;
        }
        
        // Map
        INT Result = PagingMapPage(Current->PageTable, PageAddr, (UINT64)PhysPage, Flags);
        
        if (Result != 0) {
            PhysAllocFreePage(PhysAllocGet(), PhysPage);
            return -1;
        }
        
        return 0;  // Successfully recovered
    }
    
    if ((ErrorCode & PF_PRESENT) && (ErrorCode & PF_WRITE)) {
        return -1;
    }
    
    // === CASE 4: Security violation (read/write without rights) ===
    if (ErrorCode & PF_PRESENT) {
        return -1;
    }

    return -1;
}

static INT HandleKernelPageFault(UINT64 FaultAddr, UINT64 ErrorCode) {
    (VOID)FaultAddr;
    (VOID)ErrorCode;

    return -1;
}

VOID PageFaultHandler(InterruptFrame *Frame) {
    UINT64 FaultAddr = GetPageFaultAddress();
    UINT64 ErrorCode = Frame->ErrorCode;
    
    // We check which mode the exception originated from.
    BOOL FromUser = ExceptionFromUser(Frame);
    Task *Current = GetCurrentTask();
    
    // === RESTORE ATTEMPT ===
    if (FromUser && Current && Current != &InitTask) {
        // We are trying to correct the situation for a user task
        INT Result = HandleUserPageFault(Current, FaultAddr, ErrorCode);
        
        if (Result == 0) {
            return;  // Returning to user mode
        }
        
        SchedulerKill(Current->Pid);
    }
    
    // === KERNEL: Recovery attempt ===
    if (!FromUser) {
        INT Result = HandleKernelPageFault(FaultAddr, ErrorCode);
        
        if (Result == 0) {
            return;
        }
        FatalF("page fault", Frame);
    }
    
    FatalF("page fault", Frame);
    for (;;) Halt();
}
