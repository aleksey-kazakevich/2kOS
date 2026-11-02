#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * Loopback device
 *
 * Registers itself as a normal NetworkDevice named "lo" with address
 * 127.0.0.1/8. Its Xmit simply feeds the buffer back into NetworkRx,
 * which drives the whole stack through Ethernet, IPv4, ICMP, TCP, etc.
 *
 * Useful for testing the stack without any real NIC. Also registered
 * in KDriver and SysDevice so that it shows up in `sysdev tree`.
 * ============================================================================
 */

INT LoopbackInit(VOID);