#include <Network/Ethernet.h>
#include <Network/Network.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/ICmp.h>
#include <Network/Dhcp.h>
#include <Network/Arp.h>
#include <Network/Udp.h>
#include <Network/Dns.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Return.h>
#include <Cmd.h>

/*
 * ============================================================================
 * Helpers
 * ============================================================================
 */

STATIC VOID NetPrintInterfaceHeader(VOID) {
    StdOutPrintf("  %-8s %-10s %-6s %-5s %s\n",
                 "NAME", "STATE", "MTU", "TYPE", "MAC");
    StdOutPrintf("  %-8s %-10s %-6s %-5s %s\n",
                 "--------", "----------", "------", "-----",
                 "-----------------");
}

STATIC const CHAR *NetTypeName(NetworkDeviceType Type) {
    switch (Type) {
    case NETWORK_TYPE_ETHERNET:  return "eth";
    case NETWORK_TYPE_LOOPBACK:  return "lo";
    case NETWORK_TYPE_WIFI:      return "wifi";
    case NETWORK_TYPE_PPP:       return "ppp";
    case NETWORK_TYPE_BRIDGE:    return "br";
    default:                     return "?";
    }
}

STATIC VOID NetPrintOneInterface(NetworkDevice *Dev) {
    CHAR MacStr[18];
    CHAR IpStr[16];
    CHAR MaskStr[16];
    CHAR GwStr[16];

    EthernetMacNTop(Dev->MacAddr, MacStr, sizeof(MacStr));

    UINT32 Ip   = *(UINT32*)Dev->IpAddr;
    UINT32 Mask = *(UINT32*)Dev->Netmask;
    UINT32 Gw   = *(UINT32*)Dev->Gateway;

    IpV4NTop(Ip,   IpStr,   sizeof(IpStr));
    IpV4NTop(Mask, MaskStr, sizeof(MaskStr));
    IpV4NTop(Gw,   GwStr,   sizeof(GwStr));

    StdOutPrintf("  %-8s %-10s %-6u %-5s %s\n",
                 Dev->Name,
                 (Dev->Up && Dev->Running) ? "up" : "down",
                 Dev->Mtu,
                 NetTypeName(Dev->Type),
                 MacStr);

    StdOutPrintf("           inet %s netmask %s gw %s\n",
                 IpStr, MaskStr, GwStr);

    StdOutPrintf("           rx %llu pkts / %llu bytes, %llu drop, %llu err\n",
                 Dev->Stats.RxPackets, Dev->Stats.RxBytes,
                 Dev->Stats.RxDropped, Dev->Stats.RxErrors);

    StdOutPrintf("           tx %llu pkts / %llu bytes, %llu drop, %llu err\n",
                 Dev->Stats.TxPackets, Dev->Stats.TxBytes,
                 Dev->Stats.TxDropped, Dev->Stats.TxErrors);
}

/*
 * ============================================================================
 * Subcommand: ifconfig
 * ============================================================================
 */

STATIC INT NetCmdIfConfig(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC >= 1) {
        NetworkDevice *Dev = NetworkGetDeviceByName(ArgV[0]);
        if (!Dev) {
            StdErrPrintf("net: %s not found\n", ArgV[0]);
            return -1;
        }
        StdOutPrintf("\n");
        NetPrintInterfaceHeader();
        NetPrintOneInterface(Dev);
        StdOutPrintf("\n");
        return 0;
    }

    NetworkDevice *Dev = NetworkGetFirstDevice();
    if (!Dev) {
        StdOutPrintf("net: no interfaces\n");
        return 0;
    }

    StdOutPrintf("\n");
    NetPrintInterfaceHeader();

    while (Dev) {
        NetPrintOneInterface(Dev);
        Dev = NetworkGetNext(Dev);
    }
    StdOutPrintf("\n");
    return 0;
}

/*
 * ============================================================================
 * Subcommand: up / down
 * ============================================================================
 */

