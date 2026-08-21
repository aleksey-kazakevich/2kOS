#include <Drivers/Ps2/Ps2Keyboard.h>
#include <Drivers/Ps2/Ps2.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>
#include <Idt.h>
#include <Drivers/Apic/Apic.h>
#include <Drivers/Apic/Ioapic.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Basecon.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

// ============================================================================
// SET 1 -> ASCII
// ============================================================================

// Character table for normal mode (without Shift) - SET 1
static const CHAR GSet1Lower[256] = {
    // 0x00-0x0F
    [0x01] = 27,    // ESC
    [0x02] = '1',
    [0x03] = '2',
    [0x04] = '3',
    [0x05] = '4',
    [0x06] = '5',
    [0x07] = '6',
    [0x08] = '7',
    [0x09] = '8',
    [0x0A] = '9',
    [0x0B] = '0',
    [0x0C] = '-',
    [0x0D] = '=',
    [0x0E] = '\b',  // BACKSPACE
    [0x0F] = '\t',  // TAB
    
    // 0x10-0x1F
    [0x10] = 'q',
    [0x11] = 'w',
    [0x12] = 'e',
    [0x13] = 'r',
    [0x14] = 't',
    [0x15] = 'y',
    [0x16] = 'u',
    [0x17] = 'i',
    [0x18] = 'o',
    [0x19] = 'p',
    [0x1A] = '[',
    [0x1B] = ']',
    [0x1C] = '\n',  // ENTER
    [0x1E] = 'a',
    [0x1F] = 's',
    
    // 0x20-0x2F
    [0x20] = 'd',
    [0x21] = 'f',
    [0x22] = 'g',
    [0x23] = 'h',
    [0x24] = 'j',
    [0x25] = 'k',
    [0x26] = 'l',
    [0x27] = ';',
    [0x28] = '\'',
    [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',
    [0x2D] = 'x',
    [0x2E] = 'c',
    [0x2F] = 'v',
    
    // 0x30-0x3F
    [0x30] = 'b',
    [0x31] = 'n',
    [0x32] = 'm',
    [0x33] = ',',
    [0x34] = '.',
    [0x35] = '/',
    [0x39] = ' ',  // SPACE
    
    // 0x47-0x53 (Numpad)
    [0x47] = '7',
    [0x48] = '8',
    [0x49] = '9',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

// Symbol table with Shift - SET 1
static const CHAR GSet1Upper[256] = {
    // 0x00-0x0F
    [0x01] = 27,    // ESC
    [0x02] = '!',
    [0x03] = '@',
    [0x04] = '#',
    [0x05] = '$',
    [0x06] = '%',
    [0x07] = '^',
    [0x08] = '&',
    [0x09] = '*',
    [0x0A] = '(',
    [0x0B] = ')',
    [0x0C] = '_',
    [0x0D] = '+',
    [0x0E] = '\b',  // BACKSPACE
    [0x0F] = '\t',  // TAB
    
    // 0x10-0x1F
    [0x10] = 'Q',
    [0x11] = 'W',
    [0x12] = 'E',
    [0x13] = 'R',
    [0x14] = 'T',
    [0x15] = 'Y',
    [0x16] = 'U',
    [0x17] = 'I',
    [0x18] = 'O',
    [0x19] = 'P',
    [0x1A] = '{',
    [0x1B] = '}',
    [0x1C] = '\n',  // ENTER
    [0x1E] = 'A',
    [0x1F] = 'S',
    
    // 0x20-0x2F
    [0x20] = 'D',
    [0x21] = 'F',
    [0x22] = 'G',
    [0x23] = 'H',
    [0x24] = 'J',
    [0x25] = 'K',
    [0x26] = 'L',
    [0x27] = ':',
    [0x28] = '"',
    [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',
    [0x2D] = 'X',
    [0x2E] = 'C',
    [0x2F] = 'V',
    
    // 0x30-0x3F
    [0x30] = 'B',
    [0x31] = 'N',
    [0x32] = 'M',
    [0x33] = '<',
    [0x34] = '>',
    [0x35] = '?',
    [0x39] = ' ',  // SPACE
    
    // 0x47-0x53 (Numpad)
    [0x47] = '7',
    [0x48] = '8',
    [0x49] = '9',
    [0x4B] = '4',
    [0x4C] = '5',
    [0x4D] = '6',
    [0x4F] = '1',
    [0x50] = '2',
    [0x51] = '3',
    [0x52] = '0',
    [0x53] = '.',
};

// ============================================================================
// STATE STRUCTURE
// ============================================================================

typedef struct {
    BOOL Initialized;
    BOOL Enabled;
    UINT8 Modifiers;
    BOOL KeyStates[256];
    BOOL ExtendedPrefix;
    KeyboardLayout Layout;
    UINT64 EventCount;
    UINT64 ErrorCount;
    KDriver *Driver;
    UINT32 Gsi;
    struct {
        KeyboardCallback Callback;
        VOID *UserData;
        BOOL Active;
    } Subscribers[PS2_MAX_SUBSCRIBERS];
} Ps2KeyboardState;

static Ps2KeyboardState GPs2 = {0};

static struct {
    KeyEvent Events[PS2_KEY_QUEUE_SIZE];
    volatile UINT32 Head;
    volatile UINT32 Tail;
} GPs2KeyQueue = {0};

// ============================================================================
// PROTOTYPES OF INTERNAL FUNCTIONS
// ============================================================================

static BOOL Ps2KeyboardWaitWrite(INT TimeoutMs);
static BOOL Ps2KeyboardWaitRead(INT TimeoutMs);
static VOID Ps2KeyboardFlushBuffer(VOID);
static BOOL Ps2KeyboardSendCommand(UINT8 Cmd);
static BOOL Ps2KeyboardReadDataWithTimeout(UINT8 *Data, INT TimeoutMs);
static VOID UpdateModifiers(UINT8 RawCode, BOOL Pressed);
static CHAR ScanCodeToAscii(UINT8 ScanCode, BOOL Pressed, UINT8 Modifiers);
static VOID ProcessKeyEvent(UINT8 ScanCode, BOOL Pressed, UINT8 Modifiers);
static BOOL Ps2KeyboardDisable(VOID);
static BOOL Ps2KeyboardEnable(VOID);
static BOOL Ps2KeyboardReset(VOID);
static BOOL Ps2KeyboardSetScanCodeSet(UINT8 Set);
static UINT8 Ps2KeyboardQueryScanCodeSet(VOID);
static VOID Ps2KeyboardResetModifiers(VOID);

// ============================================================================
// QUEUE FUNCTIONS
// ============================================================================

static BOOL Ps2KeyQueuePush(const KeyEvent *Event) {
    UINT32 Next = (GPs2KeyQueue.Head + 1) % PS2_KEY_QUEUE_SIZE;
    if (Next == GPs2KeyQueue.Tail) {
        GPs2.ErrorCount++;
        return FALSE;
    }
    GPs2KeyQueue.Events[GPs2KeyQueue.Head] = *Event;
    GPs2KeyQueue.Head = Next;
    return TRUE;
}

static BOOL Ps2KeyQueuePop(KeyEvent *Event) {
    if (GPs2KeyQueue.Head == GPs2KeyQueue.Tail) {
        return FALSE;
    }
    *Event = GPs2KeyQueue.Events[GPs2KeyQueue.Tail];
    GPs2KeyQueue.Tail = (GPs2KeyQueue.Tail + 1) % PS2_KEY_QUEUE_SIZE;
    return TRUE;
}

// ============================================================================
// HARDWARE COMMUNICATION
// ============================================================================

static BOOL Ps2KeyboardWaitWrite(INT TimeoutMs) {
    while (TimeoutMs--) {
        if (!(Inb(PS2_PORT_STATUS) & PS2_STATUS_INPUT_FULL)) {
            return TRUE;
        }
        TimerMdelay(1);
    }
    return FALSE;
}

static BOOL Ps2KeyboardWaitRead(INT TimeoutMs) {
    while (TimeoutMs--) {
        if (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
            return TRUE;
        }
        TimerMdelay(1);
    }
    return FALSE;
}

static VOID Ps2KeyboardFlushBuffer(VOID) {
    INT Count = 0;
    while ((Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) && Count < PS2_DRAIN_MAX_BYTES) {
        Inb(PS2_PORT_DATA);
        Count++;
        TimerMdelay(1);
    }
}

static BOOL Ps2KeyboardSendCommand(UINT8 Cmd) {
    Ps2KeyboardFlushBuffer();
    TimerMdelay(5);
    
    if (!Ps2KeyboardWaitWrite(100)) return FALSE;
    Outb(PS2_PORT_DATA, Cmd);
    TimerMdelay(2);
    
    INT Timeout = 300;
    UINT8 Response;
    while (Timeout--) {
        if (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
            Response = Inb(PS2_PORT_DATA);
            if (Response == 0xFA) return TRUE;
            if (Response == 0xFE) {
                if (!Ps2KeyboardWaitWrite(100)) return FALSE;
                Outb(PS2_PORT_DATA, Cmd);
                continue;
            }
        }
        TimerMdelay(1);
    }
    return FALSE;
}

static BOOL Ps2KeyboardReadDataWithTimeout(UINT8 *Data, INT TimeoutMs) {
    while (TimeoutMs--) {
        if (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
            *Data = Inb(PS2_PORT_DATA);
            return TRUE;
        }
        TimerMdelay(1);
    }
    return FALSE;
}

// ============================================================================
// Modifiers reset
// ============================================================================

static VOID Ps2KeyboardResetModifiers(VOID) {
    GPs2.Modifiers = 0;
    MemSet(GPs2.KeyStates, 0, sizeof(GPs2.KeyStates));
    GPs2.ExtendedPrefix = FALSE;
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: modifiers reset\n");
}

static VOID UpdateModifiers(UINT8 RawCode, BOOL Pressed) {
    switch (RawCode) {
        case PS2_SET1_LSHIFT:
        case PS2_SET1_RSHIFT:
            if (Pressed) GPs2.Modifiers |= MOD_SHIFT;
            else GPs2.Modifiers &= ~MOD_SHIFT;
            GPs2.KeyStates[RawCode] = Pressed;
            return;
            
        case PS2_SET1_LCTRL:
            if (Pressed) GPs2.Modifiers |= MOD_CTRL;
            else GPs2.Modifiers &= ~MOD_CTRL;
            GPs2.KeyStates[RawCode] = Pressed;
            return;
            
        case PS2_SET1_LALT:
            if (Pressed) GPs2.Modifiers |= MOD_ALT;
            else GPs2.Modifiers &= ~MOD_ALT;
            GPs2.KeyStates[RawCode] = Pressed;
            return;
            
        case PS2_SET1_CAPSLOCK:
            if (Pressed && !GPs2.KeyStates[RawCode]) {
                GPs2.Modifiers ^= MOD_CAPSLOCK;
            }
            GPs2.KeyStates[RawCode] = Pressed;
            return;
            
        case PS2_SET1_NUMLOCK:
            if (Pressed && !GPs2.KeyStates[RawCode]) {
                GPs2.Modifiers ^= MOD_NUMLOCK;
            }
            GPs2.KeyStates[RawCode] = Pressed;
            return;
            
        case PS2_SET1_SCROLLLOCK:
            if (Pressed && !GPs2.KeyStates[RawCode]) {
                GPs2.Modifiers ^= MOD_SCROLLLOCK;
            }
            GPs2.KeyStates[RawCode] = Pressed;
            return;
    }
}

static CHAR ScanCodeToAscii(UINT8 ScanCode, BOOL Pressed, UINT8 Modifiers) {
    if (!Pressed) return 0;
    if (ScanCode >= 256) return 0;
    
    const CHAR *Table = (Modifiers & MOD_SHIFT) ? GSet1Upper : GSet1Lower;
    CHAR Ascii = Table[ScanCode];
    
    if (Ascii == 0) return 0;
    
    // CapsLock only affects letters
    if ((Modifiers & MOD_CAPSLOCK) && (Ascii >= 'a' && Ascii <= 'z')) {
        Ascii = Ascii - 'a' + 'A';
    } else if ((Modifiers & MOD_CAPSLOCK) && (Ascii >= 'A' && Ascii <= 'Z')) {
        Ascii = Ascii - 'A' + 'a';
    }
    
    // Ctrl + letter = control character
    if ((Modifiers & MOD_CTRL) && (Ascii >= 'a' && Ascii <= 'z')) {
        Ascii = Ascii - 'a' + 1;
    } else if ((Modifiers & MOD_CTRL) && (Ascii >= 'A' && Ascii <= 'Z')) {
        Ascii = Ascii - 'A' + 1;
    }
    
    return Ascii;
}

// ============================================================================
// KEY EVENT PROCESSING
// ============================================================================

static VOID ProcessKeyEvent(UINT8 ScanCode, BOOL Pressed, UINT8 Modifiers) {
    CHAR Ascii = 0;
    
    // Getting ASCII from the table
    Ascii = ScanCodeToAscii(ScanCode, Pressed, Modifiers);
    
    // Create event
    KeyEvent Event;
    Event.ScanCode = ScanCode;
    Event.Ascii = Ascii;
    Event.RawScanCode = ScanCode;
    Event.Extended = FALSE;
    Event.State = Pressed ? KEY_STATE_PRESSED : KEY_STATE_RELEASED;
    Event.Modifiers = Modifiers;
    Event.Timestamp = TimerTicks();
    
    GPs2.EventCount++;
    Ps2KeyQueuePush(&Event);
}

// ============================================================================
// SCAN BYTE PROCESSING (SET 1 через аппаратную трансляцию)
// ============================================================================

VOID Ps2HandleScanByte(UINT8 Byte) {
    if (Byte == 0xE0) {
        GPs2.ExtendedPrefix = TRUE;
        return;
    }
    
    // Ignore E1 (Pause sequence)
    if (Byte == 0xE1) {
        return;
    }
    
    BOOL Pressed = !(Byte & 0x80);
    UINT8 RawCode = Byte & 0x7F;
    BOOL Extended = GPs2.ExtendedPrefix;
    GPs2.ExtendedPrefix = FALSE;
    
    // Updating modifiers
    UpdateModifiers(RawCode, Pressed);
    
    // Processing the event
    ProcessKeyEvent(RawCode, Pressed, GPs2.Modifiers);
}

// ============================================================================
// IRQ HANDLER
// ============================================================================

VOID Ps2KeyboardHandleIRQ(VOID) {
    while (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        UINT8 Byte = Inb(PS2_PORT_DATA);
        Ps2HandleScanByte(Byte);
    }
    
    ApicEoi();
    Ps2KeyboardDispatchEvents();
}

// ============================================================================
// PUBLIC API
// ============================================================================

BOOL Ps2KeyboardPollEvent(KeyEvent *Event) {
    if (!Event) return FALSE;
    return Ps2KeyQueuePop(Event);
}

BOOL Ps2KeyboardIsPressed(UINT8 ScanCode) {
    if (ScanCode >= 256) return FALSE;
    return GPs2.KeyStates[ScanCode];
}

UINT8 Ps2KeyboardGetModifiers(VOID) {
    return GPs2.Modifiers;
}

UINT8 Ps2KeyboardGetScanCodeSet(VOID) {
    return 1;
}

INT Ps2KeyboardSetLayout(KeyboardLayout Layout) {
    if (Layout >= KB_LAYOUT_MAX) RETURN(INCORRECT_VALUE);
    GPs2.Layout = Layout;
    RETURN(SUCCESS);
}

KeyboardLayout Ps2KeyboardGetLayout(VOID) {
    return GPs2.Layout;
}

INT Ps2KeyboardSubscribe(KeyboardCallback cb, VOID *ud) {
    if (!cb) RETURN(NO_OBJECT);
    
    for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
        if (!GPs2.Subscribers[i].Active) {
            GPs2.Subscribers[i].Callback = cb;
            GPs2.Subscribers[i].UserData = ud;
            GPs2.Subscribers[i].Active = TRUE;
            RETURN(SUCCESS);
        }
    }
    RETURN(NO_MEMORY);
}

INT Ps2KeyboardUnsubscribe(KeyboardCallback cb) {
    if (!cb) RETURN(NO_OBJECT);
    
    BOOL found = FALSE;
    for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
        if (GPs2.Subscribers[i].Active && GPs2.Subscribers[i].Callback == cb) {
            GPs2.Subscribers[i].Callback = NULLPTR;
            GPs2.Subscribers[i].UserData = NULLPTR;
            GPs2.Subscribers[i].Active = FALSE;
            found = TRUE;
        }
    }
    return found ? SUCCESS : NOT_FOUND;
}

INT Ps2KeyboardDispatchEvents(VOID) {
    KeyEvent Event;
    INT Count = 0;
    
    while (Ps2KeyQueuePop(&Event)) {
        for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
            if (GPs2.Subscribers[i].Active && GPs2.Subscribers[i].Callback) {
                GPs2.Subscribers[i].Callback(&Event, GPs2.Subscribers[i].UserData);
            }
        }
        Count++;
    }
    RETURN(Count);
}

