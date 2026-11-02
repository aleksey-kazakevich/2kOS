#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * IPv4 (RFC 791) — host stack.
 *
 * Implements:
 *   - header parse/validate/checksum on Rx
 *   - header build/checksum on Tx
 *   - routing table (dest/netmask/gateway/metric)
 *   - next-hop selection (local subnet vs gateway)
 *   - dispatch to ICMP / UDP / TCP
 *
 * Does NOT implement:
 *   - fragmentation / reassembly
 *   - options (IHL > 5 accepted on Rx, ignored on Tx)
 *   - multicast
 *   - source routing
 *
 * Addresses are UINT32 in NETWORK byte order everywhere. That is:
 * 10.0.2.15 is 0x0A00020F, not 0x0F02000A. The IPV4() macro exists so
 * call sites do not have to remember this.
 * ============================================================================
 */

#define IPV4_ADDR_LEN       4
#define IPV4_HEADER_MIN     20
#define IPV4_HEADER_MAX     60

#define IPV4_PROTO_ICMP     1
#define IPV4_PROTO_TCP      6
#define IPV4_PROTO_UDP      17

/* Fragmentation flags (top 3 bits of FragOff). */
#define IPV4_FLAG_MORE_FRAG  0x2000
#define IPV4_FLAG_DONT_FRAG  0x4000
#define IPV4_FLAG_RESERVED   0x8000
#define IPV4_FRAG_OFFSET_MASK 0x1FFF

typedef struct ATTRIBUTE(packed) IpV4Header {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    UINT8  Ihl     : 4;
    UINT8  Version : 4;
#else
    UINT8  Version : 4;
    UINT8  Ihl     : 4;
#endif
    UINT8  Tos;
    UINT16 TotalLen;
    UINT16 Id;
    UINT16 FragOff;
    UINT8  Ttl;
    UINT8  Protocol;
    UINT16 Checksum;
    UINT32 SrcAddr;
    UINT32 DstAddr;
} IpV4Header;

/*
 * IPV4(a,b,c,d) — build an address in network byte order.
 * Usage: IPV4(10, 0, 2, 15) == 0x0A00020F
 */
#define IPV4(a, b, c, d) \
    ((((UINT32)(a) & 0xFF) << 24) | \
     (((UINT32)(b) & 0xFF) << 16) | \
     (((UINT32)(c) & 0xFF) <<  8) | \
      ((UINT32)(d) & 0xFF))

#define IPV4_BROADCAST   0xFFFFFFFFu
#define IPV4_ANY         0x00000000u
#define IPV4_LOOPBACK    IPV4(127, 0, 0, 1)

/*
 * ============================================================================
 * Address conversion
 * ============================================================================
 */

UINT32 IpV4PTon(const CHAR *Str);                 /* "10.0.2.15" -> 0x0A00020F */
VOID   IpV4NTop(UINT32 Addr, CHAR *Buf, UINT32 Size);

BOOL IpV4AddrIsMulticast(UINT32 Addr);
BOOL IpV4AddrIsBroadcast(UINT32 Addr);
BOOL IpV4AddrIsLoopback(UINT32 Addr);

/*
 * ============================================================================
 * Routing
 * ============================================================================
 */

typedef struct IpV4Route {
    UINT32         Dest;
    UINT32         Netmask;
    UINT32         Gateway;
    UINT32         Src;
    NetworkDevice *Dev;
    UINT32         Metric;

    ListHead Node;
} IpV4Route;

INT IpV4RouteAdd(UINT32 Dest, UINT32 Netmask, UINT32 Gateway,
                 UINT32 Src, NetworkDevice *Dev, UINT32 Metric);
INT IpV4RouteDel(UINT32 Dest, UINT32 Netmask);

IpV4Route *IpV4RouteLookup(UINT32 Dest);
IpV4Route *IpV4RouteGetDefault(VOID);

/*
 * ============================================================================
 * Main API
 * ============================================================================
 */

INT  IpV4Init(VOID);

/*
 * IpV4Output — build an IPv4 packet from a transport payload.
 *
 * On entry Buf contains exactly the transport payload (TCP segment,
 * UDP datagram, ICMP message). This function:
 *   - validates that the payload fits the device MTU
 *   - prepends a 20-byte IPv4 header
 *   - fills in every field, computes the header checksum
 *   - resolves the next-hop MAC (ARP) or fails
 *   - frames and transmits via Ethernet
 *
 * SrcAddr == IPV4_ANY means "pick the source address of the outgoing
 * interface". DstAddr == IPV4_BROADCAST means "send to 255.255.255.255".
 *
 * On any failure Buf is freed and a negative error is returned.
 * On success ownership passes down the stack.
 */
INT IpV4Output(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr, UINT8 Protocol);

/*
 * IpV4Input — called by EthernetInput for ETH_P_IP frames. Consumes Buf.
 */
VOID IpV4Input(NetworkBuf *Buf, NetworkDevice *Dev);

typedef VOID (*IpV4ProtocolHandler)(NetworkBuf *Buf, NetworkDevice *Dev,
                                    UINT32 SrcAddr, UINT32 DstAddr);

INT  IpV4RegisterProtocol(UINT8 Protocol, IpV4ProtocolHandler Handler);
VOID IpV4UnregisterProtocol(UINT8 Protocol);
VOID IpV4UnregisterProtocol(UINT8 Protocol);

/*
 * Internet checksum (RFC 1071). Returns a value already in host byte
 * order; callers that write it to the wire must byte-swap.
 */
UINT16 IpV4Checksum(UINT16 *Data, UINT32 Len);