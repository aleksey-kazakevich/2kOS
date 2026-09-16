#pragma once

#include <Cmd.h>

// Зарегистрировать команду unmount
INT CmdUnmountRegister(VOID);

// Обработчик команды unmount
INT CmdUnmountHandler(INT ArgC, const CHAR *ArgV[]);