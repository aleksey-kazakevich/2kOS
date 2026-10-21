#include <Drivers/Ps2/Ps2Keyboard.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdArg.h>
#include <FontAccess.h>
#include <Lib/StdIo.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <SpinLock.h>
#include <Return.h>
#include <Rgb.h>

STATIC BOOL BaseconEnabled = FALSE;
STATIC Basecon GBasecon = {0};

STATIC BaseconInputCallback GInputCallback = NULLPTR;
STATIC VOID *GInputCallbackUserData = NULLPTR;
STATIC BOOL GBaseconSubscribed = FALSE;
STATIC volatile BOOL GRunning = TRUE;
STATIC Task *GConsoleTask = NULLPTR;

STATIC struct {
    BaseconKeyEvent Events[BASECON_KEY_QUEUE_SIZE];
    volatile UINT32 Head;
    volatile UINT32 Tail;
    volatile BOOL Blocked;
    SpinLock Lock;
} GKeyQueue = {0};

STATIC VOID KeyboardHandler(KeyEvent *Event, VOID *UserData);

// ============================================================================
// LOCKED INTERNAL HELPERS (without lock)
// ============================================================================

STATIC VOID BaseconPutCharLocked(CHAR C);
STATIC VOID BaseconRedrawFullLocked(VOID);

STATIC VOID BaseconAddToScrollbackLocked(const CHAR *Str) {
    if (!Str || !Str[0]) return;
    if (!GBasecon.Initialized) return;
    if (!GBasecon.PromptFixed) return;

    CHAR Buffer[1024];
    CHAR *SavePtr = NULLPTR;

    StrnCpy(Buffer, Str, sizeof(Buffer) - 1);
    Buffer[sizeof(Buffer) - 1] = '\0';

    FramebufferColor LineColor = GBasecon.Fg;

    CHAR *Line = StrTokR(Buffer, "\n", &SavePtr);
    while (Line) {
        if (GBasecon.Scrollback.Count < BASECON_SCROLLBACK_SIZE) {
            StrCpy(GBasecon.Scrollback.Lines[GBasecon.Scrollback.Count], Line);
            GBasecon.Scrollback.Colors[GBasecon.Scrollback.Count] = LineColor;
            GBasecon.Scrollback.Count++;
        } else {
            for (UINT32 I = 0; I < BASECON_SCROLLBACK_SIZE - 1; I++) {
                StrCpy(GBasecon.Scrollback.Lines[I],
                       GBasecon.Scrollback.Lines[I + 1]);
                GBasecon.Scrollback.Colors[I] =
                    GBasecon.Scrollback.Colors[I + 1];
            }
            StrCpy(GBasecon.Scrollback.Lines[BASECON_SCROLLBACK_SIZE - 1], Line);
            GBasecon.Scrollback.Colors[BASECON_SCROLLBACK_SIZE - 1] = LineColor;
        }
        Line = StrTokR(NULLPTR, "\n", &SavePtr);
    }

    BaseconRedrawFullLocked();
}

