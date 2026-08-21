#include <Types.h>
#include <Interrupt.h>

VOID BreakpointHandler(InterruptFrame *Frame) {
    Frame->RIP += 1;
    
    return;
}
