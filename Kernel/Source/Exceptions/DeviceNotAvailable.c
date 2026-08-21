#include <Types.h>
#include <ExceptionHelper.h>
#include <Asm/Cpu.h>
#include <Interrupt.h>

VOID DeviceNotAvailableHandler(InterruptFrame *Frame) {
    HANDLE_EXCEPTION(Frame, "device not available");
}
