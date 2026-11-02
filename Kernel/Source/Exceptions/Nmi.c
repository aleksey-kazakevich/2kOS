#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>
#include <Fatal.h>

EXTERN(volatile UINT32, GCpuStopRequested);
EXTERN(VOID, NmiStopHandler(VOID));

VOID NmiHandler(InterruptFrame *Frame) {
    if (GCpuStopRequested) {
        NmiStopHandler();
    }

    FatalF("nmi", Frame);
}
