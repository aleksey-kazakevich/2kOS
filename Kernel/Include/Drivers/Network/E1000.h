#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * Intel 8254x (E1000) driver
 *
 * Targets the PCI variants found in QEMU (-device e1000), VirtualBox, and
 * most real Intel gigabit NICs from the 2000s. Uses the legacy (non-MSI)
 * interrupt path so the driver works on the same IRQ lines that PciInit
 * already discovered, and requires no MSI plumbing to bring up.
 *
 * Design notes:
 *   - One Tx ring and one Rx ring, 32 descriptors each, 2 KB buffers.
 *   - Physical addresses in descriptors come straight from
 *     PhysAllocAllocatePage, so no cache-line aliasing.
 *   - Descriptors are 16-byte aligned as required by the hardware.
 *   - Both rings are in the same page-aligned allocation per descriptor,
 *     because the hardware may write the whole 2 KB buffer at once.
 *   - Interrupts are handled through HalIrq: the PCI IRQ line is resolved
 *     to a GSI, redirected to a fixed vector, and the ISR drains both
 *     rings and re-arms the cause register.
 *
 * Registered in:
 *   - KDriver  as "E1000"
 *   - SysDevice as a SYSDEV_TYPE_NETWORK child of the PCI bus device
 * ============================================================================
 */

#define E1000_VENDOR_INTEL      0x8086
#define E1000_DEVICE_82540EM    0x100E   /* QEMU default */
#define E1000_DEVICE_82545EM    0x100F
#define E1000_DEVICE_82574L     0x10D3
#define E1000_DEVICE_82567LM    0x10F5
#define E1000_DEVICE_82583V     0x150C

#define E1000_TX_RING_SIZE      32
#define E1000_RX_RING_SIZE      32
#define E1000_BUF_SIZE          2048

/*
 * Returned by E1000Init:
 *   SUCCESS       — driver attached, at least one NIC registered
 *   NOT_FOUND     — no supported PCI device present
 *   DEVICE_ERROR  — device found but bring-up failed
 */
INT E1000Init(VOID);