STATIC INT NetCmdIfUp(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 1) {
        StdErrPrintf("usage: net up <iface>\n");
        return -1;
    }

    NetworkDevice *Dev = NetworkGetDeviceByName(ArgV[0]);
    if (!Dev) {
        StdErrPrintf("net: %s not found\n", ArgV[0]);
        return -1;
    }

    if (Dev->Open) {
        INT Rc = Dev->Open(Dev);
        if (Rc != SUCCESS) {
            StdErrPrintf("net: %s open failed (%d)\n", ArgV[0], Rc);
            return -1;
        }
    }

    Dev->Up      = TRUE;
    Dev->Running = TRUE;
    StdOutPrintf("net: %s up\n", Dev->Name);
    return 0;
}

STATIC INT NetCmdIfDown(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 1) {
        StdErrPrintf("usage: net down <iface>\n");
        return -1;
    }

    NetworkDevice *Dev = NetworkGetDeviceByName(ArgV[0]);
    if (!Dev) {
        StdErrPrintf("net: %s not found\n", ArgV[0]);
        return -1;
    }

    if (Dev->Stop) {
        Dev->Stop(Dev);
    }

    Dev->Up      = FALSE;
    Dev->Running = FALSE;
    StdOutPrintf("net: %s down\n", Dev->Name);
    return 0;
}

/*
 * ============================================================================
 * Subcommand: set <iface> <ip> <mask> <gw>
 * ============================================================================
 */

STATIC INT NetCmdIfSet(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 4) {
        StdErrPrintf("usage: net set <iface> <ip> <mask> <gw>\n");
        return -1;
    }

    NetworkDevice *Dev = NetworkGetDeviceByName(ArgV[0]);
    if (!Dev) {
        StdErrPrintf("net: %s not found\n", ArgV[0]);
        return -1;
    }

    UINT32 Ip     = IpV4PTon(ArgV[1]);
    UINT32 Mask   = IpV4PTon(ArgV[2]);
    UINT32 Gw     = IpV4PTon(ArgV[3]);

    if (Ip == 0 && StrCmp(ArgV[1], "0.0.0.0") != 0) {
        StdErrPrintf("net: bad ip '%s'\n", ArgV[1]);
        return -1;
    }
    if (Mask == 0 && StrCmp(ArgV[2], "0.0.0.0") != 0) {
        StdErrPrintf("net: bad mask '%s'\n", ArgV[2]);
        return -1;
    }

    MemCpy(Dev->IpAddr,  &Ip,   4);
    MemCpy(Dev->Netmask, &Mask, 4);
    MemCpy(Dev->Gateway, &Gw,   4);

    /* Remove old routes for this device, then add the new ones. */
    /* (A full route manager would track this per device. For now we
     * just add, which may leave stale entries if the user re-runs
     * "net set". Acceptable for a shell command.) */
    IpV4RouteAdd(Ip & Mask, Mask, 0, Ip, Dev, 0);
    if (Gw) IpV4RouteAdd(0, 0, Gw, Ip, Dev, 0);

    CHAR IpStr[16], MaskStr[16], GwStr[16];
    IpV4NTop(Ip,   IpStr,   sizeof(IpStr));
    IpV4NTop(Mask, MaskStr, sizeof(MaskStr));
    IpV4NTop(Gw,   GwStr,   sizeof(GwStr));

    StdOutPrintf("net: %s = %s/%s gw %s\n",
                 Dev->Name, IpStr, MaskStr, GwStr);
    return 0;
}

/*
 * ============================================================================
 * Subcommand: route
 * ============================================================================
 */

