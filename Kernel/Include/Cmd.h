#pragma once

#include <Types.h>
#include <List.h>

typedef INT (*CmdHandler)(INT ArgC, const CHAR *ArgV[]);

// Структура встроенной команды
typedef struct CmdCommand {
    ListHead Node;              // Для связного списка
    const CHAR *Name;           // Имя команды (например, "help")
    const CHAR *Description;    // Краткое описание (для help)
    CmdHandler Handler;         // Функция-обработчик
    BOOL Builtin;              // TRUE = встроенная, FALSE = внешняя (для будущего)
} CmdCommand;

// Инициализация системы команд (вызывать при старте)
VOID CmdInit(VOID);

// Зарегистрировать встроенную команду
INT CmdRegisterBuiltin(const CHAR *Name, const CHAR *Description, CmdHandler Handler);

// Удалить встроенную команду
INT CmdUnregisterBuiltin(const CHAR *Name);

// Найти команду по имени
CmdCommand* CmdFind(const CHAR *Name);

// Выполнить команду по строке
// Возвращает: код завершения команды или -1 если команда не найдена
INT CmdExecute(const CHAR *CmdLine);

// Получить список всех команд (для help)
CmdCommand* CmdGetFirst(VOID);
CmdCommand* CmdGetNext(CmdCommand *Cmd);

// Количество зарегистрированных команд
UINT32 CmdGetCount(VOID);

// Вывести список команд в Basecon
VOID CmdPrintList(VOID);

// Обработчик ввода из Basecon
VOID CmdBaseconHandler(const CHAR *Input, VOID *UserData);