#include <Drivers/Ps2/Ps2Mouse.h>
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
// Configuration
// ============================================================================

#define PS2_MOUSE_QUEUE_SIZE    64
#define PS2_MOUSE_MAX_SUBSCRIBERS 32

// ============================================================================
// Mouse commands (Linux-like)
// ============================================================================

#define PS2_MOUSE_CMD_SETSCALE11  0xE6
#define PS2_MOUSE_CMD_SETSCALE21  0xE7
#define PS2_MOUSE_CMD_SETRES      0xE8
#define PS2_MOUSE_CMD_GETINFO     0xE9
#define PS2_MOUSE_CMD_SETSTREAM   0xEA
#define PS2_MOUSE_CMD_SETPOLL     0xF0
#define PS2_MOUSE_CMD_POLL        0xEB
#define PS2_MOUSE_CMD_RESET_WRAP  0xEC
#define PS2_MOUSE_CMD_GETID       0xF2
#define PS2_MOUSE_CMD_SETRATE     0xF3
#define PS2_MOUSE_CMD_ENABLE      0xF4
#define PS2_MOUSE_CMD_DISABLE     0xF5
#define PS2_MOUSE_CMD_RESET_DIS   0xF6
#define PS2_MOUSE_CMD_RESET_BAT   0xFF

#define PS2_MOUSE_RET_BAT         0xAA
#define PS2_MOUSE_RET_ID          0x00
#define PS2_MOUSE_RET_ACK         0xFA
#define PS2_MOUSE_RET_NAK         0xFE

// ============================================================================
// Driver State
// ============================================================================

static Ps2MouseState GMouse = {0};
static Ps2MouseSubscriber GSubscribers[PS2_MOUSE_MAX_SUBSCRIBERS] = {0};

static struct {
    MouseEvent Events[PS2_MOUSE_QUEUE_SIZE];
    volatile UINT32 Head;
    volatile UINT32 Tail;
} GMouseQueue = {0};

static BOOL MouseQueuePush(const MouseEvent *Event) {
    UINT32 Next = (GMouseQueue.Head + 1) % PS2_MOUSE_QUEUE_SIZE;
    if (Next == GMouseQueue.Tail) {
        GMouse.ErrorCount++;
        return FALSE;
    }
    GMouseQueue.Events[GMouseQueue.Head] = *Event;
    GMouseQueue.Head = Next;
    return TRUE;
}

static BOOL MouseQueuePop(MouseEvent *Event) {
    if (GMouseQueue.Head == GMouseQueue.Tail) {
        return FALSE;
    }
    *Event = GMouseQueue.Events[GMouseQueue.Tail];
    GMouseQueue.Tail = (GMouseQueue.Tail + 1) % PS2_MOUSE_QUEUE_SIZE;
    return TRUE;
}

// ============================================================================
// Auxiliary functions for working with the port
// ============================================================================

static VOID Ps2MouseFlushBuffer(VOID) {
    INT Timeout = 100;
    while (Timeout--) {
        UINT8 Status = Inb(PS2_PORT_STATUS);
        if (Status & PS2_STATUS_OUTPUT_FULL) {
            Inb(PS2_PORT_DATA);
        } else {
            break;
        }
        TimerMdelay(1);
    }
}

static UINT8 Ps2MouseReadWithTimeout(INT TimeoutMs) {
    while (TimeoutMs--) {
        UINT8 Status = Inb(PS2_PORT_STATUS);
        if (Status & PS2_STATUS_OUTPUT_FULL) {
            return Inb(PS2_PORT_DATA);
        }
        TimerMdelay(1);
    }
    return 0xFF;
}

static VOID Ps2MouseWritePort(UINT8 Data) {
    INT Timeout = 10000;
    while (Timeout--) {
        if (!(Inb(PS2_PORT_STATUS) & PS2_STATUS_INPUT_FULL)) {
            break;
        }
        CpuPause();
    }
    
    Outb(PS2_PORT_COMMAND, 0xD4);
    
    Timeout = 10000;
    while (Timeout--) {
        if (!(Inb(PS2_PORT_STATUS) & PS2_STATUS_INPUT_FULL)) {
            break;
        }
        CpuPause();
    }
    
    Outb(PS2_PORT_DATA, Data);
}

