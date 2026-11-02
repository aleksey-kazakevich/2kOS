#pragma once

#include <Asm/Cpu.h>
#include <Types.h>

typedef struct {
    volatile UINT32 Locked;
} SpinLock;

STATIC INLINE VOID SpinLockInit(SpinLock *Lock) {
    Lock->Locked = 0;
}

STATIC INLINE VOID SpinLockAcquire(SpinLock *Lock) {
    while (__sync_lock_test_and_set(&Lock->Locked, 1)) {
        CpuPause();
    }
}

STATIC INLINE VOID SpinLockRelease(SpinLock *Lock) {
    __sync_lock_release(&Lock->Locked);
}

STATIC INLINE VOID SpinLockAcquireUINT32(volatile UINT32 *Lock) {
    while (__sync_lock_test_and_set(Lock, 1)) {
        while (*Lock) CpuPause();
    }
    __sync_synchronize();
}

STATIC INLINE VOID SpinLockReleaseUINT32(volatile UINT32 *Lock) {
    __sync_synchronize();
    __sync_lock_release(Lock);
}

STATIC INLINE BOOL SpinLockTryAcquire(SpinLock *Lock) {
    return __sync_lock_test_and_set(&Lock->Locked, 1) == 0;
}

STATIC INLINE UINTPTR SpinLockAcquireIrq(SpinLock *Lock) {
    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    while (__sync_lock_test_and_set(&Lock->Locked, 1)) {
        while (Lock->Locked) CpuPause();
    }

    return Flags;
}

STATIC INLINE VOID SpinLockReleaseIrq(SpinLock *Lock, UINTPTR Flags) {
    __sync_lock_release(&Lock->Locked);
    RestoreFlags(Flags);
}