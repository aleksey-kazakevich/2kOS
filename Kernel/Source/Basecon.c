#include <Basecon.h>
#include <Lib/String.h>
#include <Lib/StdArg.h>
#include <Lib/StdIo.h>
#include <Mem/Allocator.h>
#include <Drivers/Ps2/Ps2Keyboard.h>
#include <Return.h>
#include <Rgb.h>
#include <FontAccess.h>
#include <Scheduler.h>

static BOOL BaseconEnabled = TRUE;
static Basecon GBasecon = {0};

static BaseconInputCallback GInputCallback = NULLPTR;
static VOID *GInputCallbackUserData = NULLPTR;
static BOOL GBaseconSubscribed = FALSE;

// ============================================================================
// PROTOTYPES OF INTERNAL FUNCTIONS
// ============================================================================

static VOID KeyboardHandler(KeyEvent *Event, VOID *UserData);

// ============================================================================
// NEW FEATURE: ADDING A ROW TO THE SCROLLBACK
// ============================================================================

VOID BaseconAddToScrollback(const CHAR *Str) {
    if (!Str || !Str[0]) return;
    if (!GBasecon.Initialized) return;
    if (!GBasecon.PromptFixed) return;
    
    CHAR Buffer[1024];
    CHAR *SavePtr = NULLPTR;
    
    StrnCpy(Buffer, Str, sizeof(Buffer) - 1);
    Buffer[sizeof(Buffer) - 1] = '\0';
    
    CHAR *Line = StrTokR(Buffer, "\n", &SavePtr);
    while (Line) {
        if (GBasecon.Scrollback.Count < BASECON_SCROLLBACK_SIZE) {
            StrCpy(GBasecon.Scrollback.Lines[GBasecon.Scrollback.Count], Line);
            GBasecon.Scrollback.Count++;
        } else {
            for (UINT32 I = 0; I < BASECON_SCROLLBACK_SIZE - 1; I++) {
                StrCpy(GBasecon.Scrollback.Lines[I], 
                       GBasecon.Scrollback.Lines[I + 1]);
            }
            StrCpy(GBasecon.Scrollback.Lines[BASECON_SCROLLBACK_SIZE - 1], Line);
        }
        Line = StrTokR(NULLPTR, "\n", &SavePtr);
    }
    
    BaseconRedrawFull();
}

VOID BaseconRedrawFull(VOID) {
    if (!GBasecon.Initialized || !BaseconEnabled) return;
    if (!GBasecon.PromptFixed) return;
    
    FramebufferClear(GBasecon.Bg);
    
    UINT32 MaxDisplayLines = GBasecon.Height - 1;
    UINT32 StartLine = 0;
    
    if (GBasecon.Scrollback.Count > MaxDisplayLines) {
        StartLine = GBasecon.Scrollback.Count - MaxDisplayLines;
    }
    
    // Drawing history
    for (UINT32 I = 0; I < MaxDisplayLines; I++) {
        UINT32 SrcIdx = StartLine + I;
        if (SrcIdx >= GBasecon.Scrollback.Count) break;
        
        UINT32 Y = I * FONT_LINE_HEIGHT;
        FramebufferDrawString(GBasecon.Scrollback.Lines[SrcIdx], 
                              0, Y, GBasecon.Fg, GBasecon.Bg);
    }
    
    // Draw a prompt with input on the last line
    UINT32 PromptY = (GBasecon.Height - 1) * FONT_LINE_HEIGHT;
    
    // Clearing the prompt line
    FramebufferFillRect(0, PromptY, GBasecon.FB->Width, FONT_LINE_HEIGHT, GBasecon.Bg);
    
    // We form the line: prompt + entered text
    CHAR FullLine[BASECON_INPUT_BUFFER_SIZE + 64];
    StrCpy(FullLine, GBasecon.Prompt);
    
    if (GBasecon.InputMode) {
        StrCat(FullLine, GBasecon.Input.Buffer);
    }
    
    // Drawing a line
    FramebufferDrawString(FullLine, 0, PromptY, GBasecon.Fg, GBasecon.Bg);
    
    // Draw a cursor (only if in input mode)
    if (GBasecon.InputMode) {
        UINT32 CursorX = (StrLen(GBasecon.Prompt) + GBasecon.Input.CursorPos) * FONT_WIDTH;
        if (CursorX < GBasecon.FB->Width) {
            FramebufferFillRect(CursorX, PromptY + FONT_LINE_HEIGHT - 2, 
                               FONT_WIDTH, 2, GBasecon.Fg);
        }
    }
    
    FramebufferSwapBuffers();
}

