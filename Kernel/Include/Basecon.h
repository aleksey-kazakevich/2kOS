#pragma once

#include <Drivers/Framebuffer.h>
#include <Scheduler.h>
#include <SpinLock.h>
#include <Types.h>

#define BASECON_INPUT_BUFFER_SIZE 256
#define BASECON_HISTORY_SIZE 16
#define BASECON_SCROLLBACK_SIZE 1024
#define BASECON_SCROLLBACK_LINE_LEN 256
#define BASECON_KEY_QUEUE_SIZE 128
#define BASECON_OUTLINE_BUFFER_SIZE 1024

typedef struct {
    CHAR Buffer[BASECON_INPUT_BUFFER_SIZE];
    UINT32 Length;
    UINT32 CursorPos;
    UINT32 ViewStart;
} BaseconInputLine;

typedef struct {
    CHAR Lines[BASECON_HISTORY_SIZE][BASECON_INPUT_BUFFER_SIZE];
    UINT32 Count;
    UINT32 Current;
} BaseconHistory;

typedef struct {
    CHAR Lines[BASECON_SCROLLBACK_SIZE][BASECON_SCROLLBACK_LINE_LEN];
    FramebufferColor Colors[BASECON_SCROLLBACK_SIZE];
    UINT32 Count;
    UINT32 StartLine;
    UINT32 MaxDisplayLines;
} BaseconScrollback;

typedef struct {
    Framebuffer *FB;
    UINT32 Width;
    UINT32 Height;
    FramebufferColor Fg;
    FramebufferColor Bg;
    BOOL Initialized;

    BaseconInputLine Input;
    BaseconHistory History;
    BOOL InputMode;
    CHAR Prompt[64];

    UINT32 CursorX;
    UINT32 CursorY;
    UINT32 ScrollY;

    BaseconScrollback Scrollback;
    BOOL PromptFixed;
    BOOL WaitingForProgram;
    INT WaitingPid;

    // “Current” string storage (formerly STATIC in BaseconPutChar)
    CHAR OutLineBuffer[BASECON_OUTLINE_BUFFER_SIZE];
    UINT32 OutLineLen;

    // Protection against simultaneous access to Basecon
    SpinLock Lock;
} Basecon;

typedef enum {
    BASECON_TYPE_ERROR,
    BASECON_TYPE_INFO,
    BASECON_TYPE_SUCCESS,
    BASECON_TYPE_NORMAL,
    BASECON_TYPE_WARNING
} BaseconMsgType;

typedef struct {
    UINT8 ScanCode;
    CHAR Ascii;
    BOOL Pressed;
} BaseconKeyEvent;

typedef VOID (*BaseconInputCallback)(const CHAR *Input, VOID *UserData);

INT BaseconInit(Framebuffer *FB);
VOID BaseconClear(VOID);
VOID BaseconPutString(const CHAR *Str);
INT BaseconPrintf(BaseconMsgType Type, const CHAR *Fmt, ...);
VOID BaseconSetColors(FramebufferColor Fg, FramebufferColor Bg);
Basecon *BaseconGet(VOID);
VOID BaseconSetEnabled(BOOL Enabled);

VOID BaseconSetPrompt(const CHAR *Prompt);
VOID BaseconInputStart(VOID);
VOID BaseconInputEnd(VOID);
const CHAR* BaseconGetInput(VOID);
VOID BaseconProcessKey(UINT8 ScanCode, CHAR Ascii, BOOL Pressed);
BOOL BaseconIsInputMode(VOID);
VOID BaseconSetInputCallback(BaseconInputCallback Callback, VOID *UserData);

VOID BaseconRedrawFull(VOID);
VOID BaseconAddToScrollback(const CHAR *Str);
VOID BaseconCheckProgramStatus(VOID);
VOID BaseconSetPromptFixed(BOOL Enable);

VOID BaseconStartConsoleThread(VOID); 

BOOL BaseconIsShutdown(VOID);
VOID BaseconShutdown(BOOL ClearScreen);