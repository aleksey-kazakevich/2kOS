#pragma once

#include <Types.h>
#include <List.h>

// A callback function that is called every timer tick.
typedef VOID (*TimerCallback)(VOID *UserData);

// Subscriber structure
typedef struct TimerSubscriber {
    ListHead Node;              // For the list
    TimerCallback Callback;     // Handler function
    VOID *UserData;             // User data
    UINT32 Interval;            // Call every N ticks (1 = every tick)
    UINT32 Counter;             // Current counter (internal)
    BOOL Active;                // Is the subscriber active?
    CHAR Name[32];              // Debug name
} TimerSubscriber;

// Initialization of the subscriber system
VOID TimerSubscriberInit(VOID);

// Subscribe to the timer
// Returns: SUCCESS, NO_MEMORY, ALREADY_EXISTS
INT TimerSubscribe(TimerSubscriber *Sub);

// Unsubscribe from the timer
INT TimerUnsubscribe(TimerSubscriber *Sub);

// Timer handler (call from TimerHandler)
VOID TimerSubscriberTick(VOID);

// Get the number of active subscribers.
UINT32 TimerSubscriberGetCount(VOID);