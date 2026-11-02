#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * UDP (RFC 768) — host subset.
 *
 * Port-based demultiplexing with a handler table. No sockets yet —
 * DHCP, DNS and the future socket layer register handlers directly.
 *
 * Checksum: computed on Tx, verified on Rx. A zero checksum in an
 * incoming packet is accepted as "not computed" (per RFC 768, only
 * legal for IPv4).
 * ============================================================================
 */

#define UDP_HEADER_SIZE  8
#define UDP_MAX_PORTS    65536

typedef struct ATTRIBUTE(packed) UdpHeader {
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT16 Length;
    UINT16 Checksum;
} UdpHeader;

/*
 * Handler signature. Buf contains the payload after the UDP header.
 * Src and Dst are the IPv4 addresses from the enclosing packet.
 * The handler owns Buf and must free it.
 */
typedef VOID (*UdpHandler)(NetworkBuf *Buf, UINT32 SrcAddr, UINT32 DstAddr,
                           UINT16 SrcPort, UINT16 DstPort);

/*
 * ============================================================================
 * API
 * ============================================================================
 */

INT  UdpInit(VOID);

INT  UdpRegisterHandler(UINT16 Port, UdpHandler Handler);
VOID UdpUnregisterHandler(UINT16 Port);

/*
 * UdpRx — called by IPv4 for IPV4_PROTO_UDP packets. Consumes Buf.
 */
VOID UdpRx(NetworkBuf *Buf, NetworkDevice *Dev,
           UINT32 SrcAddr, UINT32 DstAddr);

/*
 * UdpOutput — build a UDP datagram from a payload. Buf contains the
 * payload only. This function prepends the header, computes the
 * checksum, and hands off to IPv4.
 */
INT UdpOutput(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr,
              UINT16 DestPort, UINT16 SrcPort);

/*
 * Checksum helper (also used by the socket layer later).
 */
UINT16 UdpChecksum(UdpHeader *Udp, UINT32 Len, UINT32 Src, UINT32 Dst);