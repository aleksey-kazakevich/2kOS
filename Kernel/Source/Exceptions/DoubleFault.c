#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>
#include <Fatal.h>

VOID DoubleFaultHandler(InterruptFrame *Frame) {
    
    /* Panic - recovery is impossible */
    FatalF("double fault", Frame);
    
    for (;;) Halt();
}
