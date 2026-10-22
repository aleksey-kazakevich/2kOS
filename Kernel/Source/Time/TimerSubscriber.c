#include <Time/TimerSubscriber.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <Return.h>
#include <SpinLock.h>

// Global subscriber list
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
    
    // Check whether it is already subscribed
    ListHead *Pos;
    ListForEach(Pos, &GSubscriberList) {
        TimerSubscriber *Existing = ListEntry(Pos, TimerSubscriber, Node);
        if (Existing == Sub) {
            SpinLockRelease(&GSubscriberLock);
            RETURN(ALREADY_EXISTS);
        }
    }
    
    // Initialize the subscriber
    Sub->Counter = 0;
    Sub->Active = TRUE;
    if (Sub->Interval == 0) {
        Sub->Interval = 1;  // By default every tick
    }
    
    // Add to the list
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

    // Собираем колбэки в локальный массив, НЕ вызывая их под локом
    #define TSR_MAX 64
    TimerCallback Cbs[TSR_MAX];
    VOID *Uds[TSR_MAX];
    UINT32 CbCount = 0;

    UINTPTR Flags = SpinLockAcquireIrq(&GSubscriberLock);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GSubscriberList) {
        TimerSubscriber *Sub = ListEntry(Pos, TimerSubscriber, Node);

        if (!Sub->Active) {
            ListDel(&Sub->Node);
            GSubscriberCount--;
            continue;
        }

        Sub->Counter++;
        if (Sub->Counter >= Sub->Interval) {
            Sub->Counter = 0;
            if (CbCount < TSR_MAX) {
                Cbs[CbCount] = Sub->Callback;
                Uds[CbCount] = Sub->UserData;
                CbCount++;
            }
        }
    }

    SpinLockReleaseIrq(&GSubscriberLock, Flags);

    // Вне лока, вне ISR-контекста по локам
    for (UINT32 I = 0; I < CbCount; I++) {
        if (Cbs[I]) Cbs[I](Uds[I]);
    }
}

UINT32 TimerSubscriberGetCount(VOID) {
    return GSubscriberCount;
}