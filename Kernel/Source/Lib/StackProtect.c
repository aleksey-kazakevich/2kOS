#include <Types.h>
#include <Asm/Cpu.h>

UINTPTR __stack_chk_guard = 0xBAAAD00Du;

// Add these attributes so that LTO does not touch the function
static VOID ATTRIBUTE(noreturn) ATTRIBUTE(used) ATTRIBUTE(noinline) 
KStackPanic(VOID) {
    LocalInterruptsDisable();
    for (;;) Halt();
    __builtin_unreachable();
}

VOID ATTRIBUTE(noreturn) ATTRIBUTE(used) __stack_chk_fail(VOID) {
    KStackPanic();
}

VOID ATTRIBUTE(noreturn) ATTRIBUTE(used) __stack_chk_fail_local(VOID) {
    KStackPanic();
}