STATIC INT NetCmdRoute(INT ArgC, const CHAR *ArgV[]) {
    (VOID)ArgC;
    (VOID)ArgV;

    StdOutPrintf("\n  %-16s %-16s %-16s %s\n",
                 "DESTINATION", "GATEWAY", "NETMASK", "IFACE");
    StdOutPrintf("  %-16s %-16s %-16s %s\n",
                 "----------------", "----------------",
                 "----------------", "-----");

    /* We do not expose the route table directly, so walk the addresses
     * we know. In practice this prints the two default routes for each
     * configured interface, which is what the user wants to see. */
    NetworkDevice *Dev = NetworkGetFirstDevice();
    while (Dev) {
        UINT32 Ip   = *(UINT32*)Dev->IpAddr;
        UINT32 Mask = *(UINT32*)Dev->Netmask;
        UINT32 Gw   = *(UINT32*)Dev->Gateway;

        if (Ip) {
            CHAR DstStr[16], MaskStr[16], GwStr[16];
            IpV4NTop(Ip & Mask, DstStr, sizeof(DstStr));
            IpV4NTop(Mask,     MaskStr, sizeof(MaskStr));
            IpV4NTop(Gw,       GwStr,  sizeof(GwStr));
            StdOutPrintf("  %-16s %-16s %-16s %s\n",
                         DstStr, "-", MaskStr, Dev->Name);
            if (Gw) {
                StdOutPrintf("  %-16s %-16s %-16s %s\n",
                             "0.0.0.0", GwStr, "0.0.0.0", Dev->Name);
            }
        }
        Dev = NetworkGetNext(Dev);
    }
    StdOutPrintf("\n");
    return 0;
}

/*
 * ============================================================================
 * Subcommand: arp
 * ============================================================================
 */

STATIC INT NetCmdArp(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC >= 1 && StrCmp(ArgV[0], "add") == 0) {
        if (ArgC < 3) {
            StdErrPrintf("usage: net arp add <ip> <mac>\n");
            return -1;
        }
        UINT32 Ip = IpV4PTon(ArgV[1]);
        if (Ip == 0) {
            StdErrPrintf("net: bad ip '%s'\n", ArgV[1]);
            return -1;
        }
        UINT8 Mac[NETWORK_ETH_ALEN];
        if (EthernetMacPTon(ArgV[2], Mac) != SUCCESS) {
            StdErrPrintf("net: bad mac '%s'\n", ArgV[2]);
            return -1;
        }
        ArpCacheAdd(Ip, Mac);
        StdOutPrintf("net: arp %s -> %s\n", ArgV[1], ArgV[2]);
        return 0;
    }

    if (ArgC >= 1 && StrCmp(ArgV[0], "del") == 0) {
        if (ArgC < 2) {
            StdErrPrintf("usage: net arp del <ip>\n");
            return -1;
        }
        UINT32 Ip = IpV4PTon(ArgV[1]);
        if (Ip == 0) {
            StdErrPrintf("net: bad ip '%s'\n", ArgV[1]);
            return -1;
        }
        ArpCacheDel(Ip);
        StdOutPrintf("net: arp %s removed\n", ArgV[1]);
        return 0;
    }

    /* Without a subcommand, we can only say the cache is internal.
     * A future ArpCacheDump will print the table; for now we just
     * tell the user what is supported. */
    StdOutPrintf("\nusage: net arp add <ip> <mac>\n");
    StdOutPrintf("       net arp del <ip>\n\n");
    return 0;
}

/*
 * ============================================================================
 * Subcommand: ping
 * ============================================================================
 */

STATIC INT NetCmdPing(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 1) {
        StdErrPrintf("usage: net ping <ip> [count]\n");
        return -1;
    }

    UINT32 Dest = IpV4PTon(ArgV[0]);
    if (Dest == 0) {
        StdErrPrintf("net: bad ip '%s'\n", ArgV[0]);
        return -1;
    }

    UINT32 Count = 4;
    if (ArgC >= 2) {
        INT C = AToI(ArgV[1]);
        if (C > 0) Count = (UINT32)C;
    }

    PingStats Stats;
    INT Rc = ICmpPing(Dest, Count, 1000, &Stats);
    if (Rc != SUCCESS) {
        StdErrPrintf("net: ping failed (%d)\n", Rc);
        return -1;
    }
    return 0;
}