// ============================================================================
// NEW FEATURE: PROGRAM COMPLETION CHECK
// ============================================================================

VOID BaseconCheckProgramStatus(VOID) {
    if (!GBasecon.WaitingForProgram) return;
    
    if (!TaskIsAlive(GBasecon.WaitingPid)) {
        GBasecon.WaitingForProgram = FALSE;
        GBasecon.WaitingPid = -1;
        BaseconInputStart();
    }
}

// ============================================================================
// NEW FEATURE: ENABLE MODE
// ============================================================================

VOID BaseconSetPromptFixed(BOOL Enable) {
    if (!GBasecon.Initialized) return;
    GBasecon.PromptFixed = Enable;
    
    if (Enable) {
        GBasecon.Scrollback.Count = 0;
        GBasecon.Scrollback.StartLine = 0;
        GBasecon.Scrollback.MaxDisplayLines = GBasecon.Height - 1;
        GBasecon.WaitingForProgram = FALSE;
        GBasecon.WaitingPid = -1;
        
        BaseconClear();
        BaseconRedrawFull();
    }
}

// ============================================================================
// BASIC FUNCTIONS
// ============================================================================

VOID BaseconPutChar(CHAR C) {
    if (!GBasecon.Initialized) return;
    if (!BaseconEnabled) return;
    
    if (GBasecon.PromptFixed) {
        static CHAR LineBuffer[1024];
        static UINT32 LineLen = 0;
        
        if (C == '\n') {
            // We complete the current line and add it to the scrollback
            if (LineLen > 0) {
                LineBuffer[LineLen] = '\0';
                BaseconAddToScrollback(LineBuffer);
                LineLen = 0;
            }
            // DO NOT ADD AN EMPTY LINE!
            return;
        }
        
        if (C == '\r') {
            // End the line with \r (carriage return)
            if (LineLen > 0) {
                LineBuffer[LineLen] = '\0';
                BaseconAddToScrollback(LineBuffer);
                LineLen = 0;
            }
            return;
        }
        
        if (C == '\b') {
            if (LineLen > 0) LineLen--;
            return;
        }
        
        // Printable characters
        if ((C >= 0x20 && C <= 0x7E) || (C >= 0x80 && C <= 0xBF)) {
            if (LineLen < sizeof(LineBuffer) - 1) {
                LineBuffer[LineLen++] = C;
            }
        }
        return;
    }
    
    // Old mode - draw directly
    if (C == '\n') {
        GBasecon.CursorX = 0;
        GBasecon.CursorY++;
        if (GBasecon.CursorY >= GBasecon.Height) {
            GBasecon.CursorY = GBasecon.Height - 1;
        }
    } else if (C >= 0x20 || C >= 0x80) {
        UINT32 PixelX = GBasecon.CursorX * FONT_WIDTH;
        UINT32 PixelY = GBasecon.CursorY * FONT_LINE_HEIGHT;
        FramebufferDrawChar(C, PixelX, PixelY, GBasecon.Fg, GBasecon.Bg);
        GBasecon.CursorX++;
        if (GBasecon.CursorX >= GBasecon.Width) {
            GBasecon.CursorX = 0;
            GBasecon.CursorY++;
        }
    }
    FramebufferSwapBuffers();
}

VOID BaseconPutString(const CHAR *Str) {
    if (!GBasecon.Initialized || !Str) return;
    if (!BaseconEnabled) return;
    
    if (GBasecon.PromptFixed) {
        while (*Str) {
            BaseconPutChar(*Str);
            Str++;
        }
        return;
    }
    
    while (*Str) {
        BaseconPutChar(*Str);
        Str++;
    }
}

