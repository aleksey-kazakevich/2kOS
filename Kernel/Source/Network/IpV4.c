#include <Network/Ethernet.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/ICmp.h>
#include <Network/Arp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <SpinLock.h>
#include <Return.h>

STATIC ListHead        GIpV4Routes;
STATIC UINT16          GIpV4IdCounter = 0;
STATIC SpinLock        GIpV4Lock;
STATIC BOOL            GIpV4Initialized = FALSE;
STATIC IpV4ProtocolHandler GIpV4Protocols[256];

/*
 * ============================================================================
 * Address conversion
 * ============================================================================
 */

UINT32 IpV4PTon(const CHAR *Str) {
    if (!Str) return 0;

    UINT32 Octets[4] = {0, 0, 0, 0};
    INT    I = 0;
    UINT32 Val = 0;
    BOOL   HaveDigit = FALSE;

    while (*Str && I < 4) {
        if (*Str >= '0' && *Str <= '9') {
            Val = Val * 10 + (UINT32)(*Str - '0');
            HaveDigit = TRUE;
            if (Val > 255) return 0;
        } else if (*Str == '.') {
            if (!HaveDigit) return 0;
            Octets[I++] = Val;
            Val = 0;
            HaveDigit = FALSE;
        } else {
            return 0;
        }
        Str++;
    }

    if (I != 3 || !HaveDigit) return 0;
    Octets[3] = Val;

    return ((Octets[0] & 0xFF) << 24) |
           ((Octets[1] & 0xFF) << 16) |
           ((Octets[2] & 0xFF) <<  8) |
            (Octets[3] & 0xFF);
}

VOID IpV4NTop(UINT32 Addr, CHAR *Buf, UINT32 Size) {
    if (!Buf || Size < 16) return;
    UINT32 O[4] = {
        (Addr >> 24) & 0xFF, (Addr >> 16) & 0xFF,
        (Addr >>  8) & 0xFF,  Addr        & 0xFF
    };
    INT P = 0;
    for (INT I = 0; I < 4; I++) {
        if (O[I] >= 100) Buf[P++] = '0' + O[I] / 100;
        if (O[I] >= 10)  Buf[P++] = '0' + (O[I] / 10) % 10;
        Buf[P++] = '0' + O[I] % 10;
        if (I < 3) Buf[P++] = '.';
    }
    Buf[P] = '\0';
}

BOOL IpV4AddrIsMulticast(UINT32 Addr) {
    return ((Addr >> 24) & 0xF0) == 0xE0;
}

BOOL IpV4AddrIsBroadcast(UINT32 Addr) {
    return Addr == IPV4_BROADCAST;
}

BOOL IpV4AddrIsLoopback(UINT32 Addr) {
    return (Addr >> 24) == 127;
}

/*
 * ============================================================================
 * Checksum (RFC 1071)
 * ============================================================================
 */

UINT16 IpV4Checksum(UINT16 *Data, UINT32 Len) {
    UINT32 Sum = 0;
    UINT8 *P = (UINT8*)Data;

    while (Len > 1) {
        Sum += (UINT32)((P[0] << 8) | P[1]);
        P   += 2;
        Len -= 2;
    }
    if (Len) Sum += (UINT32)(P[0] << 8);

    while (Sum >> 16) Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (UINT16)(~Sum & 0xFFFF);
}

/*
 * ============================================================================
 * Routing
 * ============================================================================
 */

INT IpV4RouteAdd(UINT32 Dest, UINT32 Netmask, UINT32 Gateway,
                 UINT32 Src, NetworkDevice *Dev, UINT32 Metric) {
    IpV4Route *Route = (IpV4Route*)MemoryAllocate(sizeof(IpV4Route));
    if (!Route) return NO_MEMORY;

    Route->Dest    = Dest;
    Route->Netmask = Netmask;
    Route->Gateway = Gateway;
    Route->Src     = Src;
    Route->Dev     = Dev;
    Route->Metric  = Metric;

    UINTPTR F = SpinLockAcquireIrq(&GIpV4Lock);
    ListAddTail(&GIpV4Routes, &Route->Node);
    SpinLockReleaseIrq(&GIpV4Lock, F);

    return SUCCESS;
}

