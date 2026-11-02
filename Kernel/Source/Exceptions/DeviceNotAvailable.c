#include <Utils/ExceptionLog.h>
#include <ExceptionHelper.h>
#include <Interrupt.h>
#include <Asm/Cpu.h>
#include <Types.h>

VOID DeviceNotAvailableHandler(InterruptFrame *Frame) {
    if (ExceptionFromUser(Frame)) {
        ExceptionLogPush(EXC_DEVICE_NOT_AVAILABLE, Frame);
    }
    HANDLE_EXCEPTION(Frame, "device not available");
}
