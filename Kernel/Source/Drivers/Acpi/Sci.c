#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Acpi/Sci.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SpinLock.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Return.h>
#include <Idt.h>

// GPE registers (General Purpose Event)
#define ACPI_GPE0_BLK      0x00
#define ACPI_GPE1_BLK      0x01

// GPE status bits
#define ACPI_GPE_STATUS    0x00
#define ACPI_GPE_ENABLE    0x01

// PM1 status bits
#define ACPI_PM1_STATUS    0x00
#define ACPI_PM1_ENABLE    0x02
#define ACPI_PM1_CONTROL   0x04

// PM1 status bits
#define ACPI_PM1_PWRBTN    (1 << 8)   // Power button
#define ACPI_PM1_SLPBTN    (1 << 9)   // Sleep button
#define ACPI_PM1_RTC       (1 << 10)  // RTC alarm
#define ACPI_PM1_WAKE      (1 << 15)  // Wake event

// PM1 control bits
#define ACPI_PM1_SCI_EN    (1 << 0)   // Enable SCI
#define ACPI_PM1_BM_RLD    (1 << 1)   // Bus Master Reload
#define ACPI_PM1_GLOBAL_LOCK (1 << 2) // Global Lock
#define ACPI_PM1_SLP_TYP   (7 << 10)  // Sleep type
#define ACPI_PM1_SLP_EN    (1 << 13)  // Sleep enable

typedef struct {
    BOOL Initialized;
    BOOL Enabled;
    UINT32 Gsi;                      // GSI for SCI
    UINT8 Vector;                    // Interrupt vector
    UINT32 LastEvent;
    
    // Register addresses (virt)
    volatile VOID *Pm1aStatus;       // PM1a Status Register
    volatile VOID *Pm1aEnable;       // PM1a Enable Register
    volatile VOID *Pm1bStatus;       // PM1b Status Register (optional)
    volatile VOID *Pm1bEnable;       // PM1b Enable Register (optional)
    volatile VOID *Pm1aControl;      // PM1a Control Register
    
    volatile VOID *Gpe0Status;       // GPE0 Status
    volatile VOID *Gpe0Enable;       // GPE0 Enable
    volatile VOID *Gpe1Status;       // GPE1 Status (optional)
    volatile VOID *Gpe1Enable;       // GPE1 Enable (optional)
    
    // GPE block sizes
    UINT8 Gpe0Len;
    UINT8 Gpe1Len;
    
    // For event waiting
    BOOL EventPending;
    UINT32 PendingEvent;
    SpinLock Lock;
} AcpiSciState;

STATIC AcpiSciState GAcpiSci = {0};

STATIC UINT32 AcpiSciReadReg(volatile VOID *Addr, UINT8 Width) {
    if (!Addr) return 0;
    
    switch (Width) {
        case 0:  // System Memory
            return MmioRead32(Addr);
        case 1:  // System I/O
            return Inw((UINT16)(UINTPTR)Addr);
        default:
            return 0;
    }
}

STATIC VOID AcpiSciWriteReg(volatile VOID *Addr, UINT8 Width, UINT32 Value) {
    if (!Addr) return;
    
    switch (Width) {
        case 0:  // System Memory
            MmioWrite32(Addr, Value);
            break;
        case 1:  // System I/O
            Outw((UINT16)(UINTPTR)Addr, (UINT16)Value);
            break;
    }
}

