#pragma once

#include <Cmd.h>

// Зарегистрировать команду mount
INT CmdMountRegister(VOID);

// Обработчик команды mount
INT CmdMountHandler(INT ArgC, const CHAR *ArgV[]);