static VOID Ps2KeyboardShutdown(KDriver *Driver) {
    (VOID)Driver;
    
    if (GPs2.Enabled) {
        Ps2KeyboardDisable();
        GPs2.Enabled = FALSE;
    }
    Ps2KeyboardResetModifiers();
    GPs2.Initialized = FALSE;
}

// ============================================================================
// KEYBOARD INITIALIZATION FUNCTIONS
// ============================================================================

static BOOL Ps2KeyboardDisable(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: disabling...\n");
    Ps2KeyboardFlushBuffer();
    TimerMdelay(5);
    return Ps2KeyboardSendCommand(0xF5);
}

static BOOL Ps2KeyboardEnable(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: enabling...\n");
    Ps2KeyboardFlushBuffer();
    TimerMdelay(5);
    return Ps2KeyboardSendCommand(0xF4);
}

static BOOL Ps2KeyboardReset(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: resetting...\n");
    
    Ps2KeyboardFlushBuffer();
    TimerMdelay(10);
    
    if (!Ps2KeyboardSendCommand(0xFF)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: reset command failed\n");
        return FALSE;
    }
    
    INT Timeout = 500;
    UINT8 Response = 0;
    BOOL GotBat = FALSE;
    
    while (Timeout--) {
        if (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
            Response = Inb(PS2_PORT_DATA);
            if (Response == 0xAA) {
                GotBat = TRUE;
                break;
            } else if (Response == 0xFC) {
                return FALSE;
            }
        }
        TimerMdelay(1);
    }
    
    if (!GotBat) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: BAT timeout\n");
        return FALSE;
    }
    
    TimerMdelay(10);
    Ps2KeyboardFlushBuffer();
    Ps2KeyboardResetModifiers();
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: reset complete\n");
    return TRUE;
}