static BOOL Ps2MouseCommand(UINT8 Command, UINT8 *Response, UINT8 ResponseLen) {
    INT Retry = 3;
    
    while (Retry--) {
        Ps2MouseFlushBuffer();
        Ps2MouseWritePort(Command);
        
        // Reading the answers
        for (UINT8 i = 0; i < ResponseLen; i++) {
            UINT8 Byte = Ps2MouseReadWithTimeout(50);
            if (Byte == PS2_MOUSE_RET_NAK) {
                TimerMdelay(1);
                break;  // Resend
            }
            if (Response) {
                Response[i] = Byte;
            } else if (Byte != PS2_MOUSE_RET_ACK) {
                return FALSE;
            }
        }
        
        if (Response || ResponseLen == 0) {
            return TRUE;
        }
    }
    
    return FALSE;
}

static BOOL Ps2MouseCommandWithData(UINT8 Command, UINT8 Data) {
    INT Retry = 3;
    
    while (Retry--) {
        Ps2MouseFlushBuffer();
        
        // Sending a command
        Ps2MouseWritePort(Command);
        UINT8 Ack = Ps2MouseReadWithTimeout(50);
        if (Ack != PS2_MOUSE_RET_ACK) {
            if (Ack == PS2_MOUSE_RET_NAK) {
                TimerMdelay(1);
                continue;
            }
            return FALSE;
        }
        
        // Sending data
        Ps2MouseWritePort(Data);
        Ack = Ps2MouseReadWithTimeout(50);
        if (Ack == PS2_MOUSE_RET_ACK) {
            return TRUE;
        }
        if (Ack == PS2_MOUSE_RET_NAK) {
            TimerMdelay(1);
            continue;
        }
        return FALSE;
    }
    
    return FALSE;
}

static BOOL Ps2MouseCommandRead(UINT8 Command, UINT8 *Response, UINT8 ResponseLen) {
    INT Retry = 3;
    
    while (Retry--) {
        Ps2MouseFlushBuffer();
        
        // 1. Send a command (for example, 0xF2)
        Ps2MouseWritePort(Command);
        
        // 2. We are waiting for the obligatory ACK
        UINT8 Ack = Ps2MouseReadWithTimeout(50);
        if (Ack != PS2_MOUSE_RET_ACK) {
            if (Ack == PS2_MOUSE_RET_NAK) {
                TimerMdelay(1);
                continue; // Resend
            }
            return FALSE;
        }
        
        for (UINT8 i = 0; i < ResponseLen; i++) {
            Response[i] = Ps2MouseReadWithTimeout(50);
        }
        
        return TRUE;
    }
    
    return FALSE;
}


static BOOL Ps2MouseSimpleCommand(UINT8 Command) {
    return Ps2MouseCommand(Command, NULLPTR, 0);
}

// ============================================================================
// Resetting the mouse
// ============================================================================

static BOOL Ps2MouseReset(VOID) {
    UINT8 Response;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: resetting mouse...\n");
    
    // Clearing the buffer before sending the command
    Ps2MouseFlushBuffer();
    
    // 1. Send reset command (0xFF)
    Ps2MouseWritePort(PS2_MOUSE_CMD_RESET_BAT);  
    
    // 2. Wait and check ACK (0xFA)
    Response = Ps2MouseReadWithTimeout(500);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: reset byte 1 (ACK): 0x%02X\n", Response);
    if (Response != PS2_MOUSE_RET_ACK) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: Mouse reset failed: No ACK\n");
        return FALSE;
    }
    
    // 3. Wait and check BAT OK (0xAA)
    Response = Ps2MouseReadWithTimeout(1000); 
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: reset byte 2 (BAT): 0x%02X\n", Response);
    if (Response != PS2_MOUSE_RET_BAT) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: BAT failed (expected 0xAA)\n");
        return FALSE;
    }
    
    // 4. Read Device ID (0x00)
    UINT8 DeviceId = Ps2MouseReadWithTimeout(500);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: device ID: 0x%02X\n", DeviceId);
    
    // Just in case, we clear the buffer from possible garbage
    Ps2MouseFlushBuffer();
    
    return TRUE;
}