INT IpV4RouteDel(UINT32 Dest, UINT32 Netmask) {
    UINTPTR F = SpinLockAcquireIrq(&GIpV4Lock);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GIpV4Routes) {
        IpV4Route *R = ListEntry(Pos, IpV4Route, Node);
        if (R->Dest == Dest && R->Netmask == Netmask) {
            ListDel(&R->Node);
            SpinLockReleaseIrq(&GIpV4Lock, F);
            MemoryFree(R);
            return SUCCESS;
        }
    }

    SpinLockReleaseIrq(&GIpV4Lock, F);
    return NOT_FOUND;
}

IpV4Route *IpV4RouteLookup(UINT32 Dest) {
    UINTPTR F = SpinLockAcquireIrq(&GIpV4Lock);

    IpV4Route *Best = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &GIpV4Routes) {
        IpV4Route *R = ListEntry(Pos, IpV4Route, Node);
        if ((Dest & R->Netmask) == R->Dest) {
            if (!Best ||
                R->Metric < Best->Metric ||
                (R->Metric == Best->Metric &&
                 R->Netmask > Best->Netmask)) {
                Best = R;
            }
        }
    }

    SpinLockReleaseIrq(&GIpV4Lock, F);
    return Best;
}

IpV4Route *IpV4RouteGetDefault(VOID) {
    return IpV4RouteLookup(0);
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

INT IpV4Output(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr, UINT8 Protocol) {
    if (!Buf) return NO_OBJECT;

    /* FIX: copy the route fields under the lock. Returning a raw pointer
     * from IpV4RouteLookup is unsafe — another CPU may call RouteDel
     * concurrently and free the entry while we're still using it. */
    IpV4Route RouteCopy;
    BOOL Found = FALSE;

    UINTPTR F = SpinLockAcquireIrq(&GIpV4Lock);
    IpV4Route *Best = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &GIpV4Routes) {
        IpV4Route *R = ListEntry(Pos, IpV4Route, Node);
        if ((DestAddr & R->Netmask) == R->Dest) {
            if (!Best ||
                R->Metric < Best->Metric ||
                (R->Metric == Best->Metric &&
                 R->Netmask > Best->Netmask)) {
                Best = R;
            }
        }
    }
    if (Best) { RouteCopy = *Best; Found = TRUE; }
    SpinLockReleaseIrq(&GIpV4Lock, F);

    if (!Found) {
        NetworkFreeBuf(Buf);
        return NO_OBJECT;
    }

    NetworkDevice *RouteDev = RouteCopy.Dev;
    if (!RouteDev) {
        NetworkFreeBuf(Buf);
        return NO_OBJECT;
    }

    /* MTU check */
    if (Buf->Len + IPV4_HEADER_MIN > RouteDev->Mtu) {
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    if (NetworkReserve(Buf, IPV4_HEADER_MIN) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    IpV4Header *IpH = (IpV4Header*)Buf->Data;
    MemSet(IpH, 0, sizeof(IpV4Header));

    IpH->Version  = 4;
    IpH->Ihl      = 5;
    IpH->Tos      = 0;
    IpH->TotalLen = Htons((UINT16)Buf->Len);
    IpH->Id       = Htons(++GIpV4IdCounter);
    IpH->FragOff  = 0;
    IpH->Ttl      = 64;
    IpH->Protocol = Protocol;
    IpH->SrcAddr  = SrcAddr;
    IpH->DstAddr  = DestAddr;

    if (IpH->SrcAddr == 0) {
        if (IpV4AddrIsBroadcast(DestAddr)) {
            IpH->SrcAddr = 0;
        } else {
            if (RouteCopy.Src != 0) {
                /* RouteCopy.Src is host-order — store network-order */
                IpH->SrcAddr = Htonl(RouteCopy.Src);
            } else if (RouteDev) {
                /* Dev->IpAddr[] holds network-order bytes already */
                IpH->SrcAddr = *(UINT32*)RouteDev->IpAddr;
            }
            if (IpH->SrcAddr == 0) {
                NetworkFreeBuf(Buf);
                return NO_OBJECT;
            }
        }
    }

    IpH->Checksum = 0;
    UINT16 Csum = IpV4Checksum((UINT16*)IpH, IPV4_HEADER_MIN);
    IpH->Checksum = Htons(Csum);

    /* Next-hop MAC. Loopback and broadcast do not need ARP. */
    UINT32 NextHop;
    if (RouteCopy.Gateway != 0) {
        NextHop = Htonl(RouteCopy.Gateway);
    } else {
        NextHop = DestAddr;   /* already network-order */
    }
    UINT8  DstMac[NETWORK_ETH_ALEN];

    if (IpV4AddrIsBroadcast(DestAddr)) {
        MemCpy(DstMac, ETH_MAC_BROADCAST, NETWORK_ETH_ALEN);
    } else if (IpV4AddrIsLoopback(DestAddr) ||
               RouteDev->Type == NETWORK_TYPE_LOOPBACK) {
        MemCpy(DstMac, RouteDev->MacAddr, NETWORK_ETH_ALEN);
    } else {
        INT Rc = ArpResolve(RouteDev, NextHop, DstMac);
        if (Rc != SUCCESS) {
            /* Queue for later; ArpRx retries once the reply arrives.
             * Ownership of Buf is transferred to the ARP layer on
             * success; freed on failure. */
            if (ArpQueuePacket(RouteDev, NextHop, Buf) != SUCCESS) {
                NetworkFreeBuf(Buf);
            }
            return Rc;
        }
    }

    return EthernetOutput(Buf, RouteDev, DstMac, ETH_P_IP);
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

VOID IpV4Input(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    if (Buf->Len < IPV4_HEADER_MIN) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    IpV4Header *IpH = (IpV4Header*)Buf->Data;

    if (IpH->Version != 4) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    UINT32 HeaderLen = (UINT32)IpH->Ihl * 4;
    if (HeaderLen < IPV4_HEADER_MIN || HeaderLen > IPV4_HEADER_MAX ||
        HeaderLen > Buf->Len) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    UINT16 TotalLen = Ntohs(IpH->TotalLen);
    if (TotalLen < HeaderLen || TotalLen > Buf->Len) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    /* Trim Ethernet padding beyond the IPv4 total length. */
    if (TotalLen < Buf->Len) {
        NetworkTrim(Buf, Buf->Len - TotalLen);
    }

    /* Verify the header checksum. */
    UINT16 Received = IpH->Checksum;
    IpH->Checksum = 0;
    UINT16 Computed = Htons(IpV4Checksum((UINT16*)IpH, HeaderLen));
    IpH->Checksum = Received;
    if (Computed != Received) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }
    Buf->Flags |= NETBUF_F_IP_CSUM_OK;

    /* Fragmentation is not supported. */
    UINT16 Frag = Ntohs(IpH->FragOff);
    if (Frag & (IPV4_FLAG_MORE_FRAG | IPV4_FRAG_OFFSET_MASK)) {
        Dev->Stats.RxDropped++;
        NetworkFreeBuf(Buf);
        return;
    }

    UINT8  Proto = IpH->Protocol;
    UINT32 Src   = IpH->SrcAddr;
    UINT32 Dst   = IpH->DstAddr;

    if (NetworkPull(Buf, HeaderLen) != SUCCESS) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    Buf->IpSrc = Src;
    Buf->IpDst = Dst;

    UINT32 DstHost = Ntohl(Dst);
    UINT32 MyIpH   = *(UINT32*)Dev->IpAddr;
    if (DstHost != MyIpH &&
        Dst != IPV4_BROADCAST &&
        Dst != IPV4_LOOPBACK) {
        Dev->Stats.RxDropped++;
        NetworkFreeBuf(Buf);
        return;
    }

    if (GIpV4Protocols[Proto]) {
        GIpV4Protocols[Proto](Buf, Dev, Src, Dst);
        return;
    }

    Dev->Stats.RxDropped++;
    NetworkFreeBuf(Buf);
}

/*
 * ============================================================================
 * Protocol registration
 * ============================================================================
 */

INT IpV4RegisterProtocol(UINT8 Protocol, IpV4ProtocolHandler Handler) {
    if (Protocol >= 256) return INCORRECT_VALUE;
    if (!Handler) return NO_OBJECT;
    if (GIpV4Protocols[Protocol]) return ALREADY_EXISTS;

    GIpV4Protocols[Protocol] = Handler;
    return SUCCESS;
}

VOID IpV4UnregisterProtocol(UINT8 Protocol) {
    if (Protocol < 256) GIpV4Protocols[Protocol] = NULLPTR;
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT IpV4Init(VOID) {
    if (GIpV4Initialized) return SUCCESS;

    ListInit(&GIpV4Routes);
    MemSet(GIpV4Protocols, 0, sizeof(GIpV4Protocols));
    GIpV4IdCounter = 0;
    SpinLockInit(&GIpV4Lock);

    GIpV4Initialized = TRUE;
    StdOutPrintf("net: ipv4 initialized\n");
    return SUCCESS;
}