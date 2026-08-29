#pragma once

#include <SpinLock.h>
#include <KDriver.h>
#include <Types.h>

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

#define COM_IRQ1  4
#define COM_IRQ2  3

#define COM_DATA      0
#define COM_IER       1
#define COM_IIR       2
#define COM_FCR       2
#define COM_LCR       3
#define COM_MCR       4
#define COM_LSR       5
#define COM_MSR       6
#define COM_SCR       7

// IER (Interrupt Enable Register)
#define COM_IER_RX_DATA   (1 << 0)
#define COM_IER_TX_EMPTY  (1 << 1)
#define COM_IER_LINE      (1 << 2)
#define COM_IER_MODEM     (1 << 3)

// IIR (Interrupt Identity Register)
#define COM_IIR_NO_INT    0x01
#define COM_IIR_RX_DATA   0x04
#define COM_IIR_TX_EMPTY  0x02
#define COM_IIR_LINE      0x06

// FCR (FIFO Control Register)
#define COM_FCR_ENABLE    (1 << 0)
#define COM_FCR_CLEAR_RX  (1 << 1)
#define COM_FCR_CLEAR_TX  (1 << 2)
#define COM_FCR_DMA       (1 << 3)
#define COM_FCR_LEVEL_1   (0 << 6)
#define COM_FCR_LEVEL_4   (1 << 6)
#define COM_FCR_LEVEL_8   (2 << 6)
#define COM_FCR_LEVEL_14  (3 << 6)

// LCR (Line Control Register)
#define COM_LCR_DLAB      (1 << 7)
#define COM_LCR_5BITS     0
#define COM_LCR_6BITS     1
#define COM_LCR_7BITS     2
#define COM_LCR_8BITS     3
#define COM_LCR_1_STOP    0
#define COM_LCR_2_STOP    (1 << 2)
#define COM_LCR_PARITY    (1 << 3)
#define COM_LCR_EVEN      (1 << 4)
#define COM_LCR_STICK     (1 << 5)
#define COM_LCR_BREAK     (1 << 6)

// MCR (Modem Control Register)
#define COM_MCR_DTR       (1 << 0)
#define COM_MCR_RTS       (1 << 1)
#define COM_MCR_OUT1      (1 << 2)
#define COM_MCR_OUT2      (1 << 3)
#define COM_MCR_LOOP      (1 << 4)

// LSR (Line Status Register)
#define COM_LSR_RX_READY  (1 << 0)
#define COM_LSR_OVERRUN   (1 << 1)
#define COM_LSR_PARITY    (1 << 2)
#define COM_LSR_FRAMING   (1 << 3)
#define COM_LSR_BREAK     (1 << 4)
#define COM_LSR_TX_EMPTY  (1 << 5)
#define COM_LSR_TX_IDLE   (1 << 6)
#define COM_LSR_FIFO_ERR  (1 << 7)

#define COM_BAUD_115200   1
#define COM_BAUD_57600    2
#define COM_BAUD_38400    3
#define COM_BAUD_19200    6
#define COM_BAUD_9600     12
#define COM_BAUD_4800     24
#define COM_BAUD_2400     48
#define COM_BAUD_1200     96

typedef struct {
    UINT16 Port;
    UINT8  Irq;
    UINT32 BaudRate;
    BOOL   Initialized;
    BOOL   TransmitReady;
    BOOL   ReceiveReady;
    KDriver *Driver;
    SpinLock Lock;
} ComDevice;

// COM-port init
INT ComInit(UINT16 Port, UINT32 BaudRate);
INT ComInitAll(VOID);  // Initializes all ports

// Base operations
VOID ComPutChar(ComDevice *Dev, CHAR C);
VOID ComPutString(ComDevice *Dev, const CHAR *Str);
INT  ComPrintf(ComDevice *Dev, const CHAR *Fmt, ...);

// Read
BOOL ComDataAvailable(ComDevice *Dev);
CHAR ComGetChar(ComDevice *Dev);

// Get devices
ComDevice* ComGetDevice(UINT16 Port);
ComDevice* ComGetDefault(VOID);  // COM1

// Utils
VOID ComFlush(ComDevice *Dev);
VOID ComSetBaudRate(ComDevice *Dev, UINT32 BaudRate);
