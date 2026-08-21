#pragma once

#include <KDriver.h>
#include <Return.h>
#include <Types.h>

// ============================================================================
// CONSTANTS
// ============================================================================

#define PS2_KEY_QUEUE_SIZE 64
#define PS2_DRAIN_MAX_BYTES 256
#define PS2_MAX_SUBSCRIBERS 8

// Modifiers
#define MOD_SHIFT       0x01
#define MOD_CTRL        0x02
#define MOD_ALT         0x04
#define MOD_CAPSLOCK    0x08
#define MOD_NUMLOCK     0x10
#define MOD_SCROLLLOCK  0x20

// Key states
#define KEY_STATE_PRESSED   1
#define KEY_STATE_RELEASED  0

// Scan Code Set 1 (via hardware translation)
#define PS2_SET1_ESC            0x01
#define PS2_SET1_1              0x02
#define PS2_SET1_2              0x03
#define PS2_SET1_3              0x04
#define PS2_SET1_4              0x05
#define PS2_SET1_5              0x06
#define PS2_SET1_6              0x07
#define PS2_SET1_7              0x08
#define PS2_SET1_8              0x09
#define PS2_SET1_9              0x0A
#define PS2_SET1_0              0x0B
#define PS2_SET1_MINUS          0x0C
#define PS2_SET1_EQUAL          0x0D
#define PS2_SET1_BACKSPACE      0x0E
#define PS2_SET1_TAB            0x0F
#define PS2_SET1_Q              0x10
#define PS2_SET1_W              0x11
#define PS2_SET1_E              0x12
#define PS2_SET1_R              0x13
#define PS2_SET1_T              0x14
#define PS2_SET1_Y              0x15
#define PS2_SET1_U              0x16
#define PS2_SET1_I              0x17
#define PS2_SET1_O              0x18
#define PS2_SET1_P              0x19
#define PS2_SET1_LBRACKET       0x1A
#define PS2_SET1_RBRACKET       0x1B
#define PS2_SET1_ENTER          0x1C
#define PS2_SET1_LCTRL          0x1D
#define PS2_SET1_A              0x1E
#define PS2_SET1_S              0x1F
#define PS2_SET1_D              0x20
#define PS2_SET1_F              0x21
#define PS2_SET1_G              0x22
#define PS2_SET1_H              0x23
#define PS2_SET1_J              0x24
#define PS2_SET1_K              0x25
#define PS2_SET1_L              0x26
#define PS2_SET1_SEMICOLON      0x27
#define PS2_SET1_APOSTROPHE     0x28
#define PS2_SET1_GRAVE          0x29
#define PS2_SET1_LSHIFT         0x2A
#define PS2_SET1_BACKSLASH      0x2B
#define PS2_SET1_Z              0x2C
#define PS2_SET1_X              0x2D
#define PS2_SET1_C              0x2E
#define PS2_SET1_V              0x2F
#define PS2_SET1_B              0x30
#define PS2_SET1_N              0x31
#define PS2_SET1_M              0x32
#define PS2_SET1_COMMA          0x33
#define PS2_SET1_PERIOD         0x34
#define PS2_SET1_SLASH          0x35
#define PS2_SET1_RSHIFT         0x36
#define PS2_SET1_KP_ASTERISK    0x37
#define PS2_SET1_LALT           0x38
#define PS2_SET1_SPACE          0x39
#define PS2_SET1_CAPSLOCK       0x3A
#define PS2_SET1_F1             0x3B
#define PS2_SET1_F2             0x3C
#define PS2_SET1_F3             0x3D
#define PS2_SET1_F4             0x3E
#define PS2_SET1_F5             0x3F
#define PS2_SET1_F6             0x40
#define PS2_SET1_F7             0x41
#define PS2_SET1_F8             0x42
#define PS2_SET1_F9             0x43
#define PS2_SET1_F10            0x44
#define PS2_SET1_NUMLOCK        0x45
#define PS2_SET1_SCROLLLOCK     0x46
#define PS2_SET1_KP_7           0x47
#define PS2_SET1_KP_8           0x48
#define PS2_SET1_KP_9           0x49
#define PS2_SET1_KP_MINUS       0x4A
#define PS2_SET1_KP_4           0x4B
#define PS2_SET1_KP_5           0x4C
#define PS2_SET1_KP_6           0x4D
#define PS2_SET1_KP_PLUS        0x4E
#define PS2_SET1_KP_1           0x4F
#define PS2_SET1_KP_2           0x50
#define PS2_SET1_KP_3           0x51
#define PS2_SET1_KP_0           0x52
#define PS2_SET1_KP_PERIOD      0x53
#define PS2_SET1_F11            0x57
#define PS2_SET1_F12            0x58

// ============================================================================
// TYPES
// ============================================================================

typedef enum {
    KB_LAYOUT_US,
    KB_LAYOUT_RU,
    KB_LAYOUT_MAX
} KeyboardLayout;

typedef struct {
    UINT8 ScanCode;
    CHAR Ascii;
    UINT8 RawScanCode;
    BOOL Extended;
    UINT8 State;
    UINT8 Modifiers;
    UINT64 Timestamp;
} KeyEvent;

typedef VOID (*KeyboardCallback)(KeyEvent *Event, VOID *UserData);

// ============================================================================
// Functions
// ============================================================================

INT Ps2KeyboardInit(VOID);
BOOL Ps2KeyboardPollEvent(KeyEvent *Event);
BOOL Ps2KeyboardIsPressed(UINT8 ScanCode);
UINT8 Ps2KeyboardGetModifiers(VOID);
UINT8 Ps2KeyboardGetScanCodeSet(VOID);
INT Ps2KeyboardSetLayout(KeyboardLayout Layout);
KeyboardLayout Ps2KeyboardGetLayout(VOID);
INT Ps2KeyboardSubscribe(KeyboardCallback cb, VOID *ud);
INT Ps2KeyboardUnsubscribe(KeyboardCallback cb);
INT Ps2KeyboardDispatchEvents(VOID);

// Internal functions (for IRQ)
VOID Ps2HandleScanByte(UINT8 Byte);
VOID Ps2KeyboardHandleIRQ(VOID);
