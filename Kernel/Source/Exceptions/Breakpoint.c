#include <Interrupt.h>
#include <Types.h>

VOID BreakpointHandler(InterruptFrame *Frame) {
    Frame->RIP += 1;
    
    return;
}