STATIC VOID AcpiSciParseRegisters(VOID) {
    Acpi *AcpiObj = AcpiGet();
    if (!AcpiObj || !AcpiObj->Fadt) return;
    
    FADT *Fadt = AcpiObj->Fadt;
    
    // PM1a Status (first PM1a part)
    UINT8 *Pm1aStatusBlk = Fadt->XPm1aEvtBlk;  // Generic Address Structure
    if (Pm1aStatusBlk[0] != 0) {  // Non-zero
        UINT8 SpaceId = Pm1aStatusBlk[0];
        UINT64 Addr = *(UINT64*)(Pm1aStatusBlk + 4);
        UINT8 BitWidth = Pm1aStatusBlk[1];
        UINT8 BitOffset = Pm1aStatusBlk[2];
        
        if (SpaceId == 0) {  // System Memory
            GAcpiSci.Pm1aStatus = (volatile VOID*)PhysToVirt(Addr);
        } else if (SpaceId == 1) {  // System I/O
            GAcpiSci.Pm1aStatus = (volatile VOID*)(UINTPTR)Addr;
        }
        
        // PM1a Enable after Status
        if (GAcpiSci.Pm1aStatus) {
            GAcpiSci.Pm1aEnable = (volatile VOID*)((UINTPTR)GAcpiSci.Pm1aStatus + 2);
        }
    }
    
    // PM1a Control
    UINT8 *Pm1aCntBlk = Fadt->XPm1aCntBlk;
    if (Pm1aCntBlk[0] != 0) {
        UINT8 SpaceId = Pm1aCntBlk[0];
        UINT64 Addr = *(UINT64*)(Pm1aCntBlk + 4);
        
        if (SpaceId == 0) {
            GAcpiSci.Pm1aControl = (volatile VOID*)PhysToVirt(Addr);
        } else if (SpaceId == 1) {
            GAcpiSci.Pm1aControl = (volatile VOID*)(UINTPTR)Addr;
        }
    }
    
    // GPE0 block
    UINT8 *Gpe0Blk = Fadt->XGpe0Blk;
    if (Gpe0Blk[0] != 0) {
        UINT8 SpaceId = Gpe0Blk[0];
        UINT64 Addr = *(UINT64*)(Gpe0Blk + 4);
        
        if (SpaceId == 0) {
            GAcpiSci.Gpe0Status = (volatile VOID*)PhysToVirt(Addr);
        } else if (SpaceId == 1) {
            GAcpiSci.Gpe0Status = (volatile VOID*)(UINTPTR)Addr;
        }
        
        GAcpiSci.Gpe0Enable = (volatile VOID*)((UINTPTR)GAcpiSci.Gpe0Status + 2);
        GAcpiSci.Gpe0Len = Fadt->XGpe0BlkLen;
    }
    
    // GPE1 block (if present)
    UINT8 *Gpe1Blk = Fadt->XGpe1Blk;
    if (Gpe1Blk[0] != 0 && Fadt->XGpe1BlkLen > 0) {
        UINT8 SpaceId = Gpe1Blk[0];
        UINT64 Addr = *(UINT64*)(Gpe1Blk + 4);
        
        if (SpaceId == 0) {
            GAcpiSci.Gpe1Status = (volatile VOID*)PhysToVirt(Addr);
        } else if (SpaceId == 1) {
            GAcpiSci.Gpe1Status = (volatile VOID*)(UINTPTR)Addr;
        }
        
        GAcpiSci.Gpe1Enable = (volatile VOID*)((UINTPTR)GAcpiSci.Gpe1Status + 2);
        GAcpiSci.Gpe1Len = Fadt->XGpe1BlkLen;
    }
}

VOID AcpiSciEnable(VOID) {
    if (!GAcpiSci.Initialized) return;
    if (GAcpiSci.Enabled) return;
    
    // Enable SCI in PM1 Control
    if (GAcpiSci.Pm1aControl) {
        UINT32 Value = AcpiSciReadReg(GAcpiSci.Pm1aControl, 1);
        Value |= ACPI_PM1_SCI_EN;
        AcpiSciWriteReg(GAcpiSci.Pm1aControl, 1, Value);
    }
    
    // Enable IOAPIC interrupt
    IoapicUnmaskIrq(GAcpiSci.Gsi);
    
    GAcpiSci.Enabled = TRUE;
    StdOutPrintf("acpi: sci enabled (gsi=%d, vector=%d)\n", 
                  GAcpiSci.Gsi, GAcpiSci.Vector);
}

VOID AcpiSciDisable(VOID) {
    if (!GAcpiSci.Initialized || !GAcpiSci.Enabled) return;
    
    // Disable SCI in PM1 Control
    if (GAcpiSci.Pm1aControl) {
        UINT32 Value = AcpiSciReadReg(GAcpiSci.Pm1aControl, 1);
        Value &= ~ACPI_PM1_SCI_EN;
        AcpiSciWriteReg(GAcpiSci.Pm1aControl, 1, Value);
    }
    
    // Mask interrupt
    IoapicMaskIrq(GAcpiSci.Gsi);
    
    GAcpiSci.Enabled = FALSE;
}

UINT32 AcpiSciGetStatus(VOID) {
    UINT32 Status = 0;
    
    // Read PM1 status
    if (GAcpiSci.Pm1aStatus) {
        Status |= AcpiSciReadReg(GAcpiSci.Pm1aStatus, 1);
    }
    if (GAcpiSci.Pm1bStatus) {
        Status |= AcpiSciReadReg(GAcpiSci.Pm1bStatus, 1);
    }
    
    // Read GPE0 status
    if (GAcpiSci.Gpe0Status) {
        Status |= AcpiSciReadReg(GAcpiSci.Gpe0Status, 1);
    }
    
    return Status;
}