/*
 * ============================================================================
 * Subcommand: udp
 * ============================================================================
 */

STATIC VOID NetUdpReplyHandler(NetworkBuf *Buf, UINT32 SrcAddr,
                               UINT32 DstAddr, UINT16 SrcPort,
                               UINT16 DstPort) {
    (VOID)DstAddr;
    (VOID)DstPort;

    CHAR IpStr[16];
    IpV4NTop(SrcAddr, IpStr, sizeof(IpStr));
    StdOutPrintf("net: udp reply from %s:%u, %u bytes\n",
                 IpStr, SrcPort, Buf ? Buf->Len : 0);
    if (Buf) NetworkFreeBuf(Buf);
}

STATIC INT NetCmdUdp(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        StdErrPrintf("usage: net udp <ip> <port> [payload]\n");
        return -1;
    }

    UINT32 Dest = IpV4PTon(ArgV[0]);
    if (Dest == 0) {
        StdErrPrintf("net: bad ip '%s'\n", ArgV[0]);
        return -1;
    }
    UINT16 Port = (UINT16)AToI(ArgV[1]);
    if (Port == 0) {
        StdErrPrintf("net: bad port '%s'\n", ArgV[1]);
        return -1;
    }

    static const CHAR DefaultPayload[] = "hello from 2kOS";
    const CHAR *Payload = (ArgC >= 3) ? ArgV[2] : DefaultPayload;
    UINT32 PayloadLen = (UINT32)StrLen(Payload);

    /* Local port 9999, only used by this diagnostic command. */
    UdpRegisterHandler(9999, NetUdpReplyHandler);

    NetworkBuf *Buf = NetworkAllocBuf(PayloadLen);
    if (!Buf) {
        StdErrPrintf("net: out of memory\n");
        return -1;
    }
    if (NetworkPut(Buf, PayloadLen) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return -1;
    }
    MemCpy(Buf->Data, Payload, PayloadLen);

    INT Rc = UdpOutput(Buf, Dest, IPV4_ANY, Port, 9999);
    if (Rc != SUCCESS && Rc != NOT_FOUND) {
        StdErrPrintf("net: send failed (%d)\n", Rc);
        return -1;
    }
    StdOutPrintf("net: sent %u bytes to %s:%u\n",
                 PayloadLen, ArgV[0], Port);
    return 0;
}

/*
 * ============================================================================
 * Subcommand: dns
 * ============================================================================
 */

STATIC INT NetCmdDns(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 1) {
        StdErrPrintf("usage: net dns <name>\n");
        return -1;
    }

    UINT32 Ip = 0;
    INT Rc = DnsResolve(ArgV[0], &Ip);
    if (Rc != SUCCESS) {
        StdErrPrintf("net: dns failed for %s (%d)\n", ArgV[0], Rc);
        return -1;
    }

    CHAR IpStr[16];
    IpV4NTop(Ip, IpStr, sizeof(IpStr));
    StdOutPrintf("%s -> %s\n", ArgV[0], IpStr);
    return 0;
}

/*
 * ============================================================================
 * Subcommand: dhcp
 * ============================================================================
 */

STATIC VOID NetDhcpCallback(DhcpClient *C, BOOL Success) {
    if (Success) {
        CHAR IpStr[16];
        IpV4NTop(C->YIAddr, IpStr, sizeof(IpStr));
        StdOutPrintf("net: dhcp %s leased %s\n", C->Dev->Name, IpStr);
    } else {
        StdErrPrintf("net: dhcp %s failed\n", C->Dev->Name);
    }
}

