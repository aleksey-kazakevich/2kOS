#include <Network/IpV4.h>
#include <Network/Dhcp.h>
#include <Network/Udp.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Return.h>

STATIC UdpHandler GUdpHandlers[UDP_MAX_PORTS];
STATIC BOOL       GUdpInitialized = FALSE;

/*
 * ============================================================================
 * Checksum (RFC 768)
 * ============================================================================
 */

UINT16 UdpChecksum(UdpHeader *Udp, UINT32 Len, UINT32 Src, UINT32 Dst) {
    UINT32 Sum = 0;
    UINT8 *P;

    P = (UINT8*)&Src;  Sum += (P[0] << 8) | P[1]; Sum += (P[2] << 8) | P[3];
    P = (UINT8*)&Dst;  Sum += (P[0] << 8) | P[1]; Sum += (P[2] << 8) | P[3];
    Sum += 0;
    Sum += IPV4_PROTO_UDP;
    Sum += Len;

    P = (UINT8*)Udp;
    while (Len > 1) {
        Sum += (P[0] << 8) | P[1];
        P   += 2;
        Len -= 2;
    }
    if (Len) Sum += P[0] << 8;

    while (Sum >> 16) Sum = (Sum & 0xFFFF) + (Sum >> 16);
    return (UINT16)(~Sum & 0xFFFF);
}

/*
 * ============================================================================
 * Registration
 * ============================================================================
 */

INT UdpRegisterHandler(UINT16 Port, UdpHandler Handler) {
    if (Port >= UDP_MAX_PORTS) return INCORRECT_VALUE;
    if (!Handler) return NO_OBJECT;
    if (GUdpHandlers[Port]) return ALREADY_EXISTS;

    GUdpHandlers[Port] = Handler;
    return SUCCESS;
}

VOID UdpUnregisterHandler(UINT16 Port) {
    if (Port < UDP_MAX_PORTS) GUdpHandlers[Port] = NULLPTR;
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

VOID UdpRx(NetworkBuf *Buf, NetworkDevice *Dev,
           UINT32 SrcAddr, UINT32 DstAddr) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    if (Buf->Len < UDP_HEADER_SIZE) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    UdpHeader *Udp = (UdpHeader*)Buf->Data;
    UINT16 SrcPort = Ntohs(Udp->SrcPort);
    UINT16 DstPort = Ntohs(Udp->DstPort);
    UINT16 Length  = Ntohs(Udp->Length);

    if (Length < UDP_HEADER_SIZE || Length > Buf->Len) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    if (Length < Buf->Len) {
        NetworkTrim(Buf, Buf->Len - Length);
    }

    if (Udp->Checksum != 0) {
        UINT16 Received = Udp->Checksum;
        Udp->Checksum = 0;
        UINT16 Computed = Htons(UdpChecksum(Udp, Length, SrcAddr, DstAddr));
        Udp->Checksum = Received;
        if (Computed != Received) {
            Dev->Stats.RxErrors++;
            NetworkFreeBuf(Buf);
            return;
        }
    }
    Buf->Flags |= NETBUF_F_UDP_CSUM_OK;

    if (NetworkPull(Buf, UDP_HEADER_SIZE) != SUCCESS) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    if (GUdpHandlers[DstPort]) {
        GUdpHandlers[DstPort](Buf, SrcAddr, DstAddr, SrcPort, DstPort);
        return;
    }

    Dev->Stats.RxDropped++;
    NetworkFreeBuf(Buf);
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

INT UdpOutput(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr,
              UINT16 DestPort, UINT16 SrcPort) {
    if (!Buf) return NO_OBJECT;

    UINT32 TotalLen = Buf->Len + UDP_HEADER_SIZE;
    if (TotalLen > 0xFFFF) {
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    if (NetworkReserve(Buf, UDP_HEADER_SIZE) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    UdpHeader *Udp = (UdpHeader*)Buf->Data;
    Udp->SrcPort  = Htons(SrcPort);
    Udp->DstPort  = Htons(DestPort);
    Udp->Length   = Htons((UINT16)TotalLen);
    Udp->Checksum = 0;

    /* FIX: do not silently fall back to the first active interface.
     * Let IpV4Output pick the correct source address for the route.
     * For DHCP DISCOVER (Src=0, Dst=broadcast) IpV4Output will keep
     * Src=0, which is the correct on-wire value. For everything else
     * IpV4Output resolves Src from the route. */
    UINT32 EffectiveSrc = SrcAddr;

    if (EffectiveSrc == 0 && SrcPort != DHCP_CLIENT_PORT) {
        /* Ordinary traffic: let IpV4Output resolve via the route. */
        NetworkDevice *Dev = NetworkGetFirstActive();
        if (Dev) {
            UINT32 IfIp = *(UINT32*)Dev->IpAddr;
            if (IfIp != 0 && !IpV4AddrIsBroadcast(DestAddr)) {
                EffectiveSrc = IfIp;
            }
        }
    }

    UINT16 Csum = UdpChecksum(Udp, TotalLen, EffectiveSrc, DestAddr);
    Udp->Checksum = (Csum == 0) ? 0xFFFF : Htons(Csum);

    return IpV4Output(Buf, DestAddr, EffectiveSrc, IPV4_PROTO_UDP);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT UdpInit(VOID) {
    if (GUdpInitialized) return SUCCESS;

    MemSet(GUdpHandlers, 0, sizeof(GUdpHandlers));

    IpV4RegisterProtocol(IPV4_PROTO_UDP, UdpRx);

    GUdpInitialized = TRUE;
    StdOutPrintf("net: udp initialized\n");
    return SUCCESS;
}