#include <Mem/Allocator.h>
#include <Network/Dhcp.h>
#include <Network/IpV4.h>
#include <Network/Dns.h>
#include <Network/Udp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SpinLock.h>
#include <Return.h>

STATIC DhcpClient *GDhcpClients = NULLPTR;
STATIC SpinLock    GDhcpLock;

/*
 * ============================================================================
 * Helpers
 * ============================================================================
 */

STATIC UINT32 DhcpNewXid(VOID) {
    UINT64 Tsc = ReadTimeStampCounter();
    return (UINT32)Tsc ^ (UINT32)(Tsc >> 32);
}

STATIC UINT64 DhcpNowSeconds(VOID) {
    UINT32 Tpm = TimerTicksPerMs();
    if (Tpm == 0) Tpm = 1;
    return (TimerTicks() / Tpm) / 1000;
}

STATIC INT DhcpAddOptionByte(UINT8 *Opts, INT *Off, UINT8 Type, UINT8 Value) {
    Opts[(*Off)++] = Type;
    Opts[(*Off)++] = 1;
    Opts[(*Off)++] = Value;
    return SUCCESS;
}

STATIC INT DhcpAddOptionBytes(UINT8 *Opts, INT *Off, UINT8 Type,
                              const UINT8 *Data, UINT8 Len) {
    Opts[(*Off)++] = Type;
    Opts[(*Off)++] = Len;
    MemCpy(Opts + *Off, Data, Len);
    *Off += Len;
    return SUCCESS;
}

STATIC INT DhcpAddOptionDword(UINT8 *Opts, INT *Off, UINT8 Type, UINT32 Value) {
    Opts[(*Off)++] = Type;
    Opts[(*Off)++] = 4;
    UINT32 Be = Htonl(Value);
    MemCpy(Opts + *Off, &Be, 4);
    *Off += 4;
    return SUCCESS;
}

STATIC UINT8 *DhcpFindOption(DhcpHeader *Dhcp, UINT32 PacketLen,
                             UINT8 Type, INT *OutLen) {
    UINT8 *Opt = Dhcp->Options;
    UINT32 Off = 0;
    UINT32 Max;

    if (PacketLen <= sizeof(DhcpHeader)) return NULLPTR;
    Max = PacketLen - (UINT32)sizeof(DhcpHeader);

    while (Off < Max) {
        UINT8 T = Opt[Off++];
        if (T == DHCP_OPT_END) break;
        if (T == DHCP_OPT_PAD) continue;

        if (Off >= Max) break;
        UINT8 L = Opt[Off++];
        if (Off + L > Max) break;

        if (T == Type) {
            if (OutLen) *OutLen = L;
            return Opt + Off;
        }
        Off += L;
    }
    return NULLPTR;
}

/*
 * ============================================================================
 * Send — FIXED
 *
 * Original bug: NetworkAllocBuf(64) only ever gave 64 + 128 = 192 bytes
 * of storage, but NetworkPut(TotalLen) needed ~280. NetworkPut failed
 * with INCORRECT_VALUE, DhcpSend mapped it to NO_MEMORY, so the caller
 * saw -5. The new version builds header + options in local buffers,
 * computes the exact size, pads to the RFC 2131 minimum of 300 bytes,
 * and only then allocates.
 * ============================================================================
 */

#define DHCP_BOOTP_MIN 300

