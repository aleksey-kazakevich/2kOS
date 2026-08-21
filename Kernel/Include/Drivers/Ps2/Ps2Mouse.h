#pragma once

#include <Types.h>
#include <Drivers/Ps2/Ps2.h>
#include <KDriver.h>

// ============================================================================
// Mouse types
// ============================================================================

typedef enum {
    PS2_MOUSE_STANDARD     = 0x00,
    PS2_MOUSE_INTELLIMOUSE = 0x03,
    PS2_MOUSE_EXPLORER     = 0x04,
    PS2_MOUSE_5BUTTON      = 0x04,  // compatibility
    PS2_MOUSE_INTELLIMOUSE_EXP = 0x05
} Ps2MouseType;

// ============================================================================
// Mouse states
// ============================================================================

typedef enum {
    PS2_MOUSE_STATE_IGNORE,
    PS2_MOUSE_STATE_INITIALIZING,
    PS2_MOUSE_STATE_RESYNCING,
    PS2_MOUSE_STATE_CMD_MODE,
    PS2_MOUSE_STATE_ACTIVATED
} Ps2MouseStateEnum;

// ============================================================================
// Button states and position (for users)
// ============================================================================

typedef struct {
    INT32 X;
    INT32 Y;
    INT32 Z;
    BOOL LeftButton;
    BOOL RightButton;
    BOOL MiddleButton;
    BOOL Button4;
    BOOL Button5;
} MouseState;

// ============================================================================
// Mouse event
// ============================================================================

typedef struct {
    INT32 DeltaX;
    INT32 DeltaY;
    INT32 DeltaZ;
    BOOL LeftButton;
    BOOL RightButton;
    BOOL MiddleButton;
    BOOL Button4;
    BOOL Button5;
    UINT8 MouseType;
} MouseEvent;

// ============================================================================
// Callback для мыши
// ============================================================================

typedef VOID (*MouseCallback)(MouseEvent *Event, VOID *UserData);

typedef struct {
    MouseCallback Callback;
    VOID *UserData;
    BOOL Active;
} Ps2MouseSubscriber;

// ============================================================================
// Driver internal state
// ============================================================================

typedef struct {
    BOOL Initialized;
    BOOL Enabled;
    Ps2MouseStateEnum State;
    Ps2MouseType Type;
    UINT8 PacketSize;
    UINT8 PacketIndex;
    UINT8 Packet[8];
    UINT8 BadByte;
    UINT32 OutOfSyncCount;
    UINT32 ErrorCount;
    UINT32 EventCount;
    MouseState MouseState;    // <-- renamed so as not to conflict with Ps2MouseStateEnum State
    UINT8 SampleRate;
    UINT8 Resolution;
    UINT32 Gsi;
    KDriver *Driver;
} Ps2MouseState;

// ============================================================================
// Public functions
// ============================================================================

INT Ps2MouseInit(VOID);
INT Ps2MouseSetSampleRate(UINT8 Rate);
INT Ps2MouseSetResolution(UINT8 Resolution);
INT Ps2MouseGetType(VOID);
MouseState* Ps2MouseGetState(VOID);
BOOL Ps2MouseIsPresent(VOID);

INT Ps2MouseSubscribe(MouseCallback Cb, VOID *Ud);
INT Ps2MouseUnsubscribe(MouseCallback Cb);
INT Ps2MouseDispatchEvents(VOID);

VOID Ps2MouseHandleIRQ(VOID);
