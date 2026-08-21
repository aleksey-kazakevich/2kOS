#include <Types.h>
#include <Fatal.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID MachineCheckHandler(InterruptFrame *Frame) {
    FatalF("machine check", Frame);
    
    for (;;) Halt();
}
