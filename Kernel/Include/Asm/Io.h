#pragma once

#include <Types.h>

static inline UINT8 Inb(UINT16 Port)
{
    UINT8 Ret;
    asm volatile("inb %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline VOID Outb(UINT16 Port, UINT8 Data)
{
    asm volatile("outb %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline UINT16 Inw(UINT16 Port)
{
    UINT16 Ret;
    asm volatile("inw %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline VOID Outw(UINT16 Port, UINT16 Data)
{
    asm volatile("outw %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline UINT32 Inl(UINT16 Port)
{
    UINT32 Ret;
    asm volatile("inl %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline VOID Outl(UINT16 Port, UINT32 Data)
{
    asm volatile("outl %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline VOID IoWait(VOID) {
    Outb(0x80, 0);  // Write to unused port 0x80 (POST card)
}

#define DEBUG_PORT 0xE9

static inline VOID DebugPutChar(CHAR C) {
    Outb(DEBUG_PORT, C);  // QEMU debug port
}

static inline VOID DebugPutString(const CHAR *Str) {
    while (*Str) {
        Outb(DEBUG_PORT, *Str++);
    }
}

static inline VOID DebugPutHex(UINT64 Val) {
    CHAR Buf[17];
    const CHAR *Hex = "0123456789ABCDEF";
    Buf[16] = '\0';
    
    // Fill in strictly from end to beginning
    for (INT I = 15; I >= 0; I--) {
        Buf[I] = Hex[Val & 0xF]; // Take the least significant 4 bits
        Val >>= 4;               // Shift the number to the right
    }
    
    DebugPutString(Buf);
}
