#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID DeviceNotAvailableHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "device not available");
}
