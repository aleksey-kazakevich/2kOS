#include <Types.h>
#include <Fatal.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID DoubleFaultHandler(InterruptFrame *Frame) {
    
    /* Panic - recovery is impossible*/
    FatalF("double fault", Frame);
    
    for (;;) Halt();
}