static UINT8 Ps2KeyboardQueryScanCodeSet(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: querying scancode set...\n");
    Ps2KeyboardFlushBuffer();
    TimerMdelay(5);
    
    if (!Ps2KeyboardSendCommand(0xF0)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: query command failed\n");
        return 0;
    }
    
    if (!Ps2KeyboardWaitWrite(100)) return 0;
    Outb(PS2_PORT_DATA, 0);
    TimerMdelay(2);
    
    UINT8 Response = 0;
    if (!Ps2KeyboardReadDataWithTimeout(&Response, 200)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: query response timeout\n");
        return 0;
    }
    
    UINT8 SetId = 0;
    if (!Ps2KeyboardReadDataWithTimeout(&SetId, 200)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: query set id timeout\n");
        return 0;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: scancode set ID = 0x%02X\n", SetId);
    return SetId;
}

static BOOL Ps2KeyboardSetScanCodeSet(UINT8 Set) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: setting scancode set to %d...\n", Set);
    Ps2KeyboardFlushBuffer();
    TimerMdelay(5);
    
    if (!Ps2KeyboardSendCommand(0xF0)) return FALSE;
    
    if (!Ps2KeyboardWaitWrite(100)) return FALSE;
    Outb(PS2_PORT_DATA, Set);
    TimerMdelay(2);
    
    UINT8 Response = 0;
    if (!Ps2KeyboardReadDataWithTimeout(&Response, 200)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: set scancode timeout\n");
        return FALSE;
    }
    
    return Response == 0xFA;
}

