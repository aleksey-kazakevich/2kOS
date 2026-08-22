#pragma once

#include <Types.h>
#include <List.h>

// ============================================================================
// PS/2 ports
// ============================================================================

#define PS2_PORT_DATA    0x60
#define PS2_PORT_STATUS  0x64
#define PS2_PORT_COMMAND 0x64

// ============================================================================
// Controller Commands
// ============================================================================

#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_PORT1  0xAD
#define PS2_CMD_ENABLE_PORT1   0xAE
#define PS2_CMD_DISABLE_PORT2  0xA7
#define PS2_CMD_ENABLE_PORT2   0xA8
#define PS2_CMD_TEST_PORT1     0xAB
#define PS2_CMD_TEST_PORT2     0xA9
#define PS2_CMD_TEST_CONTROLLER 0xAA
#define PS2_CMD_SELF_TEST      0x55
#define PS2_CMD_RESET          0xFF

// ============================================================================
// Controller status
// ============================================================================

#define PS2_STATUS_OUTPUT_FULL   (1 << 0)
#define PS2_STATUS_INPUT_FULL    (1 << 1)
#define PS2_STATUS_SYSTEM        (1 << 2)
#define PS2_STATUS_CMD_DATA      (1 << 3)
#define PS2_STATUS_TIMEOUT       (1 << 4)
#define PS2_STATUS_PARITY_ERROR  (1 << 5)

// ============================================================================
// Controller Configuration (Configuration Byte)
// ============================================================================
// Bit 0: Port 1 Interrupt Enable (1 = enabled)
// Бит 1: Port 2 Interrupt Enable (1 = enabled)
// Бит 2: System Flag (1 = system passed POST)
// Бит 3: Port 1 Clock Override (1 = disabled)
// Bit 4: Port 1 Disable (1 = disabled)
// Bit 5: Port 2 Disable (1 = disabled)
// Бит 6: Port 1 Translation (1 = enabled, AT->XT)
// Бит 7: Reserved (must be 0)

#define PS2_CONFIG_PORT1_INT     (1 << 0)
#define PS2_CONFIG_PORT2_INT     (1 << 1)
#define PS2_CONFIG_SYSTEM        (1 << 2)
#define PS2_CONFIG_PORT1_CLOCK   (1 << 3)  // Clock override
#define PS2_CONFIG_PORT1_DISABLE (1 << 4)  // Disable port 1
#define PS2_CONFIG_PORT2_DISABLE (1 << 5)  // Disable port 2
#define PS2_CONFIG_PORT1_TRANS   (1 << 6)  // Translation
#define PS2_CONFIG_RESERVED      (1 << 7)

// ============================================================================
// Device commands
// ============================================================================

#define PS2_DEV_CMD_RESET        0xFF
#define PS2_DEV_CMD_DISABLE      0xF5
#define PS2_DEV_CMD_ENABLE       0xF4
#define PS2_DEV_CMD_IDENTIFY     0xF2
#define PS2_DEV_CMD_SET_SCANCODE 0xF0
#define PS2_DEV_CMD_SET_RATE     0xF3
#define PS2_DEV_CMD_SET_LEDS     0xED

// ============================================================================
// Device responses
// ============================================================================

#define PS2_RESPONSE_ACK         0xFA
#define PS2_RESPONSE_RESEND      0xFE
#define PS2_RESPONSE_BAT_OK      0xAA
#define PS2_RESPONSE_BAT_ERROR   0xFC

// ============================================================================
// Device types
// ============================================================================

#define PS2_DEVICE_KEYBOARD      0xAB
#define PS2_DEVICE_MOUSE         0x00
#define PS2_DEVICE_MOUSE_WHEEL   0x03
#define PS2_DEVICE_MOUSE_5BTN    0x04
#define PS2_DEVICE_UNKNOWN       0xFF


// ============================================================================
// Structures
// ============================================================================

typedef struct {
    UINT8 Type;           // Device type
    UINT8 Id[2];          // Device ID
    BOOL Present;         // Is the device present?
    BOOL Enabled;         // Is the device turned on?
    UINT8 ScancodeSet;    // Set of scan codes
    UINT8 LedState;       // LED status (for keyboard)
} Ps2Device;

typedef struct {
    BOOL Initialized;
    BOOL DualChannel;     // Two channels or one
    UINT8 Config;         // Controller config
    Ps2Device Port1;      // Device on port 1
    Ps2Device Port2;      // Device on port 2
    BOOL InterruptEnabled;
} Ps2Controller;

INT Ps2Init(VOID);
Ps2Controller* Ps2GetController(VOID);
BOOL Ps2IsKeyboardPresent(VOID);
BOOL Ps2IsMousePresent(VOID);
VOID Ps2SetKeyboardLED(UINT8 LED);
UINT8 Ps2ReadData(VOID);
VOID Ps2WriteData(UINT8 Data);
VOID Ps2SendCommand(UINT8 Command);
VOID Ps2WriteWithCommand(UINT8 Command, UINT8 Data);
UINT8 Ps2ReadWithCommand(UINT8 Command);
VOID Ps2EnablePort1(VOID);
VOID Ps2EnablePort2(VOID);
VOID Ps2DisablePort1(VOID);
VOID Ps2DisablePort2(VOID);