STATIC INT DhcpSend(DhcpClient *C, UINT8 MsgType, UINT32 RequestedIp) {
    if (!C || !C->Dev) return NO_OBJECT;

    /* 1. Fixed header. */
    DhcpHeader Hdr;
    MemSet(&Hdr, 0, sizeof(Hdr));
    Hdr.Op    = 1;
    Hdr.HType = 1;
    Hdr.HLen  = 6;
    Hdr.Xid   = Htonl(C->Xid);
    Hdr.Flags = Htons(0x8000);      /* broadcast replies */
    Hdr.Magic = Htonl(DHCP_MAGIC_COOKIE);
    MemCpy(Hdr.ChAddr, C->Dev->MacAddr, NETWORK_ETH_ALEN);

    /* 2. Options in a local buffer. */
    UINT8 Opts[256];
    INT Off = 0;

    DhcpAddOptionByte(Opts, &Off, DHCP_OPT_MSG_TYPE, MsgType);

    UINT8 ClientId[7];
    ClientId[0] = 1;
    MemCpy(ClientId + 1, C->Dev->MacAddr, NETWORK_ETH_ALEN);
    DhcpAddOptionBytes(Opts, &Off, DHCP_OPT_CLIENT_ID, ClientId, sizeof(ClientId));

    if (RequestedIp) {
        DhcpAddOptionDword(Opts, &Off, DHCP_OPT_REQUESTED_IP, RequestedIp);
    }
    if (MsgType == DHCP_REQUEST && C->ServerIp) {
        DhcpAddOptionDword(Opts, &Off, DHCP_OPT_SERVER_ID, C->ServerIp);
    }

    UINT8 ParamList[] = {
        DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER,
        DHCP_OPT_DNS_SERVER,  DHCP_OPT_LEASE_TIME,
        DHCP_OPT_SERVER_ID,
    };
    DhcpAddOptionBytes(Opts, &Off, DHCP_OPT_PARAM_LIST,
                       ParamList, sizeof(ParamList));

    static const CHAR Hostname[] = "2kos";
    DhcpAddOptionBytes(Opts, &Off, DHCP_OPT_HOSTNAME,
                       (const UINT8*)Hostname, sizeof(Hostname) - 1);

    Opts[Off++] = DHCP_OPT_END;

    /* 3. Real size = header + options, padded to BOOTP minimum. */
    UINT32 TotalLen = (UINT32)sizeof(DhcpHeader) + (UINT32)Off;
    if (TotalLen < DHCP_BOOTP_MIN) TotalLen = DHCP_BOOTP_MIN;

    /* 4. Allocate exactly what we need. */
    NetworkBuf *Buf = NetworkAllocBuf(TotalLen);
    if (!Buf) {
        StdErrPrintf("dhcp: alloc %u failed\n", TotalLen);
        return NO_MEMORY;
    }

    if (NetworkPut(Buf, TotalLen) != SUCCESS) {
        StdErrPrintf("dhcp: put %u failed (TrueSize=%u HeadRoom=%u)\n",
                     TotalLen, Buf->TrueSize, Buf->HeadRoom);
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    /* 5. Fill: header, options, zero padding. */
    DhcpHeader *Dhcp = (DhcpHeader*)Buf->Data;
    MemSet(Dhcp, 0, TotalLen);                     /* zero-pad */
    MemCpy(Dhcp, &Hdr, sizeof(Hdr));
    MemCpy(Dhcp->Options, Opts, (USIZE)Off);

    /* 6. Source address: 0.0.0.0 for DISCOVER, interface IP otherwise. */
    UINT32 Src = 0;
    if (*(UINT32*)C->Dev->IpAddr != 0) {
        Src = *(UINT32*)C->Dev->IpAddr;
    }

    INT Rc = UdpOutput(Buf, 0xFFFFFFFFu, Src,
                       DHCP_SERVER_PORT, DHCP_CLIENT_PORT);
    if (Rc != SUCCESS) {
        StdErrPrintf("dhcp: udp send failed rc=%d (len=%u, src=0x%08X)\n",
                     Rc, TotalLen, Src);
    }
    return Rc;
}

/*
 * ============================================================================
 * Response processing
 * ============================================================================
 */

STATIC VOID DhcpHandleOffer(DhcpClient *C, DhcpHeader *Dhcp, UINT32 PacketLen) {
    C->YIAddr = Ntohl(Dhcp->YIAddr);

    INT Len;
    UINT8 *Opt;

    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_SERVER_ID, &Len);
    if (Opt && Len >= 4) {
        UINT32 Be; MemCpy(&Be, Opt, 4); C->ServerIp = Ntohl(Be);
    }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_SUBNET_MASK, &Len);
    if (Opt && Len >= 4) {
        UINT32 Be; MemCpy(&Be, Opt, 4); C->SubnetMask = Ntohl(Be);
    }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_ROUTER, &Len);
    if (Opt && Len >= 4) {
        UINT32 Be; MemCpy(&Be, Opt, 4); C->Gateway = Ntohl(Be);
    }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_DNS_SERVER, &Len);
    if (Opt && Len >= 4) {
        UINT32 Be; MemCpy(&Be, Opt, 4); C->DnsServer = Ntohl(Be);
    }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_LEASE_TIME, &Len);
    if (Opt && Len >= 4) {
        UINT32 Be; MemCpy(&Be, Opt, 4); C->LeaseTime = Ntohl(Be);
    }

    if (C->YIAddr == 0) return;

    DhcpSend(C, DHCP_REQUEST, C->YIAddr);
}