// ============================================================================
// Detecting mouse type
// ============================================================================

static INT Ps2MouseDetectType(VOID) {
    UINT8 Param[2];
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: detecting mouse type...\n");
    
    // Reset to defaults before defining
    Ps2MouseSimpleCommand(PS2_MOUSE_CMD_RESET_DIS);
    TimerMdelay(5);
    Ps2MouseFlushBuffer();
    
    Ps2MouseCommandRead(PS2_MOUSE_CMD_GETID, Param, 1);
    UINT8 BaseId = Param[0];
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: base ID: 0x%02X\n", BaseId);
    
    if (BaseId != 0x00 && BaseId != 0x03 && BaseId != 0x04 && BaseId != 0xFF) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: unknown base ID, assuming standard\n");
        return PS2_MOUSE_STANDARD;
    }
    
    // Trying IntelliMouse: 200, 100, 80
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: trying IntelliMouse...\n");
    
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 200);
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 100);
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 80);
    
    // Using the new ID read function again
    Ps2MouseCommandRead(PS2_MOUSE_CMD_GETID, Param, 1);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: IntelliMouse ID: 0x%02X\n", Param[0]);
    
    if (Param[0] == 0x03) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: IntelliMouse detected (wheel)\n");
        return PS2_MOUSE_INTELLIMOUSE;
    }
    
    // Trying Explorer: 200, 200, 80
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: trying Explorer...\n");
    
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 200);
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 200);
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 80);
    
    // We update here too
    Ps2MouseCommandRead(PS2_MOUSE_CMD_GETID, Param, 1);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: Explorer ID: 0x%02X\n", Param[0]);
    
    if (Param[0] == 0x04) {
        // Enable horizontal scrolling
        Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 200);
        Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 80);
        Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 40);
        
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: Explorer mouse detected\n");
        return PS2_MOUSE_EXPLORER;
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: standard PS/2 mouse\n");
    return PS2_MOUSE_STANDARD;
}


// ============================================================================
// Packet processing
// ============================================================================

