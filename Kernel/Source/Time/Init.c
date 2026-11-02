#include <Drivers/Hpet.h>
#include <Time/Clock.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Types.h>

VOID InitTimer(VOID) {
    INT TimerHz = 1000;

    StdOutPrintf("time: initializing clock... ");
    InitSystemClock();
    StdOutPrintf("done\n");
    StdOutPrintf("time: using %s\n", HpetIsAvailable() ? "hpet" : "apic");
    StdOutPrintf("time: initializing timer%s... ", HpetIsAvailable() ? " (may take a few minutes) " : "");
    TimerInit(TimerHz);
    StdOutPrintf("done (%lu hz)\n", TimerFreq());
}