STATIC VOID BaseconRedrawFullLocked(VOID) {
    if (!GBasecon.Initialized || !BaseconEnabled) return;
    if (!GBasecon.PromptFixed) return;

    FramebufferClear(GBasecon.Bg);

    CHAR FullLine[BASECON_INPUT_BUFFER_SIZE + 128];
    StrCpy(FullLine, GBasecon.Prompt);
    if (GBasecon.InputMode) {
        StrCat(FullLine, GBasecon.Input.Buffer);
    }

    UINT32 FullLen = (UINT32)StrLen(FullLine);
    UINT32 LineWidth = GBasecon.Width;
    if (LineWidth == 0) LineWidth = 1;

    UINT32 NumPromptLines = (FullLen + LineWidth - 1) / LineWidth;
    if (NumPromptLines == 0) NumPromptLines = 1;

    UINT32 MaxDisplayLines = GBasecon.Height - NumPromptLines;
    if (MaxDisplayLines > GBasecon.Height) MaxDisplayLines = 0;

    UINT32 StartLine = 0;
    if (GBasecon.Scrollback.Count > MaxDisplayLines) {
        StartLine = GBasecon.Scrollback.Count - MaxDisplayLines;
    }

    for (UINT32 I = 0; I < MaxDisplayLines; I++) {
        UINT32 SrcIdx = StartLine + I;
        if (SrcIdx >= GBasecon.Scrollback.Count) break;

        UINT32 Y = I * FONT_LINE_HEIGHT;
        FramebufferDrawString(GBasecon.Scrollback.Lines[SrcIdx],
                              0, (INT32)Y,
                              GBasecon.Scrollback.Colors[SrcIdx],
                              GBasecon.Bg);
    }

    UINT32 PromptBaseY = MaxDisplayLines * FONT_LINE_HEIGHT;

    for (UINT32 LineIdx = 0; LineIdx < NumPromptLines; LineIdx++) {
        UINT32 StartChar = LineIdx * LineWidth;
        UINT32 CharsThisLine = FullLen - StartChar;
        if (CharsThisLine > LineWidth) CharsThisLine = LineWidth;
        if (CharsThisLine == 0 && LineIdx > 0) break;

        CHAR LineBuf[BASECON_INPUT_BUFFER_SIZE + 128];
        StrnCpy(LineBuf, FullLine + StartChar, CharsThisLine);
        LineBuf[CharsThisLine] = '\0';

        INT32 DrawY = (INT32)PromptBaseY + (INT32)(LineIdx * FONT_LINE_HEIGHT);

        FramebufferFillRect(0, DrawY, GBasecon.FB->Width, FONT_LINE_HEIGHT,
                            GBasecon.Bg);

        FramebufferDrawString(LineBuf, 0, DrawY, GBasecon.Fg, GBasecon.Bg);
    }

    if (GBasecon.InputMode) {
        UINT32 PromptLen = (UINT32)StrLen(GBasecon.Prompt);
        UINT32 CursorGlobal = PromptLen + GBasecon.Input.CursorPos;
        UINT32 CursorLine = CursorGlobal / LineWidth;
        UINT32 CursorCol = CursorGlobal % LineWidth;

        UINT32 CursorX = CursorCol * FONT_WIDTH;
        UINT32 CursorY = PromptBaseY + CursorLine * FONT_LINE_HEIGHT
                         + FONT_LINE_HEIGHT - 2;

        if (CursorX < GBasecon.FB->Width &&
            CursorY + 2 <= GBasecon.FB->Height) {
            FramebufferFillRect((INT32)CursorX, (INT32)CursorY, FONT_WIDTH, 2, GBasecon.Fg);
        }
    }

    FramebufferSwapBuffers();
}