INT BaseconPrintf(BaseconMsgType Type, const CHAR *Fmt, ...) {
    if (!GBasecon.Initialized || !Fmt) RETURN(INCORRECT_VALUE);
    if (!BaseconEnabled) RETURN(OPERATION_DISABLED);
    
    FramebufferColor OldFg = GBasecon.Fg;
    FramebufferColor OldBg = GBasecon.Bg;
    
    const CHAR *Prefix = NULLPTR;
    FramebufferColor PrefixColor = OldFg;
    BOOL HasPrefix = FALSE;
    
    switch (Type) {
        case BASECON_TYPE_ERROR:
            Prefix = "error: ";
            PrefixColor = RGB_RED;
            HasPrefix = TRUE;
            break;
        case BASECON_TYPE_INFO:
            Prefix = "info: ";
            PrefixColor = RGB_BLUE;
            HasPrefix = TRUE;
            break;
        case BASECON_TYPE_SUCCESS:
            Prefix = "ok: ";
            PrefixColor = RGB_GREEN;
            HasPrefix = TRUE;
            break;
        default:
            break;
    }
    
    if (HasPrefix) {
        BaseconSetColors(PrefixColor, OldBg);
        BaseconPutString(Prefix);
        BaseconSetColors(OldFg, OldBg);
    }
    
    CHAR Buffer[1024];
    VA_LIST Args;
    
    VaStart(Args, Fmt);
    INT Result = VsnPrintf(Buffer, sizeof(Buffer), Fmt, Args);
    VaEnd(Args);
    
    if (Result > 0) {
        BaseconPutString(Buffer);
    }
    
    RETURN(Result);
}

VOID BaseconSetColors(FramebufferColor Fg, FramebufferColor Bg) {
    if (!GBasecon.Initialized) return;
    GBasecon.Fg = Fg;
    GBasecon.Bg = Bg;
}

Basecon *BaseconGet(VOID) {
    if (!BaseconEnabled) return NULLPTR;
    return GBasecon.Initialized ? &GBasecon : NULLPTR;
}

VOID BaseconSetEnabled(BOOL Enabled) {
    if (BaseconEnabled == Enabled) return;
    BaseconEnabled = Enabled;
}

VOID BaseconClear(VOID) {
    if (!GBasecon.Initialized) return;
    if (!BaseconEnabled) return;
    
    if (GBasecon.PromptFixed) {
        GBasecon.Scrollback.Count = 0;
        GBasecon.Scrollback.StartLine = 0;
        BaseconRedrawFull();
    } else {
        GBasecon.CursorX = 0;
        GBasecon.CursorY = 0;
        FramebufferClear(GBasecon.Bg);
    }
}

// ============================================================================
// INPUT FUNCTIONS
// ============================================================================

VOID BaseconSetPrompt(const CHAR *Prompt) {
    if (!GBasecon.Initialized) return;
    if (Prompt) {
        StrnCpy(GBasecon.Prompt, Prompt, sizeof(GBasecon.Prompt) - 1);
        GBasecon.Prompt[sizeof(GBasecon.Prompt) - 1] = '\0';
    } else {
        GBasecon.Prompt[0] = '\0';
    }
}

VOID BaseconInputStart(VOID) {
    if (!GBasecon.Initialized) return;
    if (!BaseconEnabled) return;
    
    if (GBasecon.WaitingForProgram) return;
    
    if (!GBaseconSubscribed) {
        if (Ps2KeyboardSubscribe(KeyboardHandler, NULLPTR) == SUCCESS) {
            GBaseconSubscribed = TRUE;
        }
    }
    
    GBasecon.InputMode = TRUE;
    GBasecon.Input.Length = 0;
    GBasecon.Input.CursorPos = 0;
    GBasecon.Input.ViewStart = 0;
    GBasecon.Input.Buffer[0] = '\0';
    
    if (GBasecon.PromptFixed) {
        BaseconRedrawFull();
    } else {
        if (GBasecon.Prompt[0]) {
            BaseconPutString(GBasecon.Prompt);
        }
    }
}

VOID BaseconInputEnd(VOID) {
    if (!GBasecon.Initialized) return;
    GBasecon.InputMode = FALSE;
}

const CHAR* BaseconGetInput(VOID) {
    if (!GBasecon.Initialized) return NULLPTR;
    return GBasecon.Input.Buffer;
}

BOOL BaseconIsInputMode(VOID) {
    return GBasecon.InputMode;
}