// ============================================================================
// INITIALIZATION
// ============================================================================

EXTERN(VOID, Ps2KeyboardIrq());

INT Ps2KeyboardInit(VOID) {
    UINT32 Gsi, Flags;
    INT Result;
    
    if (GPs2.Initialized) {
        RETURN(SUCCESS);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: initializing...\n");
    
    Ps2Controller *Ctrl = Ps2GetController();
    if (!Ctrl || !Ctrl->Initialized) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: controller not initialized!\n");
        RETURN(NO_OBJECT);
    }
    
    if (!Ctrl->Port1.Present) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: port 1 not present!\n");
        RETURN(NO_OBJECT);
    }
    
    MemSet(&GPs2, 0, sizeof(Ps2KeyboardState));
    MemSet(&GPs2KeyQueue, 0, sizeof(GPs2KeyQueue));
    GPs2.Layout = KB_LAYOUT_US;
    GPs2.Modifiers = 0;
    
    TimerMdelay(100);

    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: disabling translation for setup...\n");
    UINT8 Config = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
    Config &= ~PS2_CONFIG_PORT1_TRANS;
    Ps2WriteWithCommand(PS2_CMD_WRITE_CONFIG, Config);
    TimerMdelay(20);
    
    // Disable the keyboard
    if (!Ps2KeyboardDisable()) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: disable failed (continuing)\n");
    }
    TimerMdelay(20);
    
    // Reset keyboard
    if (!Ps2KeyboardReset()) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: reset failed!\n");
        RETURN(DEVICE_ERROR);
    }
    TimerMdelay(50);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: reset OK\n");
    
    // INSTALL SET 2
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: setting keyboard to Set 2...\n");
    if (!Ps2KeyboardSetScanCodeSet(2)) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: failed to set Set 2!\n");
        RETURN(DEVICE_ERROR);
    }
    TimerMdelay(50);
    
    UINT8 Check = Ps2KeyboardQueryScanCodeSet();
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: scancode set = 0x%02X\n", Check);
    
    if (Check != 0x02 && Check != 0x42) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: keyboard not in Set 2!\n");
        RETURN(DEVICE_ERROR);
    }
    
    TimerMdelay(20);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: enabling hardware translation (Set 2 -> Set 1)...\n");
    Config = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
    Config |= PS2_CONFIG_PORT1_TRANS;
    Ps2WriteWithCommand(PS2_CMD_WRITE_CONFIG, Config);
    TimerMdelay(20);
    
    // Enable keyboard
    if (!Ps2KeyboardEnable()) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: enable failed!\n");
        RETURN(DEVICE_ERROR);
    }
    TimerMdelay(10);
    
    // Check final config
    Config = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
    if (Config & PS2_CONFIG_PORT1_TRANS) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: hardware translation ENABLED (Set 1 mode)\n");
    } else {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: hardware translation NOT enabled!\n");
    }
    
    GPs2.Enabled = TRUE;
    GPs2.Initialized = TRUE;
    
    // Register driver
    GPs2.Driver = KDriverGenerateStruct("PS2Keyboard", 0, TRUE, &GPs2, Ps2KeyboardShutdown);
    if (GPs2.Driver) {
        KDriverRegister(GPs2.Driver);
    }
    
    // Setting up IRQ
    Result = IoapicGetOverride(1, &Gsi, &Flags);
    if (IsError(Result).IsError) {
        Gsi = 1;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    GPs2.Gsi = Gsi;
    IdtSetGate(KEYBOARD, (VOID(*)())Ps2KeyboardIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    
    Result = IoapicRedirectIrq(Gsi, KEYBOARD, ApicGetId(), Flags);
    if (IsError(Result).IsError) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2kbd: ioapic redirect failed!\n");
        RETURN(INCORRECT_VALUE);
    }
    
    IoapicUnmaskIrq(Gsi);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: irq: gsi=%d, vector=%d\n", Gsi, KEYBOARD);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2kbd: initialized successfully! (Set 1 mode via hardware translation)\n");
    
    RETURN(SUCCESS);
}
