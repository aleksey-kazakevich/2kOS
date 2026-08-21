#include <Types.h>
#include <Basecon.h>
#include <Time/Clock.h>
#include <Time/Timer.h>
#include <Drivers/Hpet.h>

VOID InitTimer(VOID) {
    INT TimerHz = 1000;

    BaseconPrintf(BASECON_TYPE_NORMAL, "time: initializing clock... ");
    InitSystemClock();
    BaseconPrintf(BASECON_TYPE_NORMAL, "done\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "time: using %s\n", HpetIsAvailable() ? "hpet" : "apic");
    BaseconPrintf(BASECON_TYPE_NORMAL, "time: initializing timer%s... ", HpetIsAvailable() ? " (may take a few minutes) " : "");
    TimerInit(TimerHz);
    BaseconPrintf(BASECON_TYPE_NORMAL, "done (%lu hz)\n", TimerFreq());
}
