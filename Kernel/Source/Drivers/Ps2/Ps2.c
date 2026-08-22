#include <Drivers/Ps2/Ps2.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <KDriver.h>
#include <Return.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <Time/Timer.h>
#include <Interrupt.h>

// ============================================================================
// Global data
// ============================================================================

static Ps2Controller GPs2Controller = {0};

static UINT8 Ps2ReadStatus(VOID) {
    return Inb(PS2_PORT_STATUS);
}

static VOID Ps2WaitWrite(VOID) {
    INT Timeout = 100000;
    while (Timeout--) {
        if (!(Ps2ReadStatus() & PS2_STATUS_INPUT_FULL)) {
            return;
        }
        CpuPause();
    }
}

static VOID Ps2WaitRead(VOID) {
    INT Timeout = 100000;
    while (Timeout--) {
        if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
            return;
        }
        CpuPause();
    }
}

UINT8 Ps2ReadData(VOID) {
    Ps2WaitRead();
    return Inb(PS2_PORT_DATA);
}

VOID Ps2WriteData(UINT8 Data) {
    Ps2WaitWrite();
    Outb(PS2_PORT_DATA, Data);
}

VOID Ps2SendCommand(UINT8 Command) {
    Ps2WaitWrite();
    Outb(PS2_PORT_COMMAND, Command);
}

UINT8 Ps2ReadWithCommand(UINT8 Command) {
    Ps2SendCommand(Command);
    return Ps2ReadData();
}

VOID Ps2WriteWithCommand(UINT8 Command, UINT8 Data) {
    Ps2SendCommand(Command);
    Ps2WriteData(Data);
}

static VOID Ps2FlushOutput(VOID) {
    INT Timeout = 100;
    while (Timeout-- && (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL)) {
        Inb(PS2_PORT_DATA);
        CpuPause();
    }
}

static UINT8 Ps2ReadWithTimeout(INT TimeoutMs) {
    while (TimeoutMs--) {
        if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
            return Ps2ReadData();
        }
        TimerMdelay(1);
    }
    return 0xFF;
}

static BOOL Ps2WritePort(UINT8 Port, UINT8 Data) {
    if (Port == 2) {
        Ps2SendCommand(0xD4);
    }
    Ps2WriteData(Data);
    return TRUE;
}

static BOOL Ps2WaitAck(INT TimeoutMs) {
    UINT8 Response = Ps2ReadWithTimeout(TimeoutMs);
    return Response == PS2_RESPONSE_ACK;
}

// ============================================================================
// Device discovery on a port
// ============================================================================

