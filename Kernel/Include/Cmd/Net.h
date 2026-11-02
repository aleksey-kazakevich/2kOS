#pragma once

#include <Types.h>

/*
 * ============================================================================
 * Network shell commands
 *
 * Registered through the normal Cmd system (CmdRegisterBuiltin), so the
 * command names and semantics follow the same conventions as diskmgr,
 * mount, sysdev, etc.
 *
 * The single entry point is "net <subcommand> [args]". Subcommands are:
 *
 *   net                              list interfaces (same as "net ifconfig")
 *   net ifconfig                     list all interfaces
 *   net ifconfig <iface>             show one interface
 *   net up <iface>                   bring an interface up
 *   net down <iface>                 bring an interface down
 *   net set <iface> <ip> <mask> <gw> configure statically
 *   net route                        list routes
 *   net arp                          list ARP cache
 *   net arp add <ip> <mac>           add a static ARP entry
 *   net arp del <ip>                 remove an ARP entry
 *   net ping <ip> [count]            ICMP echo
 *   net udp <ip> <port> [payload]    send a single UDP datagram
 *   net dns <name>                   resolve a hostname
 *   net dhcp [<iface>]               start a DHCP client
 *   net help                         show this text
 * ============================================================================
 */

INT NetCmdRegister(VOID);