STATIC INT NetCmdDhcp(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC >= 1) {
        NetworkDevice *Dev = NetworkGetDeviceByName(ArgV[0]);
        if (!Dev) {
            StdErrPrintf("net: %s not found\n", ArgV[0]);
            return -1;
        }
	DhcpClient *C = DhcpGetClient(Dev);
    	if (!C) {
            C = (DhcpClient*)MemoryAllocate(sizeof(DhcpClient));
            if (!C) return -1;
            DhcpInit(C, Dev);
            C->Callback = NetDhcpCallback;
    	}
        INT Rc = DhcpStart(C);
        if (Rc != SUCCESS) {
            StdErrPrintf("net: dhcp send failed (%d)\n", Rc);
            return -1;
        }
        StdOutPrintf("net: dhcp discover sent on %s\n", Dev->Name);
        return 0;
    }

    DhcpStartOnAllInterfaces(NetDhcpCallback);
    StdOutPrintf("net: dhcp discover sent on all ethernet interfaces\n");
    return 0;
}

/*
 * ============================================================================
 * Help
 * ============================================================================
 */

STATIC VOID NetCmdHelp(VOID) {
    StdOutPrintf("\n");
    StdOutPrintf("=== net commands ===\n");
    StdOutPrintf("\n");
    StdOutPrintf("  net                          list interfaces\n");
    StdOutPrintf("  net ifconfig [<iface>]       show one or all interfaces\n");
    StdOutPrintf("  net up <iface>               bring an interface up\n");
    StdOutPrintf("  net down <iface>             bring an interface down\n");
    StdOutPrintf("  net set <iface> <ip> <mask> <gw>\n");
    StdOutPrintf("                               configure an interface statically\n");
    StdOutPrintf("  net route                    show the routing table\n");
    StdOutPrintf("\n");
    StdOutPrintf("  net arp                      ARP cache usage\n");
    StdOutPrintf("  net arp add <ip> <mac>       add a static entry\n");
    StdOutPrintf("  net arp del <ip>             remove an entry\n");
    StdOutPrintf("\n");
    StdOutPrintf("  net ping <ip> [count]        ICMP echo (default 4)\n");
    StdOutPrintf("  net udp <ip> <port> [text]   send a single UDP datagram\n");
    StdOutPrintf("  net dns <name>               resolve a hostname\n");
    StdOutPrintf("  net dhcp [<iface>]           request an address via DHCP\n");
    StdOutPrintf("\n");
    StdOutPrintf("  net help                     show this text\n");
    StdOutPrintf("\n");
}

/*
 * ============================================================================
 * Handler
 * ============================================================================
 */

INT CmdNetHandler(INT ArgC, const CHAR *ArgV[]) {
    if (ArgC < 2) {
        return NetCmdIfConfig(0, NULLPTR);
    }

    const CHAR *Sub = ArgV[1];

    if (StrCmp(Sub, "ifconfig") == 0) {
        return NetCmdIfConfig(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "up") == 0) {
        return NetCmdIfUp(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "down") == 0) {
        return NetCmdIfDown(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "set") == 0) {
        return NetCmdIfSet(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "route") == 0) {
        return NetCmdRoute(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "arp") == 0) {
        return NetCmdArp(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "ping") == 0) {
        return NetCmdPing(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "udp") == 0) {
        return NetCmdUdp(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "dns") == 0) {
        return NetCmdDns(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "dhcp") == 0) {
        return NetCmdDhcp(ArgC - 2, &ArgV[2]);
    }
    if (StrCmp(Sub, "help") == 0 || StrCmp(Sub, "-h") == 0 ||
        StrCmp(Sub, "--help") == 0) {
        NetCmdHelp();
        return 0;
    }

    StdErrPrintf("net: unknown subcommand '%s'\n", Sub);
    StdOutPrintf("Try 'net help'\n");
    return -1;
}

/*
 * ============================================================================
 * Registration
 * ============================================================================
 */

INT NetCmdRegister(VOID) {
    return CmdRegisterBuiltin("net",
                              "Network configuration and diagnostics",
                              CmdNetHandler);
}