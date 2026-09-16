#pragma once

#include <Cmd.h>

// Зарегистрировать команду format
INT CmdFormatRegister(VOID);

// Обработчик команды format
INT CmdFormatHandler(INT ArgC, const CHAR *ArgV[]);