VOID BaseconSetInputCallback(BaseconInputCallback Callback, VOID *UserData) {
    GInputCallback = Callback;
    GInputCallbackUserData = UserData;
}

// ============================================================================
// KEYBOARD HANDLER
// ============================================================================

static VOID KeyboardHandler(KeyEvent *Event, VOID *UserData) {
    if (!Event) return;
    BaseconProcessKey(Event->RawScanCode, Event->Ascii, 
                      Event->State == KEY_STATE_PRESSED);
}

VOID BaseconProcessKey(UINT8 ScanCode, CHAR Ascii, BOOL Pressed) {
    if (!GBasecon.Initialized || !BaseconEnabled) return;
    if (!Pressed) return;

    UINT8 CharCode = (UINT8)Ascii;

    // If not in input mode, just output the symbol
    if (!GBasecon.InputMode) {
        if ((CharCode >= 0x20 && CharCode <= 0x7E) || 
            (CharCode >= 0x80 && CharCode <= 0xBF)) {
            BaseconPutChar(Ascii);
        }
        return;
    }
    
    CHAR *Buffer = GBasecon.Input.Buffer;
    UINT32 *Len = &GBasecon.Input.Length;
    UINT32 *CursorPos = &GBasecon.Input.CursorPos;
    
    switch (ScanCode) {
        case 0x1C: { // Enter
            if (GBasecon.PromptFixed && *Len > 0) {
                CHAR FullLine[BASECON_INPUT_BUFFER_SIZE + 64];
                StrCpy(FullLine, GBasecon.Prompt);
                StrCat(FullLine, Buffer);
                BaseconAddToScrollback(FullLine);
            }
            
            if (*Len > 0) {
                if (GBasecon.History.Count < BASECON_HISTORY_SIZE) {
                    StrCpy(GBasecon.History.Lines[GBasecon.History.Count], Buffer);
                    GBasecon.History.Count++;
                } else {
                    for (UINT32 I = 0; I < BASECON_HISTORY_SIZE - 1; I++) {
                        StrCpy(GBasecon.History.Lines[I], GBasecon.History.Lines[I + 1]);
                    }
                    StrCpy(GBasecon.History.Lines[BASECON_HISTORY_SIZE - 1], Buffer);
                }
                GBasecon.History.Current = GBasecon.History.Count;
            }
            
            if (GInputCallback) {
                GInputCallback(Buffer, GInputCallbackUserData);
            }
            
            *Len = 0;
            *CursorPos = 0;
            GBasecon.Input.ViewStart = 0;
            Buffer[0] = '\0';
            GBasecon.InputMode = FALSE;
            
            if (!GBasecon.WaitingForProgram) {
                BaseconInputStart();
            }
            return;
        }
        
        case 0x0E: { // Backspace
            if (*CursorPos > 0) {
                for (UINT32 I = *CursorPos - 1; I < *Len - 1; I++) {
                    Buffer[I] = Buffer[I + 1];
                }
                (*Len)--;
                (*CursorPos)--;
                
                if (*CursorPos < GBasecon.Input.ViewStart) {
                    GBasecon.Input.ViewStart = *CursorPos;
                }
                
                Buffer[*Len] = '\0';
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        case 0x53: { // Delete (extended)
            if (*CursorPos < *Len) {
                for (UINT32 I = *CursorPos; I < *Len - 1; I++) {
                    Buffer[I] = Buffer[I + 1];
                }
                (*Len)--;
                Buffer[*Len] = '\0';
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        case 0x4B: { // Left Arrow (extended)
            if (*CursorPos > 0) {
                (*CursorPos)--;
                if (*CursorPos < GBasecon.Input.ViewStart) {
                    GBasecon.Input.ViewStart = *CursorPos;
                }
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        case 0x4D: { // Right Arrow (extended)
            if (*CursorPos < *Len) {
                (*CursorPos)++;
                if (*CursorPos - GBasecon.Input.ViewStart >= GBasecon.Width) {
                    GBasecon.Input.ViewStart = *CursorPos - GBasecon.Width + 1;
                }
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        case 0x47: { // Home (extended)
            *CursorPos = 0;
            GBasecon.Input.ViewStart = 0;
            if (GBasecon.PromptFixed) {
                BaseconRedrawFull();
            }
            break;
        }
        
        case 0x4F: { // End (extended)
            *CursorPos = *Len;
            if (*CursorPos > GBasecon.Width) {
                GBasecon.Input.ViewStart = *CursorPos - GBasecon.Width + 1;
            }
            if (GBasecon.PromptFixed) {
                BaseconRedrawFull();
            }
            break;
        }
        
        case 0x48: { // Up Arrow (history)
            if (GBasecon.History.Current > 0) {
                GBasecon.History.Current--;
                StrCpy(Buffer, GBasecon.History.Lines[GBasecon.History.Current]);
                *Len = StrLen(Buffer);
                *CursorPos = *Len;
                if (*Len > GBasecon.Width) {
                    GBasecon.Input.ViewStart = *Len - GBasecon.Width + 1;
                } else {
                    GBasecon.Input.ViewStart = 0;
                }
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        case 0x50: { // Down Arrow (history)
            if (GBasecon.History.Current < GBasecon.History.Count) {
                GBasecon.History.Current++;
                if (GBasecon.History.Current < GBasecon.History.Count) {
                    StrCpy(Buffer, GBasecon.History.Lines[GBasecon.History.Current]);
                } else {
                    Buffer[0] = '\0';
                }
                *Len = StrLen(Buffer);
                *CursorPos = *Len;
                if (*Len > GBasecon.Width) {
                    GBasecon.Input.ViewStart = *Len - GBasecon.Width + 1;
                } else {
                    GBasecon.Input.ViewStart = 0;
                }
                if (GBasecon.PromptFixed) {
                    BaseconRedrawFull();
                }
            }
            break;
        }
        
        default: {
            if ((CharCode >= 0x20 && CharCode <= 0x7E) || 
                (CharCode >= 0x80 && CharCode <= 0xBF)) {
                if (*Len < BASECON_INPUT_BUFFER_SIZE - 1) {
                    for (UINT32 I = *Len; I > *CursorPos; I--) {
                        Buffer[I] = Buffer[I - 1];
                    }
                    Buffer[*CursorPos] = Ascii;
                    (*Len)++;
                    (*CursorPos)++;
                    
                    if (*CursorPos - GBasecon.Input.ViewStart >= GBasecon.Width) {
                        GBasecon.Input.ViewStart = *CursorPos - GBasecon.Width + 1;
                    }
                    
                    Buffer[*Len] = '\0';
                    if (GBasecon.PromptFixed) {
                        BaseconRedrawFull();  // ← Redraw with new text
                    }
                }
            }
            break;
        }
    }
}

// ============================================================================
// INITIALIZATION
// ============================================================================

INT BaseconInit(Framebuffer *FB) {
    if (!FB || !FB->Initialized) RETURN(INCORRECT_VALUE);
    
    MemSet(&GBasecon, 0, sizeof(Basecon));
    
    GBasecon.FB = FB;
    GBasecon.Width = FB->Width / FONT_WIDTH;
    GBasecon.Height = FB->Height / FONT_LINE_HEIGHT;
    GBasecon.Fg = RGB_WHITE;
    GBasecon.Bg = RGB_BLACK;
    GBasecon.Initialized = TRUE;
    GBasecon.InputMode = FALSE;
    GBasecon.Input.Length = 0;
    GBasecon.Input.CursorPos = 0;
    GBasecon.Input.ViewStart = 0;
    GBasecon.Input.Buffer[0] = '\0';
    GBasecon.History.Count = 0;
    GBasecon.History.Current = 0;
    StrCpy(GBasecon.Prompt, "$> ");
    GBaseconSubscribed = FALSE;
    GBasecon.CursorX = 0;
    GBasecon.CursorY = 0;
    GBasecon.ScrollY = 0;
    
    GBasecon.PromptFixed = TRUE;
    GBasecon.Scrollback.Count = 0;
    GBasecon.Scrollback.StartLine = 0;
    GBasecon.Scrollback.MaxDisplayLines = GBasecon.Height - 1;
    GBasecon.WaitingForProgram = FALSE;
    GBasecon.WaitingPid = -1;
    
    BaseconClear();
    
    RETURN(SUCCESS);
}
