#pragma once

#include <Cmd.h>

// Зарегистрировать команду diskmgr
INT CmdDiskMgrRegister(VOID);

// Обработчик команды diskmgr
INT CmdDiskMgrHandler(INT ArgC, const CHAR *ArgV[]);