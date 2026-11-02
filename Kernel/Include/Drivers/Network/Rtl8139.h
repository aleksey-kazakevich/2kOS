#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * Realtek RTL8139 driver
 *
 * Targets the ubiquitous 10/100 NIC found in QEMU (-device rtl8139),
 * VirtualBox, VMware, and most consumer PCs from ~1998 to ~2005.
 *
 * Differences from E1000:
 *   - I/O port based, not MMIO. All register access is Inb/Inw/Outb/Outw,
 *     so no virtual mapping is needed for the control path.
 *   - DMA rings live in physically contiguous memory; the Rx ring is a
 *     circular buffer, not a descriptor array.
 *   - The MAC address is read from the IDR0..IDR5 registers.
 *   - One interrupt line, one vector. The ISR dispatches on the ISR
 *     register, not on a cause register per ring.
 *
 * Registered in:
 *   - KDriver  as "Rtl8139"
 *   - SysDevice as a SYSDEV_TYPE_NETWORK child of PCI0
 * ============================================================================
 */

#define RTL8139_VENDOR_REALTEK   0x10EC
#define RTL8139_DEVICE_8139      0x8139
#define RTL8139_DEVICE_8139D     0x8139   /* same silicon, different label */

INT Rtl8139Init(VOID);