static VOID ProcessMousePacket(VOID) {
    UINT8 *Packet = GMouse.Packet;
    MouseEvent Event;
    INT RelX, RelY;
    INT RelZ = 0;
    INT RelH = 0;
    
    MemSet(&Event, 0, sizeof(MouseEvent));
    Event.MouseType = GMouse.Type;
    
    // ====== Package validity check ======
    if (!(Packet[0] & 0x08)) {
        GMouse.OutOfSyncCount++;
        if (GMouse.OutOfSyncCount > 5) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: too many sync errors, resetting\n");
            GMouse.State = PS2_MOUSE_STATE_IGNORE;
        }
        return;
    }
    
    // ====== Motion calculation (correct formula from Linux) ======
    RelX = Packet[1] ? Packet[1] - ((Packet[0] << 4) & 0x100) : 0;
    RelY = Packet[2] ? Packet[2] - ((Packet[0] << 3) & 0x100) : 0;
    
    // ====== Buttons ======
    BOOL LeftButton   = (Packet[0] & 0x01) != 0;
    BOOL RightButton  = (Packet[0] & 0x02) != 0;
    BOOL MiddleButton = (Packet[0] & 0x04) != 0;
    BOOL Button4 = FALSE;
    BOOL Button5 = FALSE;
    
    // ====== Processing IntelliMouse/Explorer ======
    if (GMouse.PacketSize >= 4 && GMouse.Type >= PS2_MOUSE_INTELLIMOUSE) {
        switch (Packet[3] & 0xC0) {
            case 0x00:  // No scrolling or normal vertical
            case 0xC0:
                // Signed 4 bit extension for wheel
                RelZ = (INT8)(Packet[3] & 0x0F);
                if (RelZ & 0x08) RelZ -= 16;
                RelZ = -RelZ;  // Invert for standard direction
                
                // Side buttons for Explorer
                if (GMouse.Type >= PS2_MOUSE_EXPLORER) {
                    Button4 = (Packet[3] & 0x10) != 0;
                    Button5 = (Packet[3] & 0x20) != 0;
                }
                break;
                
            case 0x80:  // Vertical scroll Explorer 4.0
                RelZ = (INT8)(Packet[3] & 0x3F);
                if (RelZ & 0x20) RelZ -= 64;
                RelZ = -RelZ;
                break;
                
            case 0x40:  // Horizontal scroll Explorer 4.0
                RelH = (INT8)(Packet[3] & 0x3F);
                if (RelH & 0x20) RelH -= 64;
                RelH = -RelH;
                break;
        }
    }
    
    // ====== Global state update ======
    GMouse.MouseState.LeftButton = LeftButton;
    GMouse.MouseState.RightButton = RightButton;
    GMouse.MouseState.MiddleButton = MiddleButton;
    GMouse.MouseState.Button4 = Button4;
    GMouse.MouseState.Button5 = Button5;
    
    GMouse.MouseState.X += RelX;
    GMouse.MouseState.Y += RelY;
    GMouse.MouseState.Z += RelZ;
    
    // ====== Filling event ======
    Event.DeltaX = RelX;
    Event.DeltaY = RelY;
    Event.DeltaZ = RelZ;
    Event.LeftButton = LeftButton;
    Event.RightButton = RightButton;
    Event.MiddleButton = MiddleButton;
    Event.Button4 = Button4;
    Event.Button5 = Button5;
    
    GMouse.EventCount++;
    GMouse.OutOfSyncCount = 0;
    
    MouseQueuePush(&Event);
}

// ============================================================================
// Handle one byte
// ============================================================================

static VOID Ps2MouseHandleByte(UINT8 Byte) {
    switch (GMouse.State) {
        case PS2_MOUSE_STATE_IGNORE:
            return;
            
        case PS2_MOUSE_STATE_INITIALIZING:
        case PS2_MOUSE_STATE_RESYNCING:
            // Ignoring data during initialization
            return;
            
        case PS2_MOUSE_STATE_ACTIVATED:
            // Hot swap check: 0xAA 0x00
            if (GMouse.PacketIndex == 0 && Byte == PS2_MOUSE_RET_BAT) {
                GMouse.Packet[GMouse.PacketIndex++] = Byte;
                return;
            }
            if (GMouse.PacketIndex == 1 && GMouse.Packet[0] == PS2_MOUSE_RET_BAT) {
                if (Byte == PS2_MOUSE_RET_ID) {
                    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: device reconnected\n");
                    GMouse.State = PS2_MOUSE_STATE_IGNORE;
                    // TODO: reinitialization
                    return;
                }
                // Не 0xAA 0x00 — обрабатываем первый байт как обычный
                GMouse.PacketIndex = 0;
                GMouse.BadByte = GMouse.Packet[0];
            }
            break;
            
        default:
            break;
    }
    
    // ====== Synchronization on the first byte ======
    if (GMouse.PacketIndex == 0) {
        if (!(Byte & 0x08)) {
            // Bit 3 not set - loss of synchronization
            GMouse.BadByte = Byte;
            GMouse.OutOfSyncCount++;
            
            if (GMouse.OutOfSyncCount > 5) {
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: lost sync, bad byte 0x%02X\n", Byte);
                GMouse.State = PS2_MOUSE_STATE_RESYNCING;
            }
            return;
        }
    }
    
    GMouse.Packet[GMouse.PacketIndex++] = Byte;
    
    if (GMouse.PacketIndex >= GMouse.PacketSize) {
        GMouse.PacketIndex = 0;
        ProcessMousePacket();
    }
}

// ============================================================================
// Interrupt handler
// ============================================================================