STATIC VOID DhcpHandleAck(DhcpClient *C, DhcpHeader *Dhcp, UINT32 PacketLen) {
    INT Len;
    UINT8 *Opt;

    UINT32 YI = Ntohl(Dhcp->YIAddr);
    if (YI != 0) C->YIAddr = YI;

    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_SERVER_ID, &Len);
    if (Opt && Len >= 4) { UINT32 Be; MemCpy(&Be, Opt, 4); C->ServerIp = Ntohl(Be); }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_SUBNET_MASK, &Len);
    if (Opt && Len >= 4) { UINT32 Be; MemCpy(&Be, Opt, 4); C->SubnetMask = Ntohl(Be); }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_ROUTER, &Len);
    if (Opt && Len >= 4) { UINT32 Be; MemCpy(&Be, Opt, 4); C->Gateway = Ntohl(Be); }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_DNS_SERVER, &Len);
    if (Opt && Len >= 4) { UINT32 Be; MemCpy(&Be, Opt, 4); C->DnsServer = Ntohl(Be); }
    Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_LEASE_TIME, &Len);
    if (Opt && Len >= 4) { UINT32 Be; MemCpy(&Be, Opt, 4); C->LeaseTime = Ntohl(Be); }

    if (C->YIAddr == 0) {
        C->Configured = FALSE;
        if (C->Callback) C->Callback(C, FALSE);
        return;
    }

    C->Configured = TRUE;

    MemCpy(C->Dev->IpAddr,  &C->YIAddr, 4);
    MemCpy(C->Dev->Netmask, &C->SubnetMask, 4);
    MemCpy(C->Dev->Gateway, &C->Gateway, 4);

    IpV4RouteAdd(C->YIAddr & C->SubnetMask, C->SubnetMask, 0,
                 C->YIAddr, C->Dev, 0);
    if (C->Gateway) {
        IpV4RouteAdd(0, 0, C->Gateway, C->YIAddr, C->Dev, 0);
    }
    if (C->DnsServer) {
        DnsSetNameserver(C->DnsServer);
    }

    UINT64 Now = DhcpNowSeconds();
    C->LeaseExpires = Now + C->LeaseTime;
    C->RenewTime    = Now + C->LeaseTime / 2;
    C->RebindTime   = Now + C->LeaseTime * 7 / 8;

    CHAR IpStr[16];
    IpV4NTop(C->YIAddr, IpStr, sizeof(IpStr));
    StdOutPrintf("dhcp: %s leased %s\n", C->Dev->Name, IpStr);

    if (C->Callback) C->Callback(C, TRUE);
}

STATIC VOID DhcpHandleNak(DhcpClient *C) {
    C->Configured = FALSE;
    C->YIAddr     = 0;
    if (C->Callback) C->Callback(C, FALSE);
}

STATIC VOID DhcpRx(NetworkBuf *Buf, UINT32 SrcAddr, UINT32 DstAddr,
                   UINT16 SrcPort, UINT16 DstPort) {
    (VOID)SrcAddr;
    (VOID)DstAddr;
    (VOID)SrcPort;
    (VOID)DstPort;

    if (!Buf || Buf->Len < sizeof(DhcpHeader)) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    UINT32 PacketLen = Buf->Len;
    DhcpHeader *Dhcp = (DhcpHeader*)Buf->Data;

    if (Ntohl(Dhcp->Magic) != DHCP_MAGIC_COOKIE) {
	StdErrPrintf("dhcp: rx wrong magic 0x%08X\n", Ntohl(Dhcp->Magic));
        NetworkFreeBuf(Buf);
        return;
    }
    StdOutPrintf("dhcp: rx msg len=%u magic ok, xid=0x%08X, chaddr=%02x:%02x:%02x:%02x:%02x:%02x\n",
             PacketLen, Ntohl(Dhcp->Xid),
             Dhcp->ChAddr[0], Dhcp->ChAddr[1], Dhcp->ChAddr[2],
             Dhcp->ChAddr[3], Dhcp->ChAddr[4], Dhcp->ChAddr[5]);

    UINTPTR F = SpinLockAcquireIrq(&GDhcpLock);
    DhcpClient *C = GDhcpClients;
    while (C) {
        if (C->Dev &&
            MemCmp(C->Dev->MacAddr, Dhcp->ChAddr, NETWORK_ETH_ALEN) == 0 &&
            Ntohl(Dhcp->Xid) == C->Xid) {
            break;
        }
        C = C->Next;
    }
    SpinLockReleaseIrq(&GDhcpLock, F);

    if (!C) {
        NetworkFreeBuf(Buf);
        return;
    }

    INT Len = 0;
    UINT8 *Opt = DhcpFindOption(Dhcp, PacketLen, DHCP_OPT_MSG_TYPE, &Len);
    if (!Opt || Len < 1) {
        NetworkFreeBuf(Buf);
        return;
    }
    UINT8 MsgType = Opt[0];

    switch (MsgType) {
    case DHCP_OFFER: DhcpHandleOffer(C, Dhcp, PacketLen); break;
    case DHCP_ACK:   DhcpHandleAck(C, Dhcp, PacketLen);   break;
    case DHCP_NAK:   DhcpHandleNak(C);                    break;
    default: break;
    }

    NetworkFreeBuf(Buf);
}