static UINT8 Ps2DetectDevice(UINT8 Port) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: detecting device on port %d...\n", Port);

    if (Port == 2) {
        // Try to enable port 2 in config (may not work on some hardware)
        UINT8 Config = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
        Config &= ~PS2_CONFIG_PORT2_DISABLE;
        Ps2WriteWithCommand(PS2_CMD_WRITE_CONFIG, Config);
        TimerMdelay(10);
    }
    
    // Enable the port
    UINT8 EnableCmd = (Port == 1) ? PS2_CMD_ENABLE_PORT1 : PS2_CMD_ENABLE_PORT2;
    Ps2SendCommand(EnableCmd);
    TimerMdelay(30);
    
    // Clear buffer
    Ps2FlushOutput();
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: buffer flushed\n", Port);
    
    // Send RESET
    Ps2WritePort(Port, PS2_DEV_CMD_RESET);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: RESET sent\n", Port);
    
    // Wait ACK (0xFA)
    UINT8 Response = 0;
    INT Timeout = 500;
    BOOL GotAck = FALSE;
    
    while (Timeout--) {
        if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
            Response = Ps2ReadData();
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: read byte = 0x%02X\n", Port, Response);
            
            if (Response == PS2_RESPONSE_ACK) {
                GotAck = TRUE;
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ACK received\n", Port);
                break;
            } else {
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ignoring garbage 0x%02X\n", Port, Response);
            }
        }
        TimerMdelay(1);
    }
    
    if (!GotAck) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: no ACK, device not present\n", Port);
        return PS2_DEVICE_UNKNOWN;
    }
    
    // Wait BAT (0xAA)
    Response = 0;
    Timeout = 1000;
    BOOL GotBat = FALSE;
    
    while (Timeout--) {
        if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
            Response = Ps2ReadData();
        
            if (Response == PS2_RESPONSE_BAT_OK) {
                GotBat = TRUE;
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: BAT OK (0xAA)\n", Port);
                break;
            } else if (Response == 0xFC) {
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: BAT ERROR (0xFC)\n", Port);
                return PS2_DEVICE_UNKNOWN;
            }
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ignoring 0x%02X, waiting for BAT\n", Port, Response);
        }
        TimerMdelay(1);
    }
    
    if (!GotBat) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: BAT timeout\n", Port);
        return PS2_DEVICE_UNKNOWN;
    }
    
    TimerMdelay(20);
    Ps2FlushOutput();
    
    // Send IDENTIFY
    Ps2WritePort(Port, PS2_DEV_CMD_IDENTIFY);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: IDENTIFY sent\n", Port);
    
    Timeout = 200;
    GotAck = FALSE;
    
    while (Timeout--) {
        if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
            Response = Ps2ReadData();
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: IDENTIFY response = 0x%02X\n", Port, Response);
            
            if (Response == PS2_RESPONSE_ACK) {
                GotAck = TRUE;
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ACK on IDENTIFY\n", Port);
                break;
            } else {
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: expected ACK, got 0x%02X\n", Port, Response);
                break;
            }
        }
        TimerMdelay(1);
    }
    
    // Read ID
    UINT8 Id1 = 0xFF, Id2 = 0xFF;
    
    if (Response != PS2_RESPONSE_ACK) {
        Id1 = Response;
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ID1 = 0x%02X (from response)\n", Port, Id1);
    } else {
        Timeout = 200;
        BOOL GotId = FALSE;
        while (Timeout--) {
            if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
                Id1 = Ps2ReadData();
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ID1 = 0x%02X\n", Port, Id1);
                GotId = TRUE;
                break;
            }
            TimerMdelay(1);
        }
        if (!GotId) {
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ID1 timeout\n", Port);
            return PS2_DEVICE_UNKNOWN;
        }
    }
    
    TimerMdelay(5);
    if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
        Id2 = Ps2ReadData();
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: ID2 = 0x%02X\n", Port, Id2);
    }
    
    Ps2FlushOutput();
    
    // Detect device type
    UINT8 DeviceType = PS2_DEVICE_UNKNOWN;
    
    if (Port == 1) {
        if (Id1 == 0xAB || Id1 == 0x83) {
            DeviceType = PS2_DEVICE_KEYBOARD;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: keyboard detected (ID=0x%02X)\n", Port, Id1);
        } else if (Id1 == 0x00) {
            DeviceType = PS2_DEVICE_MOUSE;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: mouse detected (ID=0x%02X)\n", Port, Id1);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: unknown device (ID=0x%02X)\n", Port, Id1);
        }
    } else {
        if (Id1 == 0x00) {
            DeviceType = PS2_DEVICE_MOUSE;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: standard mouse\n", Port);
        } else if (Id1 == 0x03) {
            DeviceType = PS2_DEVICE_MOUSE_WHEEL;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: wheel mouse\n", Port);
        } else if (Id1 == 0x04) {
            DeviceType = PS2_DEVICE_MOUSE_5BTN;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: 5-button mouse\n", Port);
        } else if (Id1 == 0xAB || Id1 == 0x83) {
            DeviceType = PS2_DEVICE_KEYBOARD;
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: keyboard on port 2 (unusual)\n", Port);
        } else {
            BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: unknown device (ID=0x%02X)\n", Port, Id1);
        }
    }
    
    // Enable the device so it starts sending packets
    if (DeviceType != PS2_DEVICE_UNKNOWN) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: enabling device...\n", Port);
        
        Ps2WritePort(Port, PS2_DEV_CMD_ENABLE);
        TimerMdelay(10);
        
        UINT8 EnableAck = 0;
        INT EnableTimeout = 200;
        while (EnableTimeout--) {
            if (Ps2ReadStatus() & PS2_STATUS_OUTPUT_FULL) {
                EnableAck = Ps2ReadData();
                BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: enable response = 0x%02X\n", Port, EnableAck);
                break;
            }
            TimerMdelay(1);
        }
        
        Ps2FlushOutput();
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port %d: detection complete, device type=0x%02X\n", Port, DeviceType);
    return DeviceType;
}

// ============================================================================
// Controller initialization
// ============================================================================

static INT Ps2ControllerInit(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: initializing controller...\n");
    
    // Disable ports during initialization
    Ps2SendCommand(PS2_CMD_DISABLE_PORT1);
    Ps2SendCommand(PS2_CMD_DISABLE_PORT2);
    Ps2FlushOutput();
    
    UINT8 Config = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: initial config = 0x%02X\n", Config);
    
    // ALWAYS assume second port exists.
    // If there is no device, Ps2DetectDevice(2) will return UNKNOWN.
    GPs2Controller.DualChannel = TRUE;
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: assuming dual channel (port 2 always present)\n");
    
    // Controller test
    Ps2SendCommand(PS2_CMD_TEST_CONTROLLER);
    UINT8 TestResult = Ps2ReadData();
    
    if (TestResult != PS2_CMD_SELF_TEST && TestResult != 0x57) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: controller test failed (0x%02X)\n", TestResult);
        RETURN(DEVICE_ERROR);
    }
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: controller test passed\n");
    
    // Set config
    Config = PS2_CONFIG_SYSTEM;
    Config &= ~(PS2_CONFIG_PORT1_DISABLE | PS2_CONFIG_PORT2_DISABLE);
    Config &= ~PS2_CONFIG_PORT1_CLOCK;
    Config |= PS2_CONFIG_PORT1_TRANS;
    
    Ps2WriteWithCommand(PS2_CMD_WRITE_CONFIG, Config);
    GPs2Controller.Config = Config;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: final config = 0x%02X\n", Config);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: %s channel\n", 
                  GPs2Controller.DualChannel ? "dual" : "single");
    
    RETURN(SUCCESS);
}

