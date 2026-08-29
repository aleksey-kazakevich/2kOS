#pragma once

#include <Types.h>
#include <List.h>

// Коллбэк, вызываемый каждый тик таймера
typedef VOID (*TimerCallback)(VOID *UserData);

// Структура подписчика
typedef struct TimerSubscriber {
    ListHead Node;              // Для списка
    TimerCallback Callback;     // Функция-обработчик
    VOID *UserData;             // Пользовательские данные
    UINT32 Interval;            // Вызывать каждые N тиков (1 = каждый тик)
    UINT32 Counter;             // Текущий счетчик (внутренний)
    BOOL Active;                // Активен ли подписчик
    CHAR Name[32];              // Имя для отладки
} TimerSubscriber;

// Инициализация системы подписчиков
VOID TimerSubscriberInit(VOID);

// Подписаться на таймер
// Возвращает: SUCCESS, NO_MEMORY, ALREADY_EXISTS
INT TimerSubscribe(TimerSubscriber *Sub);

// Отписаться от таймера
INT TimerUnsubscribe(TimerSubscriber *Sub);

// Обработчик таймера (вызывать из TimerHandler)
VOID TimerSubscriberTick(VOID);

// Получить количество активных подписчиков
UINT32 TimerSubscriberGetCount(VOID);