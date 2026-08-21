#pragma once

#include <Types.h>

static inline UINT8 MmioRead8(volatile VOID* Addr) {
    return *(volatile UINT8*)Addr;
}

static inline UINT16 MmioRead16(volatile VOID* Addr) {
    return *(volatile UINT16*)Addr;
}

static inline UINT32 MmioRead32(volatile VOID* Addr) {
    return *(volatile UINT32*)Addr;
}

static inline UINT64 MmioRead64(volatile VOID* Addr) {
    return *(volatile UINT64*)Addr;
}

static inline VOID MmioWrite8(volatile VOID* Addr, UINT8 Value) {
    *(volatile UINT8*)Addr = Value;
}

static inline VOID MmioWrite16(volatile VOID* Addr, UINT16 Value) {
    *(volatile UINT16*)Addr = Value;
}

static inline VOID MmioWrite32(volatile VOID* Addr, UINT32 Value) {
    *(volatile UINT32*)Addr = Value;
}

static inline VOID MmioWrite64(volatile VOID* Addr, UINT64 Value) {
    *(volatile UINT64*)Addr = Value;
}

#define MmioRead(Addr) MmioRead32(Addr)
#define MmioWrite(Addr, Value) MmioWrite32(Addr, Value)