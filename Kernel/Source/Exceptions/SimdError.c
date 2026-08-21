#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID SimdErrorHandler(InterruptFrame *Frame) {
    UINT32 Mxcsr;
    
    /* Reading MXCSR*/
    asm volatile("stmxcsr %0" : "=m"(Mxcsr));
    
    HANDLE_EXCEPTION(Frame, "simd error");
}