STATIC VOID BaseconPutCharLocked(CHAR C) {
    if (!GBasecon.Initialized) return;
    if (!BaseconEnabled) return;

    if (GBasecon.PromptFixed) {
        CHAR *LineBuffer = GBasecon.OutLineBuffer;
        UINT32 *LineLen = &GBasecon.OutLineLen;

        if (C == '\n') {
            if (*LineLen > 0) {
                LineBuffer[*LineLen] = '\0';
                BaseconAddToScrollbackLocked(LineBuffer);
                *LineLen = 0;
                LineBuffer[0] = '\0';
            }
            return;
        }

        if (C == '\r') {
            *LineLen = 0;
            LineBuffer[0] = '\0';
            BaseconRedrawFullLocked();
            return;
        }

        if (C == '\b') {
            if (*LineLen > 0) (*LineLen)--;
            return;
        }

        if ((C >= 0x20 && C <= 0x7E) || (C >= 0x80 && C <= 0xBF)) {
            if (*LineLen >= GBasecon.Width) {
                LineBuffer[*LineLen] = '\0';
                BaseconAddToScrollbackLocked(LineBuffer);
                *LineLen = 0;
            }

            if (*LineLen < BASECON_OUTLINE_BUFFER_SIZE - 1) {
                LineBuffer[(*LineLen)++] = C;
            }
        }
        return;
    }

    // Old mode
    if (C == '\n') {
        GBasecon.CursorX = 0;
        GBasecon.CursorY++;
        if (GBasecon.CursorY >= GBasecon.Height) {
            Framebuffer *FB = GBasecon.FB;
            UINT32 LineBytes = FONT_LINE_HEIGHT * FB->Pitch;
            UINT32 TotalBytes = FB->Height * FB->Pitch;
            UINT8 *Buf = (UINT8*)FB->Layers[0].Buffer;

            if (Buf && TotalBytes > LineBytes) {
                MemMove(Buf, Buf + LineBytes, TotalBytes - LineBytes);
                MemSet(Buf + TotalBytes - LineBytes, 0, LineBytes);
            }
            GBasecon.CursorY = GBasecon.Height - 1;
        }
    } else if (C >= 0x20 || C >= 0x80) {
        UINT32 PixelX = GBasecon.CursorX * FONT_WIDTH;
        UINT32 PixelY = GBasecon.CursorY * FONT_LINE_HEIGHT;
        FramebufferDrawChar(C, (INT32)PixelX, (INT32)PixelY, GBasecon.Fg, GBasecon.Bg);
        GBasecon.CursorX++;
        if (GBasecon.CursorX >= GBasecon.Width) {
            GBasecon.CursorX = 0;
            GBasecon.CursorY++;
            if (GBasecon.CursorY >= GBasecon.Height) {
                Framebuffer *FB = GBasecon.FB;
                UINT32 LineBytes = FONT_LINE_HEIGHT * FB->Pitch;
                UINT32 TotalBytes = FB->Height * FB->Pitch;
                UINT8 *Buf = (UINT8*)FB->Layers[0].Buffer;

                if (Buf && TotalBytes > LineBytes) {
                    MemMove(Buf, Buf + LineBytes, TotalBytes - LineBytes);
                    MemSet(Buf + TotalBytes - LineBytes, 0, LineBytes);
                }
                GBasecon.CursorY = GBasecon.Height - 1;
            }
        }
    }
}

STATIC VOID BaseconPutStringLocked(const CHAR *Str) {
    if (!GBasecon.Initialized || !Str) return;
    if (!BaseconEnabled) return;

    while (*Str) {
        BaseconPutCharLocked(*Str);
        Str++;
    }

    FramebufferSwapBuffers();
}

// ============================================================================
// KEY QUEUE (separate lock)
// ============================================================================

VOID BaseconQueueKey(UINT8 ScanCode, CHAR Ascii, BOOL Pressed) {
    UINTPTR Flags = SpinLockAcquireIrq(&GKeyQueue.Lock);

    UINT32 Next = (GKeyQueue.Head + 1) % BASECON_KEY_QUEUE_SIZE;
    if (Next == GKeyQueue.Tail) {
        SpinLockReleaseIrq(&GKeyQueue.Lock, Flags);
        return;
    }

    GKeyQueue.Events[GKeyQueue.Head].ScanCode = ScanCode;
    GKeyQueue.Events[GKeyQueue.Head].Ascii = Ascii;
    GKeyQueue.Events[GKeyQueue.Head].Pressed = Pressed;
    GKeyQueue.Head = Next;

    BOOL NeedWake = GKeyQueue.Blocked;
    GKeyQueue.Blocked = FALSE;

    SpinLockReleaseIrq(&GKeyQueue.Lock, Flags);

    if (NeedWake && GConsoleTask) {
        TaskWakeup(GConsoleTask);
    }
}

// ============================================================================
// PUBLIC (they take GBasecon.Lock)
// ============================================================================

