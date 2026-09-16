#pragma once

#include <Cmd.h>

// Зарегистрировать команду power
INT CmdPowerRegister(VOID);

// Обработчик команды power
INT CmdPowerHandler(INT ArgC, const CHAR *ArgV[]);