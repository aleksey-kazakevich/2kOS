#include <Time/TimerSubscriber.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <Return.h>
#include <SpinLock.h>

// Глобальный список подписчиков
STATIC ListHead GSubscriberList;
STATIC SpinLock GSubscriberLock;
STATIC UINT32 GSubscriberCount = 0;
STATIC BOOL GInitialized = FALSE;

VOID TimerSubscriberInit(VOID) {
    if (GInitialized) return;
    
    ListInit(&GSubscriberList);
    SpinLockInit(&GSubscriberLock);
    GSubscriberCount = 0;
    GInitialized = TRUE;
}

INT TimerSubscribe(TimerSubscriber *Sub) {
    if (!Sub || !Sub->Callback) RETURN(NO_OBJECT);
    if (!GInitialized) RETURN(NO_OBJECT);
    
    SpinLockAcquire(&GSubscriberLock);
    
    // Проверяем, не подписан ли уже
    ListHead *Pos;
    ListForEach(Pos, &GSubscriberList) {
        TimerSubscriber *Existing = ListEntry(Pos, TimerSubscriber, Node);
        if (Existing == Sub) {
            SpinLockRelease(&GSubscriberLock);
            RETURN(ALREADY_EXISTS);
        }
    }
    
    // Инициализируем подписчика
    Sub->Counter = 0;
    Sub->Active = TRUE;
    if (Sub->Interval == 0) {
        Sub->Interval = 1;  // По умолчанию каждый тик
    }
    
    // Добавляем в список
    ListAddTail(&GSubscriberList, &Sub->Node);
    GSubscriberCount++;
    
    SpinLockRelease(&GSubscriberLock);
    
    RETURN(SUCCESS);
}

INT TimerUnsubscribe(TimerSubscriber *Sub) {
    if (!Sub) RETURN(NO_OBJECT);
    if (!GInitialized) RETURN(NO_OBJECT);
    
    SpinLockAcquire(&GSubscriberLock);
    
    BOOL Found = FALSE;
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GSubscriberList) {
        TimerSubscriber *Existing = ListEntry(Pos, TimerSubscriber, Node);
        if (Existing == Sub) {
            ListDel(&Sub->Node);
            Sub->Active = FALSE;
            GSubscriberCount--;
            Found = TRUE;
            break;
        }
    }
    
    SpinLockRelease(&GSubscriberLock);
    
    if (Found) {
        RETURN(SUCCESS);
    }
    
    RETURN(NOT_FOUND);
}

VOID TimerSubscriberTick(VOID) {
    if (!GInitialized) return;
    if (ListEmpty(&GSubscriberList)) return;
    
    SpinLockAcquire(&GSubscriberLock);
    
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GSubscriberList) {
        TimerSubscriber *Sub = ListEntry(Pos, TimerSubscriber, Node);
        
        if (!Sub->Active) {
            // Удаляем неактивных
            ListDel(&Sub->Node);
            GSubscriberCount--;
            continue;
        }
        
        // Увеличиваем счетчик
        Sub->Counter++;
        
        // Проверяем, наступил ли интервал
        if (Sub->Counter >= Sub->Interval) {
            Sub->Counter = 0;
            
            // Вызываем коллбэк (без блокировки, чтобы не было дедлоков)
            SpinLockRelease(&GSubscriberLock);
            
            if (Sub->Callback) {
                Sub->Callback(Sub->UserData);
            }
            
            SpinLockAcquire(&GSubscriberLock);
        }
    }
    
    SpinLockRelease(&GSubscriberLock);
}

UINT32 TimerSubscriberGetCount(VOID) {
    return GSubscriberCount;
}