VOID BaseconAddToScrollback(const CHAR *Str) {
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    BaseconAddToScrollbackLocked(Str);
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

VOID BaseconRedrawFull(VOID) {
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    BaseconRedrawFullLocked();
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

VOID BaseconCheckProgramStatus(VOID) {
    // We read without lock — flags are written under lock, but reading is reasonable
    // not critical for accuracy
    if (!GBasecon.WaitingForProgram) return;
    if (!TaskIsAlive(GBasecon.WaitingPid)) {
        GBasecon.WaitingForProgram = FALSE;
        GBasecon.WaitingPid = -1;
        BaseconInputStart();
    }
}

VOID BaseconSetPromptFixed(BOOL Enable) {
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    if (!GBasecon.Initialized) {
        SpinLockReleaseIrq(&GBasecon.Lock, Flags);
        return;
    }

    GBasecon.PromptFixed = Enable;

    if (Enable) {
        GBasecon.Scrollback.Count = 0;
        GBasecon.Scrollback.StartLine = 0;
        GBasecon.Scrollback.MaxDisplayLines = GBasecon.Height - 1;
        GBasecon.WaitingForProgram = FALSE;
        GBasecon.WaitingPid = -1;

        // inline clear + redraw (without taking the lock again)
        GBasecon.Scrollback.Count = 0;
        GBasecon.Scrollback.StartLine = 0;
        BaseconRedrawFullLocked();
    }
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

VOID BaseconPutString(const CHAR *Str) {
    if (!GBasecon.Initialized || !Str) return;
    if (!BaseconEnabled) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    BaseconPutStringLocked(Str);
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

INT BaseconPrintf(BaseconMsgType Type, const CHAR *Fmt, ...) {
    if (!GBasecon.Initialized || !Fmt) RETURN(INCORRECT_VALUE);
    if (!BaseconEnabled) RETURN(OPERATION_DISABLED);

    CHAR Buffer[1024];
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT Result = VsnPrintf(Buffer, sizeof(Buffer), Fmt, Args);
    VaEnd(Args);

    if (Result <= 0) RETURN(Result);

    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);

    FramebufferColor OldFg = GBasecon.Fg;
    FramebufferColor OldBg = GBasecon.Bg;

    const CHAR *Prefix = NULLPTR;
    FramebufferColor PrefixColor = OldFg;
    BOOL HasPrefix = FALSE;

    switch (Type) {
        case BASECON_TYPE_ERROR:   Prefix = "E: "; PrefixColor = RGB_RED;    HasPrefix = TRUE; break;
        case BASECON_TYPE_INFO:    Prefix = "I: "; PrefixColor = RGB_BLUE;   HasPrefix = TRUE; break;
        case BASECON_TYPE_SUCCESS: Prefix = "S: "; PrefixColor = RGB_GREEN;  HasPrefix = TRUE; break;
        case BASECON_TYPE_WARNING: Prefix = "W: "; PrefixColor = RGB_YELLOW; HasPrefix = TRUE; break;
        default: break;
    }

    if (HasPrefix) {
        GBasecon.Fg = PrefixColor;
        BaseconPutStringLocked(Prefix);
        GBasecon.Fg = OldFg;
    }

    BaseconPutStringLocked(Buffer);

    SpinLockReleaseIrq(&GBasecon.Lock, Flags);

    RETURN(Result);
}

VOID BaseconSetColors(FramebufferColor Fg, FramebufferColor Bg) {
    if (!GBasecon.Initialized) return;
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    GBasecon.Fg = Fg;
    GBasecon.Bg = Bg;
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
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

    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);

    if (GBasecon.PromptFixed) {
        GBasecon.Scrollback.Count = 0;
        GBasecon.Scrollback.StartLine = 0;
        BaseconRedrawFullLocked();
    } else {
        GBasecon.CursorX = 0;
        GBasecon.CursorY = 0;
        FramebufferClear(GBasecon.Bg);
    }

    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

VOID BaseconSetPrompt(const CHAR *Prompt) {
    if (!GBasecon.Initialized) return;
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    if (Prompt) {
        StrnCpy(GBasecon.Prompt, Prompt, sizeof(GBasecon.Prompt) - 1);
        GBasecon.Prompt[sizeof(GBasecon.Prompt) - 1] = '\0';
    } else {
        GBasecon.Prompt[0] = '\0';
    }
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
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

    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);

    GBasecon.Fg = RGB_GRAY;
    GBasecon.Bg = RGB_BLACK;

    GBasecon.InputMode = TRUE;
    GBasecon.Input.Length = 0;
    GBasecon.Input.CursorPos = 0;
    GBasecon.Input.ViewStart = 0;
    GBasecon.Input.Buffer[0] = '\0';

    if (GBasecon.PromptFixed) {
        BaseconRedrawFullLocked();
    } else {
        if (GBasecon.Prompt[0]) {
            BaseconPutStringLocked(GBasecon.Prompt);
        }
    }

    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

VOID BaseconInputEnd(VOID) {
    if (!GBasecon.Initialized) return;
    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
    GBasecon.InputMode = FALSE;
    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
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

STATIC VOID KeyboardHandler(KeyEvent *Event, VOID *UserData) {
    (VOID)UserData;
    if (!Event) return;
    BaseconQueueKey(Event->RawScanCode, Event->Ascii,
                    Event->State == KEY_STATE_PRESSED);
}

// ============================================================================
// KEY PROCESSING — takes GBasecon.Lock once
// ============================================================================

VOID BaseconProcessKey(UINT8 ScanCode, CHAR Ascii, BOOL Pressed) {
    if (!GBasecon.Initialized || !BaseconEnabled) return;
    if (!Pressed) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);

    UINT8 CharCode = (UINT8)Ascii;

    if (!GBasecon.InputMode) {
        if ((CharCode >= 0x20 && CharCode <= 0x7E) ||
            (CharCode >= 0x80 && CharCode <= 0xBF)) {
            BaseconPutCharLocked(Ascii);
        }
        SpinLockReleaseIrq(&GBasecon.Lock, Flags);
        return;
    }

    CHAR *Buffer = GBasecon.Input.Buffer;
    UINT32 *Len = &GBasecon.Input.Length;
    UINT32 *CursorPos = &GBasecon.Input.CursorPos;
    UINT32 MaxInputChars = BASECON_INPUT_BUFFER_SIZE - 1;

    switch (ScanCode) {
        case 0x1C: { // Enter
            if (GBasecon.PromptFixed && *Len > 0) {
                CHAR FullLine[BASECON_INPUT_BUFFER_SIZE + 128];
                StrCpy(FullLine, GBasecon.Prompt);
                StrCat(FullLine, Buffer);
                BaseconAddToScrollbackLocked(FullLine);
            }

            if (*Len > 0) {
                if (GBasecon.History.Count < BASECON_HISTORY_SIZE) {
                    StrCpy(GBasecon.History.Lines[GBasecon.History.Count], Buffer);
                    GBasecon.History.Count++;
                } else {
                    for (UINT32 I = 0; I < BASECON_HISTORY_SIZE - 1; I++) {
                        StrCpy(GBasecon.History.Lines[I],
                               GBasecon.History.Lines[I + 1]);
                    }
                    StrCpy(GBasecon.History.Lines[BASECON_HISTORY_SIZE - 1],
                           Buffer);
                }
                GBasecon.History.Current = GBasecon.History.Count;
            }

            // Calling a callback OUTSIDE the lock so that there is no deadline
            BaseconInputCallback Cb = GInputCallback;
            VOID *CbData = GInputCallbackUserData;
            CHAR CbBuffer[BASECON_INPUT_BUFFER_SIZE];
            StrnCpy(CbBuffer, Buffer, sizeof(CbBuffer) - 1);
            CbBuffer[sizeof(CbBuffer) - 1] = '\0';

            *Len = 0;
            *CursorPos = 0;
            GBasecon.Input.ViewStart = 0;
            Buffer[0] = '\0';
            GBasecon.InputMode = FALSE;

            BOOL NeedRestart = !GBasecon.WaitingForProgram;

            SpinLockReleaseIrq(&GBasecon.Lock, Flags);

            if (Cb) {
                Cb(CbBuffer, CbData);
            }
            if (NeedRestart) {
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
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        case 0x53: { // Delete
            if (*CursorPos < *Len) {
                for (UINT32 I = *CursorPos; I < *Len - 1; I++) {
                    Buffer[I] = Buffer[I + 1];
                }
                (*Len)--;
                Buffer[*Len] = '\0';
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        case 0x4B: { // Left
            if (*CursorPos > 0) {
                (*CursorPos)--;
                if (*CursorPos < GBasecon.Input.ViewStart) {
                    GBasecon.Input.ViewStart = *CursorPos;
                }
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        case 0x4D: { // Right
            if (*CursorPos < *Len) {
                (*CursorPos)++;
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        case 0x47: { // Home
            *CursorPos = 0;
            GBasecon.Input.ViewStart = 0;
            if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            break;
        }

        case 0x4F: { // End
            *CursorPos = *Len;
            if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            break;
        }

        case 0x48: { // Up
            if (GBasecon.History.Current > 0) {
                GBasecon.History.Current--;
                StrCpy(Buffer, GBasecon.History.Lines[GBasecon.History.Current]);
                *Len = (UINT32)StrLen(Buffer);
                *CursorPos = *Len;
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        case 0x50: { // Down
            if (GBasecon.History.Current < GBasecon.History.Count) {
                GBasecon.History.Current++;
                if (GBasecon.History.Current < GBasecon.History.Count) {
                    StrCpy(Buffer,
                           GBasecon.History.Lines[GBasecon.History.Current]);
                } else {
                    Buffer[0] = '\0';
                }
                *Len = (UINT32)StrLen(Buffer);
                *CursorPos = *Len;
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }

        default: {
            if ((CharCode >= 0x20 && CharCode <= 0x7E) ||
                (CharCode >= 0x80 && CharCode <= 0xBF)) {
                if (*Len >= MaxInputChars) break;
                for (UINT32 I = *Len; I > *CursorPos; I--) {
                    Buffer[I] = Buffer[I - 1];
                }
                Buffer[*CursorPos] = Ascii;
                (*Len)++;
                (*CursorPos)++;
                Buffer[*Len] = '\0';
                if (GBasecon.PromptFixed) BaseconRedrawFullLocked();
            }
            break;
        }
    }

    SpinLockReleaseIrq(&GBasecon.Lock, Flags);
}

// ============================================================================
// INIT
// ============================================================================

INT BaseconInit(Framebuffer *FB) {
    if (!FB || !FB->Initialized) RETURN(INCORRECT_VALUE);

    MemSet(&GBasecon, 0, sizeof(Basecon));

    GBasecon.FB = FB;
    GBasecon.Width = FB->Width / FONT_WIDTH;
    GBasecon.Height = FB->Height / FONT_LINE_HEIGHT;
    GBasecon.Fg = RGB_GRAY;
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

    GRunning = TRUE;

    GBasecon.PromptFixed = FALSE;
    GBasecon.Scrollback.Count = 0;
    GBasecon.Scrollback.StartLine = 0;
    GBasecon.Scrollback.MaxDisplayLines = GBasecon.Height - 1;
    GBasecon.WaitingForProgram = FALSE;
    GBasecon.WaitingPid = -1;

    GBasecon.OutLineLen = 0;
    GBasecon.OutLineBuffer[0] = '\0';

    SpinLockInit(&GBasecon.Lock);

    BaseconClear();

    RETURN(SUCCESS);
}

STATIC VOID ConsoleThread(VOID) {
    GConsoleTask = GetCurrentTask();

    while (GRunning) {
        while (GRunning) {
            BaseconKeyEvent Ev;
            BOOL Have = FALSE;

            UINTPTR Flags = SpinLockAcquireIrq(&GKeyQueue.Lock);
            if (GKeyQueue.Head != GKeyQueue.Tail) {
                Ev = GKeyQueue.Events[GKeyQueue.Tail];
                GKeyQueue.Tail = (GKeyQueue.Tail + 1) % BASECON_KEY_QUEUE_SIZE;
                Have = TRUE;
            }
            SpinLockReleaseIrq(&GKeyQueue.Lock, Flags);

            if (!Have) break;
            BaseconProcessKey(Ev.ScanCode, Ev.Ascii, Ev.Pressed);
        }

        if (!GRunning) break;

        UINTPTR Flags = SpinLockAcquireIrq(&GKeyQueue.Lock);
        BOOL NeedBlock = (GKeyQueue.Head == GKeyQueue.Tail);
        if (NeedBlock) GKeyQueue.Blocked = TRUE;
        SpinLockReleaseIrq(&GKeyQueue.Lock, Flags);

        if (NeedBlock) {
            TaskBlock();
        }
    }

    /* kconsole is being completed normally. */
    GConsoleTask = NULLPTR;
    TaskExit(0);
}

VOID BaseconStartConsoleThread(VOID) {
    if (GConsoleTask) return;
    if (!GRunning) return;

    SpinLockInit(&GKeyQueue.Lock);
    GKeyQueue.Head = 0;
    GKeyQueue.Tail = 0;
    GKeyQueue.Blocked = FALSE;

    INT Pid = TaskCreate(ConsoleThread, 1024 * 1024, "kconsole");
    if (Pid < 0) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "basecon: failed to start kconsole thread\n");
    }
}

VOID BaseconShutdown(BOOL ClearScreen) {
    /* Idempotence: if it is already turned off, we exit. */
    if (!GRunning && !BaseconEnabled) return;

    /* 1. Stop kconsole. */
    GRunning = FALSE;

    if (GConsoleTask) {
        TaskWakeup(GConsoleTask);   /* wake up if in TaskBlock */
    }

    /* 2. Clear the screen (before disabling BaseconEnabled). */
    if (ClearScreen && GBasecon.Initialized) {
        UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
        FramebufferClear(GBasecon.Bg);
        FramebufferSwapBuffers();
        SpinLockReleaseIrq(&GBasecon.Lock, Flags);
    }

    /* 3. Unsubscribe from the keyboard. */
    if (GBaseconSubscribed) {
        Ps2KeyboardUnsubscribe(KeyboardHandler);
        GBaseconSubscribed = FALSE;
    }

    /* 4. Reset the callback. */
    GInputCallback = NULLPTR;
    GInputCallbackUserData = NULLPTR;

    /* 5. Clear the queue of keys. */
    {
        UINTPTR Flags = SpinLockAcquireIrq(&GKeyQueue.Lock);
        GKeyQueue.Head = 0;
        GKeyQueue.Tail = 0;
        GKeyQueue.Blocked = FALSE;
        SpinLockReleaseIrq(&GKeyQueue.Lock, Flags);
    }

    /* 6. Disable BaseconEnabled — all Basecon* functions will return immediately. */
    BaseconEnabled = FALSE;

    /* 7. Reset the GBasecon flags. */
    if (GBasecon.Initialized) {
        UINTPTR Flags = SpinLockAcquireIrq(&GBasecon.Lock);
        GBasecon.InputMode = FALSE;
        GBasecon.PromptFixed = FALSE;
        GBasecon.WaitingForProgram = FALSE;
        GBasecon.WaitingPid = -1;
        GBasecon.Input.Length = 0;
        GBasecon.Input.CursorPos = 0;
        GBasecon.Input.ViewStart = 0;
        GBasecon.Input.Buffer[0] = '\0';
        GBasecon.OutLineLen = 0;
        GBasecon.OutLineBuffer[0] = '\0';
        SpinLockReleaseIrq(&GBasecon.Lock, Flags);
    }
}

BOOL BaseconIsShutdown(VOID) {
    return !GRunning;
}