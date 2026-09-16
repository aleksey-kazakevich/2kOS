#include <Drivers/Com.h>
#include <Lib/String.h>
#include <Lib/StdArg.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Asm/Io.h>
#include <Return.h>

STATIC ComDevice GComDevices[4] = {
    {.Port = COM1_PORT, .Irq = COM_IRQ1, .Initialized = FALSE},
    {.Port = COM2_PORT, .Irq = COM_IRQ2, .Initialized = FALSE},
    {.Port = COM3_PORT, .Irq = COM_IRQ1, .Initialized = FALSE},
    {.Port = COM4_PORT, .Irq = COM_IRQ2, .Initialized = FALSE}
};

STATIC ComDevice *GDefaultCom = NULLPTR;

STATIC INLINE VOID ComWriteReg(ComDevice *Dev, UINT8 Offset, UINT8 Value) {
    Outb(Dev->Port + Offset, Value);
}

STATIC INLINE UINT8 ComReadReg(ComDevice *Dev, UINT8 Offset) {
    return Inb(Dev->Port + Offset);
}

STATIC BOOL ComPortExists(UINT16 Port) {
    Outb(Port + COM_SCR, 0xAA);
    UINT8 Test = Inb(Port + COM_SCR);
    Outb(Port + COM_SCR, 0x55);
    UINT8 Test2 = Inb(Port + COM_SCR);
    
    return (Test == 0xAA && Test2 == 0x55);
}

STATIC VOID ComSetupBaud(ComDevice *Dev, UINT32 BaudRate) {
    UINT16 Divisor = 115200 / BaudRate;
    
    if (Divisor == 0) Divisor = 1;
    
    UINT8 Lcr = ComReadReg(Dev, COM_LCR);
    Lcr |= COM_LCR_DLAB;
    ComWriteReg(Dev, COM_LCR, Lcr);
    
    ComWriteReg(Dev, COM_DATA, Divisor & 0xFF);
    ComWriteReg(Dev, COM_IER, (Divisor >> 8) & 0xFF);
    
    // Turn off DLAB
    Lcr &= ~COM_LCR_DLAB;
    ComWriteReg(Dev, COM_LCR, Lcr);
}

STATIC VOID ComSetupLine(ComDevice *Dev) {
    UINT8 Lcr = COM_LCR_8BITS | COM_LCR_1_STOP;
    ComWriteReg(Dev, COM_LCR, Lcr);
}

STATIC VOID ComSetupFifo(ComDevice *Dev) {
    UINT8 Fcr = COM_FCR_ENABLE | COM_FCR_CLEAR_RX | COM_FCR_CLEAR_TX;
    ComWriteReg(Dev, COM_FCR, Fcr);
}

STATIC VOID ComSetupModem(ComDevice *Dev) {
    // DTR, RTS, OUT2
    UINT8 Mcr = COM_MCR_DTR | COM_MCR_RTS | COM_MCR_OUT2;
    ComWriteReg(Dev, COM_MCR, Mcr);
}

STATIC VOID ComDisableInterrupts(ComDevice *Dev) {
    ComWriteReg(Dev, COM_IER, 0);
}

INT ComInit(UINT16 Port, UINT32 BaudRate) {
    ComDevice *Dev = NULLPTR;
    for (INT I = 0; I < 4; I++) {
        if (GComDevices[I].Port == Port) {
            Dev = &GComDevices[I];
            break;
        }
    }
    
    if (!Dev) RETURN(NO_OBJECT);
    if (Dev->Initialized) RETURN(SUCCESS);
    
    if (!ComPortExists(Port)) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "com: port 0x%X does not exist\n", Port);
        RETURN(NO_OBJECT);
    }
    
    SpinLockAcquire(&Dev->Lock);
    
    ComDisableInterrupts(Dev);
    
    ComSetupBaud(Dev, BaudRate);
    
    ComSetupLine(Dev);
    
    ComSetupFifo(Dev);
    
    ComSetupModem(Dev);
    
    Dev->BaudRate = BaudRate;
    Dev->Initialized = TRUE;
    Dev->TransmitReady = TRUE;
    Dev->ReceiveReady = FALSE;
    
    if (!Dev->Driver) {
        CHAR Name[32];
        SnPrintf(Name, sizeof(Name), "COM%u", (Port - COM1_PORT) + 1);
        Dev->Driver = KDriverGenerateStruct(Name, DCL1, TRUE, Dev, NULLPTR);
        KDriverRegister(Dev->Driver);
    }
    
    if (Port == COM1_PORT && !GDefaultCom) {
        GDefaultCom = Dev;
    }
    
    SpinLockRelease(&Dev->Lock);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "com: initialized port 0x%X at %u baud\n", 
                  Port, BaudRate);
    
    RETURN(SUCCESS);
}

INT ComInitAll(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "com: probing ports...\n");
    
    INT Found = 0;
    
    UINT16 Ports[] = {COM1_PORT, COM2_PORT, COM3_PORT, COM4_PORT};
    
    for (INT I = 0; I < 4; I++) {
        if (ComPortExists(Ports[I])) {
            if (ComInit(Ports[I], COM_BAUD_115200) == SUCCESS) {
                Found++;
            }
        }
    }
    
    ComRegisterSysDevices();

    BaseconPrintf(BASECON_TYPE_NORMAL, "com: found %d ports\n", Found);
    return Found;
}