VOID Ps2MouseHandleIRQ(VOID) {
    UINT8 Byte = Ps2ReadData();
    
    Ps2MouseHandleByte(Byte);
    
    ApicEoi();
    Ps2MouseDispatchEvents();
}

EXTERN(VOID, Ps2MouseIrq(VOID));

// ============================================================================
// Initialization
// ============================================================================

INT Ps2MouseInit(VOID) {
    UINT32 Gsi, Flags;
    INT Result;
    
    if (GMouse.Initialized) {
        RETURN(SUCCESS);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: initializing...\n");
    
    // Checking the controller
    Ps2Controller *Ctrl = Ps2GetController();
    if (!Ctrl || !Ctrl->Initialized) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: controller not initialized!\n");
        RETURN(NO_OBJECT);
    }
    
    if (!Ps2IsMousePresent()) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: mouse not present!\n");
        RETURN(NO_OBJECT);
    }
    
    // Initial state
    MemSet(&GMouse, 0, sizeof(Ps2MouseState));
    GMouse.PacketSize = 3;
    GMouse.PacketIndex = 0;
    GMouse.State = PS2_MOUSE_STATE_INITIALIZING;
    
    // Enable port 2
    Ps2EnablePort2();
    TimerMdelay(10);
    
    // Resetting the mouse
    if (!Ps2MouseReset()) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: reset failed!\n");
        RETURN(DEVICE_ERROR);
    }
    
    // Setting defaults
    Ps2MouseSimpleCommand(PS2_MOUSE_CMD_RESET_DIS);
    TimerMdelay(5);
    Ps2MouseFlushBuffer();
    
    // Determining the type
    GMouse.Type = Ps2MouseDetectType();
    
    switch (GMouse.Type) {
        case PS2_MOUSE_INTELLIMOUSE:
            GMouse.PacketSize = 4;
            break;
        case PS2_MOUSE_EXPLORER:
            GMouse.PacketSize = 4;
            break;
        default:
            GMouse.PacketSize = 3;
            break;
    }
    
    // Setting the resolution
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRES, 2);  // 4 counts/mm
    GMouse.Resolution = 25 << 2;  // 100 DPI
    
    // Scaling 1:1
    Ps2MouseSimpleCommand(PS2_MOUSE_CMD_SETSCALE11);
    
    // Sample rate 100
    Ps2MouseCommandWithData(PS2_MOUSE_CMD_SETRATE, 100);
    GMouse.SampleRate = 100;
    
    // ====== Final cleanup ======
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: final flush...\n");
    Ps2MouseFlushBuffer();
    TimerMdelay(10);
    
    INT FlushedBytes = 0;
    while (Inb(PS2_PORT_STATUS) & PS2_STATUS_OUTPUT_FULL) {
        UINT8 Garbage = Inb(PS2_PORT_DATA);
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: garbage: 0x%02X\n", Garbage);
        FlushedBytes++;
    }
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: flushed %d bytes\n", FlushedBytes);
    
    // Resetting the package index
    GMouse.PacketIndex = 0;
    
    // ====== Enable data flow ======
    if (!Ps2MouseSimpleCommand(PS2_MOUSE_CMD_ENABLE)) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: enable failed!\n");
        RETURN(DEVICE_ERROR);
    }
    
    GMouse.Enabled = TRUE;
    GMouse.State = PS2_MOUSE_STATE_ACTIVATED;
    GMouse.Initialized = TRUE;
    
    // Registering the driver
    GMouse.Driver = KDriverGenerateStruct("PS2Mouse", 0, TRUE, &GMouse, NULLPTR);
    if (GMouse.Driver) {
        KDriverRegister(GMouse.Driver);
    }
    
    // ====== IRQ setup ======
    Result = IoapicGetOverride(12, &Gsi, &Flags);
    if (IsError(Result).IsError) {
        Gsi = 12;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    GMouse.Gsi = Gsi;
    
    IdtSetGate(MOUSE, (VOID(*)())Ps2MouseIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    
    Result = IoapicRedirectIrq(Gsi, MOUSE, ApicGetId(), Flags);
    if (IsError(Result).IsError) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: ioapic redirect failed!\n");
        RETURN(DEVICE_ERROR);
    }
    
    IoapicUnmaskIrq(Gsi);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: irq: gsi=%d, vector=%d\n", Gsi, MOUSE);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: initialized successfully!\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2mouse: type: %s, packetsize=%d, resolution=%d dpi, rate=%d Hz\n",
                  GMouse.Type == PS2_MOUSE_STANDARD ? "Standard" :
                  GMouse.Type == PS2_MOUSE_INTELLIMOUSE ? "IntelliMouse" :
                  GMouse.Type == PS2_MOUSE_EXPLORER ? "Explorer" : "Unknown",
                  GMouse.PacketSize, GMouse.Resolution, GMouse.SampleRate);
    
    RETURN(SUCCESS);
}

