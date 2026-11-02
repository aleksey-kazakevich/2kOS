#pragma once

#include <Network/IpV4.h>
#include <Types.h>

/*
 * ============================================================================
 * ICMP (RFC 792) — host subset.
 *
 * Implements:
 *   - echo request / echo reply (ping)
 *   - destination unreachable and time exceeded (received only)
 *   - checksum verification and generation
 *
 * Does NOT implement:
 *   - timestamp, address mask, router advertisement
 *   - redirect
 *
 * The ping API is split into a blocking form (ICmpPing) used by the
 * shell and an async form (ICmpPingAsync) used by anything that has to
 * keep running while waiting.
 * ============================================================================
 */

#define ICMP_TYPE_ECHO_REPLY        0
#define ICMP_TYPE_DEST_UNREACH      3
#define ICMP_TYPE_SOURCE_QUENCH     4
#define ICMP_TYPE_REDIRECT          5
#define ICMP_TYPE_ECHO_REQUEST      8
#define ICMP_TYPE_TIME_EXCEEDED     11
#define ICMP_TYPE_PARAM_PROBLEM     12
#define ICMP_TYPE_TIMESTAMP         13
#define ICMP_TYPE_TIMESTAMP_REPLY   14

#define ICMP_CODE_NET_UNREACH       0
#define ICMP_CODE_HOST_UNREACH      1
#define ICMP_CODE_PROT_UNREACH      2
#define ICMP_CODE_PORT_UNREACH      3
#define ICMP_CODE_FRAG_NEEDED       4
#define ICMP_CODE_TTL_EXCEEDED      0
#define ICMP_CODE_REASSEMBLY        1

/* 64 - 8 = 56 bytes of payload. The first 8 bytes carry a timestamp. */
#define ICMP_DATA_SIZE              56

typedef struct ATTRIBUTE(packed) ICmpHeader {
    UINT8  Type;
    UINT8  Code;
    UINT16 Checksum;
    UINT16 Id;
    UINT16 Sequence;
} ICmpHeader;

typedef struct ATTRIBUTE(packed) ICmpUnreachHeader {
    UINT8  Type;
    UINT8  Code;
    UINT16 Checksum;
    UINT16 Unused;
    UINT16 Mtu;
} ICmpUnreachHeader;

/*
 * ============================================================================
 * Ping statistics
 * ============================================================================
 */

typedef struct PingStats {
    UINT32 Sent;
    UINT32 Received;
    UINT32 Lost;
    UINT64 MinRttUs;
    UINT64 MaxRttUs;
    UINT64 TotalRttUs;
    UINT64 AvgRttUs;
} PingStats;

typedef VOID (*PingCallback)(PingStats *Stats, VOID *Arg);

/*
 * ============================================================================
 * API
 * ============================================================================
 */

INT ICmpInit(VOID);

/*
 * ICmpRx — called by IPv4 for IPV4_PROTO_ICMP packets. Consumes Buf.
 * SrcAddr and DstAddr are passed by IPv4 so the reply can be sent back
 * without re-parsing the IPv4 header.
 */
VOID ICmpRx(NetworkBuf *Buf, NetworkDevice *Dev,
            UINT32 SrcAddr, UINT32 DstAddr);

/*
 * ICmpOutput — send an arbitrary ICMP message. Buf contains the ICMP
 * header plus payload; this function computes the checksum, fills in
 * Type/Code, and hands off to IPv4.
 */
INT ICmpOutput(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr,
               UINT8 Type, UINT8 Code);

/*
 * ICmpPing — blocking. Sends Count echo requests to DestAddr, waits up
 * to TimeoutMs for each, fills Stats. Returns SUCCESS.
 */
INT ICmpPing(UINT32 DestAddr, UINT32 Count, UINT32 TimeoutMs,
             PingStats *Stats);

/*
 * ICmpPingAsync — non-blocking. The callback is invoked once when all
 * Count replies have arrived or timed out.
 */
INT ICmpPingAsync(UINT32 DestAddr, UINT32 Count, UINT32 TimeoutMs,
                  PingCallback Callback, VOID *Arg);

VOID ICmpPingCancel(VOID);

/*
 * Periodic housekeeping; call once per second.
 */
VOID ICmpTick(VOID);

const CHAR *ICmpTypeString(UINT8 Type);
const CHAR *ICmpCodeString(UINT8 Type, UINT8 Code);

/* Internet checksum, identical to the one in IPv4. */
UINT16 ICmpChecksum(UINT16 *Data, UINT32 Len);