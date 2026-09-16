#include <Drivers/Acpi/Acpi.h>
#include <Drivers/Power.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Asm/Cpu.h>

STATIC BOOL GShuttingDown = FALSE;
STATIC BOOL IsReboot = FALSE;

STATIC VOID PowerKillAllTasks(VOID) {
    if (IsReboot) BaseconPrintf(BASECON_TYPE_NORMAL, "power: preparing to reboot...\n");
    else BaseconPrintf(BASECON_TYPE_NORMAL, "power: preparing to shutdown...\n");
    
    // Делаем несколько попыток, чтобы убить все процессы
    for (INT Attempt = 0; Attempt < 5; Attempt++) {
        TaskInfo Info[32];
        INT Count = TaskList(Info, 32);
        if (Count <= 0) break;
        
        BOOL AnyKilled = FALSE;
        for (INT I = 0; I < Count; I++) {
            // Не убиваем INIT (PID 0)
            if (Info[I].Pid == 0) continue;
            if (Info[I].State == TASK_ZOMBIE) continue;
            
            SchedulerKill(Info[I].Pid);
            AnyKilled = TRUE;
        }
        
        // Если никого не убили — выходим
        if (!AnyKilled) break;
        
        // Даём время на завершение
        TimerMdelay(10);
    }
    
    // Финальная проверка: если остались живые процессы — убиваем их принудительно
    TaskInfo Info[32];
    INT Count = TaskList(Info, 32);
    for (INT I = 0; I < Count; I++) {
        if (Info[I].Pid != 0 && Info[I].State != TASK_ZOMBIE) {
            BaseconPrintf(BASECON_TYPE_WARNING, 
                          "power: %s (PID %d) is not killed, trying force...\n", 
                          Info[I].Name, Info[I].Pid);
            SchedulerKill(Info[I].Pid);
        }
    }
    
    TimerMdelay(50);
}

VOID PowerShutdown(VOID) {
    if (GShuttingDown) return;
    GShuttingDown = TRUE;
    IsReboot = FALSE;    

    LocalInterruptsDisable();
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "power: shutting down...\n");
    
    // Убиваем все задачи
    PowerKillAllTasks();
    
    // Небольшая задержка для завершения
    TimerMdelay(50);
    
    LocalInterruptsEnable();
    
    // Выключаемся
    AcpiShutdown();
    
    // Если AcpiShutdown не сработал — вешаемся
    while (1) {
        Halt();
    }
}

VOID PowerReboot(VOID) {
    if (GShuttingDown) return;
    GShuttingDown = TRUE;
    IsReboot = TRUE;
    
    LocalInterruptsDisable();
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "power: rebooting...\n");
    
    // Убиваем все задачи
    PowerKillAllTasks();
    
    // Небольшая задержка для завершения
    TimerMdelay(50);
    
    LocalInterruptsEnable();
    
    // Перезагружаемся
    AcpiReboot();
    
    // Если AcpiReboot не сработал — вешаемся
    while (1) {
        Halt();
    }
}

BOOL PowerIsShuttingDown(VOID) {
    return GShuttingDown;
}