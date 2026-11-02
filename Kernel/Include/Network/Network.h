#pragma once

#include <Time/Timer.h>
#include <Types.h>
#include <List.h>

/*
 * ============================================================================
 * Constants
 * ============================================================================
 */

#define NETWORK_MTU             1500
#define NETWORK_HEADROOM        128
#define NETWORK_BUFFER_SIZE     (NETWORK_MTU + NETWORK_HEADROOM)

#define NETWORK_ETH_ALEN        6
#define NETWORK_IP_ALEN         4
#define NETWORK_IP6_ALEN        16

/* EtherTypes. Duplicated from Ethernet.h on purpose so that code which
 * only needs the constant does not pull in the whole header. */
#define NETWORK_ETH_P_IP        0x0800
#define NETWORK_ETH_P_ARP       0x0806
#define NETWORK_ETH_P_IPV6      0x86DD

/* IP protocol numbers. */
#define NETWORK_IP_PROTO_ICMP   1
#define NETWORK_IP_PROTO_TCP    6
#define NETWORK_IP_PROTO_UDP    17

/*
 * ============================================================================
 * Byte order helpers
 *
 * The kernel is little-endian, the network is big-endian. These four
 * inlines are the only place in the stack where the conversion happens.
 * Everything above Ethernet works in host byte order.
 * ============================================================================
 */

STATIC INLINE UINT16 Ntohs(UINT16 N) { return (UINT16)((N >> 8) | (N << 8)); }
STATIC INLINE UINT16 Htons(UINT16 N) { return (UINT16)((N >> 8) | (N << 8)); }

STATIC INLINE UINT32 Ntohl(UINT32 N) {
    return ((N & 0x000000FFu) << 24) |
           ((N & 0x0000FF00u) <<  8) |
           ((N & 0x00FF0000u) >>  8) |
           ((N & 0xFF000000u) >> 24);
}

STATIC INLINE UINT32 Htonl(UINT32 N) {
    return ((N & 0x000000FFu) << 24) |
           ((N & 0x0000FF00u) <<  8) |
           ((N & 0x00FF0000u) >>  8) |
           ((N & 0xFF000000u) >> 24);
}

/*
 * ============================================================================
 * Packet buffer (sk_buff-like)
 *
 * Layout:
 *
 *     Head                 Data                Tail            End
 *      |                    |                   |               |
 *      v                    v                   v               v
 *     +--------------------+-------------------+---------------+
 *     | headroom (128)     | payload (Len)     | tailroom      |
 *     +--------------------+-------------------+---------------+
 *
 *   - Data - Head        == HeadRoom
 *   - Tail - Data        == Len
 *   - End  - Head        == TrueSize
 *
 * Lower layers push headers into the headroom (Data moves left,
 * Len grows). Upper layers pull headers off (Data moves right,
 * Len shrinks). The allocation itself never moves after
 * NetworkAllocBuf.
 *
 * RefCount exists because TCP retransmission needs to keep a copy of
 * a segment while the original is on the wire. A buffer is freed when
 * the count reaches zero.
 * ============================================================================
 */

typedef struct NetworkBuf {
    struct NetworkBuf    *Next;      /* chain: fragments, queues, retransmit  */

    UINT8  *Head;                    /* start of the allocation               */
    UINT8  *Data;                    /* start of the current payload          */
    UINT8  *Tail;                    /* one past the last byte of the payload */
    UINT8  *End;                     /* one past the end of the allocation    */

    UINT32  Len;                     /* Tail - Data                           */
    UINT32  HeadRoom;                /* Data - Head                           */
    UINT32  TrueSize;                /* End  - Head                           */

    UINT16  Protocol;                /* set on Tx, used by Rx                 */

    UINT32  IpSrc;                   /* set by IPv4 on Rx                     */
    UINT32  IpDst;

    struct NetworkDevice *Dev;       /* receiving or transmitting device      */

    UINT32  Flags;
    volatile UINT32 RefCount;
} NetworkBuf;

/* Flags for NetworkBuf.Flags. */
#define NETBUF_F_IP_CSUM_OK   (1u << 0)
#define NETBUF_F_TCP_CSUM_OK  (1u << 1)
#define NETBUF_F_UDP_CSUM_OK  (1u << 2)
#define NETBUF_F_BCAST        (1u << 3)
#define NETBUF_F_MCAST        (1u << 4)

/*
 * ============================================================================
 * Statistics
 * ============================================================================
 */

typedef struct NetworkStats {
    UINT64 RxPackets;
    UINT64 RxBytes;
    UINT64 RxErrors;
    UINT64 RxDropped;

    UINT64 TxPackets;
    UINT64 TxBytes;
    UINT64 TxErrors;
    UINT64 TxDropped;
} NetworkStats;

