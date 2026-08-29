#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>
#include <Fatal.h>

VOID NmiHandler(InterruptFrame *Frame) {
    FatalF("nmi", Frame);

    for (;;) Halt();
}
