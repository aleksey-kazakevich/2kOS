#pragma once

#include <Types.h>

// Выключение системы
VOID PowerShutdown(VOID);

// Перезагрузка системы
VOID PowerReboot(VOID);

// Проверить, выполняется ли завершение
BOOL PowerIsShuttingDown(VOID);