/*
 * ============================================================================
 * Client API
 * ============================================================================
 */

INT DhcpInit(DhcpClient *C, NetworkDevice *Dev) {
    if (!C || !Dev) return NO_OBJECT;

    MemSet(C, 0, sizeof(DhcpClient));
    C->Dev = Dev;
    C->Xid = DhcpNewXid();

    UINTPTR F = SpinLockAcquireIrq(&GDhcpLock);
    C->Next = GDhcpClients;
    GDhcpClients = C;
    SpinLockReleaseIrq(&GDhcpLock, F);

    return SUCCESS;
}

INT DhcpStart(DhcpClient *C) {
    if (!C) return NO_OBJECT;
    C->Running    = TRUE;
    C->Configured = FALSE;
    C->YIAddr     = 0;
    C->Xid        = DhcpNewXid();

    C->RebindTime = DhcpNowSeconds() + 5;

    if (!IpV4RouteGetDefault()) {
        IpV4RouteAdd(0, 0, 0, 0, C->Dev, 0);
    }

    return DhcpSend(C, DHCP_DISCOVER, 0);
}

/* FIX: retry keeps the Xid so a late OFFER still matches. */
STATIC INT DhcpRetry(DhcpClient *C) {
    return DhcpSend(C, DHCP_DISCOVER, 0);
}

INT DhcpRenew(DhcpClient *C) {
    if (!C || !C->Configured) return NO_OBJECT;
    return DhcpSend(C, DHCP_REQUEST, C->YIAddr);
}

INT DhcpRelease(DhcpClient *C) {
    if (!C || !C->Configured) return NO_OBJECT;
    DhcpSend(C, DHCP_RELEASE, C->YIAddr);
    C->Configured = FALSE;
    return SUCCESS;
}

VOID DhcpStartOnAllInterfaces(DhcpCallback Callback) {
    NetworkDevice *Dev = NetworkGetFirstDevice();
    while (Dev) {
        if ((Dev->Type == NETWORK_TYPE_ETHERNET ||
             Dev->Type == NETWORK_TYPE_WIFI) &&
            Dev->Up && Dev->Running) {

            DhcpClient *C = (DhcpClient*)MemoryAllocate(sizeof(DhcpClient));
            if (C) {
                DhcpInit(C, Dev);
                C->Callback = Callback;
                DhcpStart(C);
            }
        }
        Dev = NetworkGetNext(Dev);
    }
}

/*
 * ============================================================================
 * Timer tick
 * ============================================================================
 */

VOID DhcpTick(VOID) {
    UINT64 Now = DhcpNowSeconds();

    UINTPTR F = SpinLockAcquireIrq(&GDhcpLock);
    DhcpClient *C = GDhcpClients;
    while (C) {
        if (C->Running) {
            if (!C->Configured) {
                if (Now >= C->RebindTime) {
                    StdOutPrintf("dhcp: %s retry DISCOVER (xid=0x%08X)\n",
                                 C->Dev->Name, C->Xid);
                    DhcpRetry(C);                     /* FIX */
                    C->RebindTime = Now + 5;
                }
            } else if (C->LeaseTime > 0) {
                if (Now >= C->LeaseExpires) {
                    C->Configured = FALSE;
                    StdOutPrintf("dhcp: %s lease expired, re-DISCOVER\n", C->Dev->Name);
                    DhcpStart(C);
                    C->RebindTime = Now + 5;
                } else if (Now >= C->RebindTime) {
                    DhcpRenew(C);
                    C->RebindTime = Now + C->LeaseTime / 8;
                } else if (Now >= C->RenewTime) {
                    DhcpRenew(C);
                    C->RenewTime = Now + C->LeaseTime / 2;
                }
            }
        }
        C = C->Next;
    }
    SpinLockReleaseIrq(&GDhcpLock, F);
}

DhcpClient *DhcpGetClient(NetworkDevice *Dev) {
    UINTPTR F = SpinLockAcquireIrq(&GDhcpLock);
    DhcpClient *C = GDhcpClients;
    while (C) {
        if (C->Dev == Dev) {
            SpinLockReleaseIrq(&GDhcpLock, F);
            return C;
        }
        C = C->Next;
    }
    SpinLockReleaseIrq(&GDhcpLock, F);
    return NULLPTR;
}

/*
 * ============================================================================
 * Subsystem init
 * ============================================================================
 */

INT DhcpSubsystemInit(VOID) {
    GDhcpClients = NULLPTR;
    SpinLockInit(&GDhcpLock);
    UdpRegisterHandler(DHCP_CLIENT_PORT, DhcpRx);
    return SUCCESS;
}