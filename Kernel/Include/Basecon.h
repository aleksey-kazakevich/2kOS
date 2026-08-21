#pragma once

#include <Types.h>
#include <Drivers/Framebuffer.h>

// Input buffer
#define BASECON_INPUT_BUFFER_SIZE 256
#define BASECON_HISTORY_SIZE 16
#define BASECON_SCROLLBACK_SIZE 1024
#define BASECON_SCROLLBACK_LINE_LEN 256

// Structure for the input line
typedef struct {
    CHAR Buffer[BASECON_INPUT_BUFFER_SIZE];
    UINT32 Length;
    UINT32 CursorPos;
    UINT32 ViewStart;
} BaseconInputLine;

// Structure for command history
typedef struct {
    CHAR Lines[BASECON_HISTORY_SIZE][BASECON_INPUT_BUFFER_SIZE];
    UINT32 Count;
    UINT32 Current;
} BaseconHistory;

// Scrollback structure (entire output history)
typedef struct {
    CHAR Lines[BASECON_SCROLLBACK_SIZE][BASECON_SCROLLBACK_LINE_LEN];
    UINT32 Count;
    UINT32 StartLine;
    UINT32 MaxDisplayLines;
} BaseconScrollback;

// Terminal structure
typedef struct {
    Framebuffer *FB;
    UINT32 Width;
    UINT32 Height;
    FramebufferColor Fg;
    FramebufferColor Bg;
    BOOL Initialized;
    
    // Enter
    BaseconInputLine Input;
    BaseconHistory History;
    BOOL InputMode;
    CHAR Prompt[64];
    
    // Old mode (cursor)
    UINT32 CursorX;
    UINT32 CursorY;
    UINT32 ScrollY;
    
    // New fields for the "prompt is always below" mode
    BaseconScrollback Scrollback;
    BOOL PromptFixed;
    BOOL WaitingForProgram;
    INT WaitingPid;
} Basecon;

typedef enum {
    BASECON_TYPE_ERROR,
    BASECON_TYPE_INFO,
    BASECON_TYPE_SUCCESS,
    BASECON_TYPE_NORMAL
} BaseconMsgType;

// Callback for input
typedef VOID (*BaseconInputCallback)(const CHAR *Input, VOID *UserData);

// ============================================================================
// EXISTING FUNCTIONS
// ============================================================================

INT BaseconInit(Framebuffer *FB);
VOID BaseconClear(VOID);
VOID BaseconPutChar(CHAR C);
VOID BaseconPutString(const CHAR *Str);
INT BaseconPrintf(BaseconMsgType Type, const CHAR *Fmt, ...);
VOID BaseconSetColors(FramebufferColor Fg, FramebufferColor Bg);
Basecon *BaseconGet(VOID);
VOID BaseconSetEnabled(BOOL Enabled);

// ============================================================================
// INPUT FUNCTIONS
// ============================================================================

VOID BaseconSetPrompt(const CHAR *Prompt);
VOID BaseconInputStart(VOID);
VOID BaseconInputEnd(VOID);
const CHAR* BaseconGetInput(VOID);
VOID BaseconProcessKey(UINT8 ScanCode, CHAR Ascii, BOOL Pressed);
BOOL BaseconIsInputMode(VOID);
VOID BaseconSetInputCallback(BaseconInputCallback Callback, VOID *UserData);

// ============================================================================
// NEW FEATURES
// ============================================================================

VOID BaseconRedrawFull(VOID);
VOID BaseconAddToScrollback(const CHAR *Str);
VOID BaseconCheckProgramStatus(VOID);
VOID BaseconSetPromptFixed(BOOL Enable);
