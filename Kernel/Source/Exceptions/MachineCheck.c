#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>
#include <Fatal.h>

VOID MachineCheckHandler(InterruptFrame *Frame) {
    FatalF("machine check", Frame);
    
    for (;;) Halt();
}