VOID ComPutChar(ComDevice *Dev, CHAR C) {
    if (!Dev || !Dev->Initialized) return;
    
    SpinLockAcquire(&Dev->Lock);
    
    while (!(ComReadReg(Dev, COM_LSR) & COM_LSR_TX_EMPTY)) {
        asm volatile ("pause");
    }
    
    // Sending a byte
    ComWriteReg(Dev, COM_DATA, (UINT8)C);
    
    SpinLockRelease(&Dev->Lock);
}

VOID ComPutString(ComDevice *Dev, const CHAR *Str) {
    if (!Dev || !Str) return;
    
    while (*Str) {
        if (*Str == '\n') {
            ComPutChar(Dev, '\r');
        }
        ComPutChar(Dev, *Str++);
    }
}

INT ComPrintf(ComDevice *Dev, const CHAR *Fmt, ...) {
    if (!Dev || !Fmt) RETURN(INCORRECT_VALUE);
    
    CHAR Buffer[1024];
    VA_LIST Args;
    
    VaStart(Args, Fmt);
    INT Result = VsnPrintf(Buffer, sizeof(Buffer), Fmt, Args);
    VaEnd(Args);
    
    if (Result > 0) {
        ComPutString(Dev, Buffer);
    }
    
    RETURN(Result);
}

BOOL ComDataAvailable(ComDevice *Dev) {
    if (!Dev || !Dev->Initialized) return FALSE;
    return (ComReadReg(Dev, COM_LSR) & COM_LSR_RX_READY) != 0;
}

CHAR ComGetChar(ComDevice *Dev) {
    if (!Dev || !Dev->Initialized) return 0;
    
    SpinLockAcquire(&Dev->Lock);
    
    while (!ComDataAvailable(Dev)) {
        asm volatile ("pause");
    }
    
    CHAR C = (CHAR)ComReadReg(Dev, COM_DATA);
    
    SpinLockRelease(&Dev->Lock);
    return C;
}

ComDevice* ComGetDevice(UINT16 Port) {
    for (INT I = 0; I < 4; I++) {
        if (GComDevices[I].Port == Port && GComDevices[I].Initialized) {
            return &GComDevices[I];
        }
    }
    return NULLPTR;
}

ComDevice* ComGetDefault(VOID) {
    return GDefaultCom;
}

VOID ComFlush(ComDevice *Dev) {
    if (!Dev || !Dev->Initialized) return;
    
    SpinLockAcquire(&Dev->Lock);
    
    while (!(ComReadReg(Dev, COM_LSR) & COM_LSR_TX_IDLE)) {
        asm volatile ("pause");
    }
    
    SpinLockRelease(&Dev->Lock);
}

VOID ComSetBaudRate(ComDevice *Dev, UINT32 BaudRate) {
    if (!Dev || !Dev->Initialized) return;
    
    SpinLockAcquire(&Dev->Lock);
    ComSetupBaud(Dev, BaudRate);
    Dev->BaudRate = BaudRate;
    SpinLockRelease(&Dev->Lock);
}

STATIC UINT8 ComPortToIndex(UINT16 Port) {
    switch (Port) {
        case COM1_PORT: return 1;
        case COM2_PORT: return 2;
        case COM3_PORT: return 3;
        case COM4_PORT: return 4;
        default:        return 0;
    }
}

STATIC UINT8 ComPortToIrq(UINT16 Port) {
    switch (Port) {
        case COM1_PORT:
        case COM3_PORT: return COM_IRQ1;   // IRQ 4
        case COM2_PORT:
        case COM4_PORT: return COM_IRQ2;   // IRQ 3
        default:        return 0;
    }
}

STATIC VOID ComRegisterOne(ComDevice *Dev, UINT8 Index) {
    if (!Dev || !Dev->Initialized) return;

    CHAR Name[SYSDEV_NAME_MAX];
    SnPrintf(Name, sizeof(Name), "COM%u", Index);

    CHAR Desc[SYSDEV_NAME_MAX];
    SnPrintf(Desc, sizeof(Desc), "Serial port at 0x%X", Dev->Port);

    SysDevice *Parent = SysDeviceGetRoot();
    SysDevice *Sd = SysDeviceCreate(Parent, Name, Desc, SYSDEV_TYPE_SERIAL);
    if (!Sd) return;

    // --- Ресурсы ---

    // I/O порт (8 регистров)
    SysDeviceAddResource(Sd, SYSDEV_RES_IO, Dev->Port, 8);

    // IRQ
    UINT8 Irq = ComPortToIrq(Dev->Port);
    if (Irq) {
        SysDeviceAddResource(Sd, SYSDEV_RES_IRQ, Irq, 1);
    }

    // --- Свойства ---

    SysDeviceSetPropU32(Sd, "port", Dev->Port);
    SysDeviceSetPropU32(Sd, "irq", Irq);
    SysDeviceSetPropU32(Sd, "baud", Dev->BaudRate);

    // --- Привязка к KDriver ---

    if (Dev->Driver) {
        SysDeviceBindDriver(Sd, Dev->Driver);
    } else {
        SysDeviceSetState(Sd, SYSDEV_STATE_ENABLED);
    }
}

UINT32 ComRegisterSysDevices(VOID) {
    if (!SysDeviceGetRoot()) {
        return 0;
    }

    UINT32 Count = 0;
    for (INT I = 0; I < 4; I++) {
        ComDevice *Dev = &GComDevices[I];
        if (!Dev->Initialized) continue;

        UINT8 Index = ComPortToIndex(Dev->Port);
        if (Index == 0) continue;

        ComRegisterOne(Dev, Index);
        Count++;
    }

    return Count;
}