// ============================================================================
// Public functions
// ============================================================================

INT Ps2Init(VOID) {
    if (GPs2Controller.Initialized) {
        RETURN(SUCCESS);
    }
    
    MemSet(&GPs2Controller, 0, sizeof(Ps2Controller));
    
    INT Result = Ps2ControllerInit();
    if (Result != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ps2: controller init failed\n");
        RETURN(Result);
    }
    
    // Detect devices
    GPs2Controller.Port1.Type = Ps2DetectDevice(1);
    GPs2Controller.Port1.Present = (GPs2Controller.Port1.Type != PS2_DEVICE_UNKNOWN);
    
    // Always try to detect on port 2
    GPs2Controller.Port2.Type = Ps2DetectDevice(2);
    GPs2Controller.Port2.Present = (GPs2Controller.Port2.Type != PS2_DEVICE_UNKNOWN);
    
    // Enable interrupts
    UINT8 FinalConfig = Ps2ReadWithCommand(PS2_CMD_READ_CONFIG);
    FinalConfig |= PS2_CONFIG_PORT1_INT;
    if (GPs2Controller.DualChannel) {
        FinalConfig |= PS2_CONFIG_PORT2_INT;
    }
    Ps2WriteWithCommand(PS2_CMD_WRITE_CONFIG, FinalConfig);
    
    // Register driver
    KDriverRegister(KDriverGenerateStruct("PS/2", 0, TRUE, &GPs2Controller, NULLPTR));
    
    GPs2Controller.Initialized = TRUE;
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: initialization complete\n");
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port1: %s (type 0x%02X)\n", 
                  GPs2Controller.Port1.Present ? "present" : "empty",
                  GPs2Controller.Port1.Type);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: port2: %s (type 0x%02X)\n", 
                  GPs2Controller.Port2.Present ? "present" : "empty",
                  GPs2Controller.Port2.Type);
    BaseconPrintf(BASECON_TYPE_NORMAL, "ps2: channel: %s\n", 
                  GPs2Controller.DualChannel ? "dual" : "single");
    
    RETURN(SUCCESS);
}

Ps2Controller* Ps2GetController(VOID) {
    return GPs2Controller.Initialized ? &GPs2Controller : NULLPTR;
}

BOOL Ps2IsKeyboardPresent(VOID) {
    return GPs2Controller.Initialized && 
           GPs2Controller.Port1.Present && 
           GPs2Controller.Port1.Type == PS2_DEVICE_KEYBOARD;
}

BOOL Ps2IsMousePresent(VOID) {
    return GPs2Controller.Initialized && 
           ((GPs2Controller.Port1.Present && 
             (GPs2Controller.Port1.Type == PS2_DEVICE_MOUSE ||
              GPs2Controller.Port1.Type == PS2_DEVICE_MOUSE_WHEEL ||
              GPs2Controller.Port1.Type == PS2_DEVICE_MOUSE_5BTN)) ||
            (GPs2Controller.Port2.Present && 
             (GPs2Controller.Port2.Type == PS2_DEVICE_MOUSE ||
              GPs2Controller.Port2.Type == PS2_DEVICE_MOUSE_WHEEL ||
              GPs2Controller.Port2.Type == PS2_DEVICE_MOUSE_5BTN)));
}

VOID Ps2SetKeyboardLED(UINT8 LED) {
    if (!Ps2IsKeyboardPresent()) return;
    
    Ps2SendCommand(PS2_CMD_ENABLE_PORT1);
    TimerMdelay(1);
    
    Ps2WriteData(PS2_DEV_CMD_SET_LEDS);
    if (Ps2ReadWithTimeout(50) == PS2_RESPONSE_ACK) {
        Ps2WriteData(LED);
    }
    
    GPs2Controller.Port1.LedState = LED;
}

VOID Ps2Flush(VOID) {
    Ps2FlushOutput();
}

VOID Ps2EnablePort1(VOID) {
    Ps2SendCommand(PS2_CMD_ENABLE_PORT1);
}

VOID Ps2EnablePort2(VOID) {
    Ps2SendCommand(PS2_CMD_ENABLE_PORT2);
}

VOID Ps2DisablePort1(VOID) {
    Ps2SendCommand(PS2_CMD_DISABLE_PORT1);
}

VOID Ps2DisablePort2(VOID) {
    Ps2SendCommand(PS2_CMD_DISABLE_PORT2);
}