INT Ps2MouseSetSampleRate(UINT8 Rate) {
    if (!GMouse.Initialized) RETURN(NO_OBJECT);
    
    if (Ps2MouseCommand(PS2_MOUSE_CMD_SETRATE, &Rate, 1)) {
        GMouse.SampleRate = Rate;
        RETURN(SUCCESS);
    }
    RETURN(DEVICE_ERROR);
}

INT Ps2MouseSetResolution(UINT8 Resolution) {
    if (!GMouse.Initialized) RETURN(NO_OBJECT);
    
    // Convert to mouse format (0=1, 1=2, 2=4, 3=8 counts/mm)
    UINT8 ResByte;
    if (Resolution <= 1) ResByte = 0;
    else if (Resolution <= 2) ResByte = 1;
    else if (Resolution <= 4) ResByte = 2;
    else ResByte = 3;
    
    if (Ps2MouseCommand(PS2_MOUSE_CMD_SETRES, &ResByte, 1)) {
        GMouse.Resolution = 25 << ResByte;
        RETURN(SUCCESS);
    }
    RETURN(DEVICE_ERROR);
}

INT Ps2MouseGetType(VOID) {
    return GMouse.Type;
}

MouseState* Ps2MouseGetState(VOID) {
    return GMouse.Initialized ? &GMouse.MouseState : NULLPTR;
}

BOOL Ps2MouseIsPresent(VOID) {
    return GMouse.Initialized && GMouse.Enabled;
}

INT Ps2MouseSubscribe(MouseCallback Cb, VOID *Ud) {
    if (!Cb) RETURN(NO_OBJECT);
    if (!GMouse.Initialized) RETURN(NO_OBJECT);
    
    for (UINT32 i = 0; i < PS2_MOUSE_MAX_SUBSCRIBERS; i++) {
        if (!GSubscribers[i].Active) {
            GSubscribers[i].Callback = Cb;
            GSubscribers[i].UserData = Ud;
            GSubscribers[i].Active = TRUE;
            RETURN(SUCCESS);
        }
    }
    RETURN(NO_MEMORY);
}

INT Ps2MouseUnsubscribe(MouseCallback Cb) {
    if (!Cb) RETURN(NO_OBJECT);
    
    BOOL found = FALSE;
    for (UINT32 i = 0; i < PS2_MOUSE_MAX_SUBSCRIBERS; i++) {
        if (GSubscribers[i].Active && GSubscribers[i].Callback == Cb) {
            GSubscribers[i].Callback = NULLPTR;
            GSubscribers[i].UserData = NULLPTR;
            GSubscribers[i].Active = FALSE;
            found = TRUE;
        }
    }
    return found ? SUCCESS : NOT_FOUND;
}

INT Ps2MouseDispatchEvents(VOID) {
    MouseEvent Event;
    INT Count = 0;
    
    while (MouseQueuePop(&Event)) {
        for (UINT32 i = 0; i < PS2_MOUSE_MAX_SUBSCRIBERS; i++) {
            if (GSubscribers[i].Active && GSubscribers[i].Callback) {
                GSubscribers[i].Callback(&Event, GSubscribers[i].UserData);
            }
        }
        Count++;
    }
    return Count;
}
