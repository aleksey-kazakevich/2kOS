#pragma once

#include <Types.h>
#include <Cmd.h>

// Зарегистрировать команду echo
INT CmdEchoRegister(VOID);

// Обработчик команды echo (можно вызывать напрямую)
INT CmdEchoHandler(INT ArgC, const CHAR *ArgV[]);