VOID AcpiSciClearStatus(UINT32 Status) {
    if (GAcpiSci.Pm1aStatus) {
        AcpiSciWriteReg(GAcpiSci.Pm1aStatus, 1, Status & 0xFFFF);
    }
    if (GAcpiSci.Pm1bStatus) {
        AcpiSciWriteReg(GAcpiSci.Pm1bStatus, 1, Status & 0xFFFF);
    }
    if (GAcpiSci.Gpe0Status) {
        AcpiSciWriteReg(GAcpiSci.Gpe0Status, 1, Status & 0xFFFF);
    }
}

BOOL AcpiSciIsPowerButtonPressed(VOID) {
    UINT32 Status = AcpiSciGetStatus();
    return (Status & ACPI_PM1_PWRBTN) != 0;
}

VOID AcpiSciHandler(VOID) {
    // Get status
    UINT32 Status = AcpiSciGetStatus();
    
    if (Status == 0) {
        // Spurious interrupt
        ApicEoi();
        return;
    }
    
    // Save status
    GAcpiSci.LastEvent = Status;
    
    // Clear status
    AcpiSciClearStatus(Status);
    
    // Process event
    if (Status & ACPI_PM1_PWRBTN) {
        StdOutPrintf("acpi: power button pressed!\n");
        AcpiShutdown();
    }
    
    SpinLockAcquire(&GAcpiSci.Lock);
    GAcpiSci.EventPending = TRUE;
    GAcpiSci.PendingEvent = Status;
    SpinLockRelease(&GAcpiSci.Lock);
    
    // EOI
    ApicEoi();
}

INT AcpiSciWaitEvent(UINT32 TimeoutMs) {
    UINT64 Start = TimerTicks();
    UINT64 Timeout = (UINT64)TimeoutMs * TimerFreq() / 1000;
    
    while (1) {
        SpinLockAcquire(&GAcpiSci.Lock);
        if (GAcpiSci.EventPending) {
            GAcpiSci.EventPending = FALSE;
            SpinLockRelease(&GAcpiSci.Lock);
            return SUCCESS;
        }
        SpinLockRelease(&GAcpiSci.Lock);
        
        if (TimeoutMs > 0 && (TimerTicks() - Start) >= Timeout) {
            return TIMEOUT;
        }
        CpuPause();
    }
}

UINT32 AcpiSciGetLastEvent(VOID) {
    return GAcpiSci.LastEvent;
}

EXTERN(VOID, AcpiSciIrq());

INT AcpiSciInit(VOID) {
    UINT32 Gsi, Flags;
    INT Result;
    
    if (GAcpiSci.Initialized) {
        RETURN(SUCCESS);
    }
    
    Acpi *AcpiObj = AcpiGet();
    if (!AcpiObj || !AcpiObj->Fadt) {
        StdErrPrintf("acpi: no FADT for SCI\n");
        RETURN(NO_OBJECT);
    }
    
    FADT *Fadt = AcpiObj->Fadt;
    
    // Get GSI for SCI
    Gsi = Fadt->SciInt;
    if (Gsi == 0) {
        StdErrPrintf("acpi: SCI GSI is 0\n");
        RETURN(INCORRECT_VALUE);
    }
    
    Result = IoapicGetOverride(Gsi, &Gsi, &Flags);
    if (Result != SUCCESS) {
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    GAcpiSci.Gsi = Gsi;
    GAcpiSci.Vector = 0x50;  // Using vector 0x50 for SCI
    
    // Parsing registers
    AcpiSciParseRegisters();
    
    if (!GAcpiSci.Pm1aStatus) {
        StdErrPrintf("acpi: no PM1a status register\n");
        RETURN(NO_OBJECT);
    }
    
    // Setting IDT
    IdtSetGate(GAcpiSci.Vector, (VOID(*)())AcpiSciIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    
    // Setting IOAPIC
    Result = IoapicRedirectIrq(Gsi, GAcpiSci.Vector, ApicGetId(), Flags);
    if (Result != SUCCESS) {
        StdErrPrintf("acpi: IOAPIC redirect failed\n");
        RETURN(Result);
    }
    
    // Initializing spinlock
    SpinLockInit(&GAcpiSci.Lock);
    
    // Enable SCI
    AcpiSciEnable();
    
    GAcpiSci.Initialized = TRUE;
    
    StdOutPrintf("acpi: sci initialized (gsi=%d, vector=%d)\n", 
                  Gsi, GAcpiSci.Vector);
    
    RETURN(SUCCESS);
}

UINT32 AcpiSciGetGsi(VOID) {
    return GAcpiSci.Gsi;
}