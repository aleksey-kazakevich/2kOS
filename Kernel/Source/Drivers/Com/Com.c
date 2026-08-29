#include <Drivers/Com.h>
#include <Lib/String.h>
#include <Lib/StdArg.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Asm/Io.h>
#include <Return.h>

static ComDevice GComDevices[4] = {
    {.Port = COM1_PORT, .Irq = COM_IRQ1, .Initialized = FALSE},
    {.Port = COM2_PORT, .Irq = COM_IRQ2, .Initialized = FALSE},
    {.Port = COM3_PORT, .Irq = COM_IRQ1, .Initialized = FALSE},
    {.Port = COM4_PORT, .Irq = COM_IRQ2, .Initialized = FALSE}
};

static ComDevice *GDefaultCom = NULLPTR;

static inline VOID ComWriteReg(ComDevice *Dev, UINT8 Offset, UINT8 Value) {
    Outb(Dev->Port + Offset, Value);
}

static inline UINT8 ComReadReg(ComDevice *Dev, UINT8 Offset) {
    return Inb(Dev->Port + Offset);
}

static BOOL ComPortExists(UINT16 Port) {
    Outb(Port + COM_SCR, 0xAA);
    UINT8 Test = Inb(Port + COM_SCR);
    Outb(Port + COM_SCR, 0x55);
    UINT8 Test2 = Inb(Port + COM_SCR);
    
    return (Test == 0xAA && Test2 == 0x55);
}

static VOID ComSetupBaud(ComDevice *Dev, UINT32 BaudRate) {
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

static VOID ComSetupLine(ComDevice *Dev) {
    UINT8 Lcr = COM_LCR_8BITS | COM_LCR_1_STOP;
    ComWriteReg(Dev, COM_LCR, Lcr);
}

static VOID ComSetupFifo(ComDevice *Dev) {
    UINT8 Fcr = COM_FCR_ENABLE | COM_FCR_CLEAR_RX | COM_FCR_CLEAR_TX;
    ComWriteReg(Dev, COM_FCR, Fcr);
}

static VOID ComSetupModem(ComDevice *Dev) {
    // DTR, RTS, OUT2
    UINT8 Mcr = COM_MCR_DTR | COM_MCR_RTS | COM_MCR_OUT2;
    ComWriteReg(Dev, COM_MCR, Mcr);
}

static VOID ComDisableInterrupts(ComDevice *Dev) {
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