/*
 * ============================================================================
 * Device types
 * ============================================================================
 */

typedef enum {
    NETWORK_TYPE_NONE = 0,
    NETWORK_TYPE_ETHERNET = 1,
    NETWORK_TYPE_WIFI = 2,
    NETWORK_TYPE_LOOPBACK = 3,
    NETWORK_TYPE_PPP = 4,
    NETWORK_TYPE_BRIDGE = 5,
} NetworkDeviceType;

/*
 * ============================================================================
 * Network device
 *
 * The driver fills in Name, Type, MacAddr, Mtu and the function
 * pointers, then calls NetworkRegisterDevice. Everything above this
 * struct only ever talks to the device through these callbacks.
 * ============================================================================
 */

typedef struct NetworkDevice {
    CHAR     Name[16];
    UINT32   Index;
    NetworkDeviceType Type;

    UINT8    MacAddr[NETWORK_ETH_ALEN];
    UINT8    IpAddr[NETWORK_IP_ALEN];       /* 0.0.0.0 = unconfigured */
    UINT8    Netmask[NETWORK_IP_ALEN];
    UINT8    Gateway[NETWORK_IP_ALEN];

    BOOL     Up;
    BOOL     Running;

    UINT32   Mtu;

    INT  (*Open)(struct NetworkDevice *Dev);
    INT  (*Stop)(struct NetworkDevice *Dev);
    INT  (*Xmit)(struct NetworkBuf *Buf, struct NetworkDevice *Dev);
    INT  (*Poll)(struct NetworkDevice *Dev);

    NetworkStats Stats;
    VOID        *Priv;

    ListHead Node;
} NetworkDevice;

/*
 * ============================================================================
 * Buffer API
 * ============================================================================
 */

NetworkBuf *NetworkAllocBuf(UINT32 Size);
VOID        NetworkFreeBuf(NetworkBuf *Buf);

VOID        NetworkBufRef(NetworkBuf *Buf);
VOID        NetworkBufUnref(NetworkBuf *Buf);
NetworkBuf *NetworkBufClone(NetworkBuf *Buf);

/*
 * NetworkReserve — move Data backwards by HeadRoom bytes, growing the
 * headroom for a header that will be written next. Returns SUCCESS or
 * INCORRECT_VALUE if there is not enough headroom.
 */
INT NetworkReserve(NetworkBuf *Buf, UINT32 HeadRoom);

/*
 * NetworkPut — extend the payload by Len bytes at the end. The new
 * bytes are NOT zeroed. Returns SUCCESS or INCORRECT_VALUE.
 */
INT NetworkPut(NetworkBuf *Buf, UINT32 Len);

/*
 * NetworkPull — strip Len bytes from the front of the payload. The
 * caller usually already saved a pointer to the old Data to read the
 * stripped header. Returns SUCCESS or INCORRECT_VALUE.
 */
INT NetworkPull(NetworkBuf *Buf, UINT32 Len);

/*
 * NetworkTrim — drop Len bytes from the end. Used to remove Ethernet
 * padding that the sender added to reach the minimum frame size.
 */
INT NetworkTrim(NetworkBuf *Buf, UINT32 Len);

/*
 * ============================================================================
 * Device registry
 * ============================================================================
 */

INT            NetworkRegisterDevice(NetworkDevice *Dev);
INT            NetworkUnregisterDevice(NetworkDevice *Dev);
NetworkDevice *NetworkGetDevice(UINT32 Index);
NetworkDevice *NetworkGetDeviceByName(const CHAR *Name);
NetworkDevice *NetworkGetFirstActive(VOID);
NetworkDevice *NetworkGetFirstDevice(VOID);
NetworkDevice *NetworkGetNext(NetworkDevice *Dev);
INT            NetworkGetDevicesByType(NetworkDeviceType Type,
                                       NetworkDevice **Devices, INT Max);

/*
 * ============================================================================
 * Data path
 * ============================================================================
 */

INT   NetworkRx(NetworkBuf *Buf, NetworkDevice *Dev);
INT   NetworkTx(NetworkBuf *Buf, NetworkDevice *Dev);
VOID  NetworkPollDevices(VOID);

/*
 * ============================================================================
 * Protocol registration
 *
 * Thin wrapper over EthernetRegisterProtocol so that code which does
 * not want to include Ethernet.h can still register a protocol.
 * ============================================================================
 */

typedef VOID (*NetworkRxHandler)(NetworkBuf *Buf, NetworkDevice *Dev);

INT  NetworkRegisterProtocol(UINT16 EthType, NetworkRxHandler Handler);
VOID NetworkUnregisterProtocol(UINT16 EthType);

/*
 * ============================================================================
 * Lifecycle
 * ============================================================================
 */

INT NetworkInit(VOID);
INT NetworkLateInit(VOID);