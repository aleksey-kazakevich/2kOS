#include <Types.h>
#include <Fatal.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID NmiHandler(InterruptFrame *Frame) {
    FatalF("nmi", Frame);

    for (;;) Halt();
}
