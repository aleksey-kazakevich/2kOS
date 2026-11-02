#pragma once

#include <Network/Network.h>
#include <Network/IpV4.h>
#include <SpinLock.h>
#include <Types.h>
#include <List.h>

/*
 * ============================================================================
 * TCP (RFC 793, RFC 6298 for RTO, RFC 5681 for congestion control) — host
 * subset.
 *
 * Implements:
 *   - full state machine (CLOSED..TIME_WAIT)
 *   - three-way handshake, both active and passive
 *   - sliding window, MSS negotiation, SACK is NOT implemented
 *   - retransmission with exponential backoff, RTT estimation per RFC 6298
 *   - simplified slow start / congestion avoidance (RFC 5681)
 *   - FIN teardown, TIME_WAIT timer
 *
 * Does NOT implement:
 *   - options beyond MSS in SYN (no window scale, no timestamps, no SACK)
 *   - urgent data
 *   - keepalive probes
 *   - simultaneous open / close
 *
 * Segments in flight are kept as NetworkBuf references, not copies.
 * Retransmission simply re-sends the same buffer, which is why
 * NetworkBuf has a RefCount.
 * ============================================================================
 */

#define TCP_HEADER_MIN          20
#define TCP_HEADER_MAX          60
#define TCP_MSS_DEFAULT         536
#define TCP_MSS_CALCULATED      1460
#define TCP_WINDOW_DEFAULT      65535
#define TCP_WINDOW_MAX          65535

#define TCP_RTO_MIN             200       /* ms */
#define TCP_RTO_MAX             120000    /* ms */
#define TCP_RTO_INITIAL         3000      /* ms */
#define TCP_SYN_RTO             3000      /* ms */
#define TCP_MSL                 30000     /* ms */
#define TCP_2MSL                (2 * TCP_MSL)
#define TCP_MAX_RETRANSMIT      12

/*
 * ============================================================================
 * Flags
 * ============================================================================
 */

#define TCP_FLAG_FIN            0x001
#define TCP_FLAG_SYN            0x002
#define TCP_FLAG_RST            0x004
#define TCP_FLAG_PSH            0x008
#define TCP_FLAG_ACK            0x010
#define TCP_FLAG_URG            0x020
#define TCP_FLAG_ECE            0x040
#define TCP_FLAG_CWR            0x080
#define TCP_FLAG_NS             0x100

/*
 * ============================================================================
 * States
 * ============================================================================
 */

typedef enum {
    TCP_STATE_CLOSED       = 0,
    TCP_STATE_LISTEN       = 1,
    TCP_STATE_SYN_SENT     = 2,
    TCP_STATE_SYN_RCVD     = 3,
    TCP_STATE_ESTABLISHED  = 4,
    TCP_STATE_FIN_WAIT_1   = 5,
    TCP_STATE_FIN_WAIT_2   = 6,
    TCP_STATE_CLOSE_WAIT   = 7,
    TCP_STATE_CLOSING      = 8,
    TCP_STATE_LAST_ACK     = 9,
    TCP_STATE_TIME_WAIT    = 10,
} TcpState;

const CHAR *TcpStateString(TcpState State);

/*
 * ============================================================================
 * On-wire header
 * ============================================================================
 */

typedef struct ATTRIBUTE(packed) TcpHeader {
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT32 SeqNum;
    UINT32 AckNum;
    UINT16 OffsetReservedFlags;    /* top 4 bits: data offset; low 9: flags */
    UINT16 Window;
    UINT16 Checksum;
    UINT16 Urgent;
} TcpHeader;

#define TCP_GET_DATA_OFFSET(h)  (((h)->OffsetReservedFlags >> 12) & 0xF)
#define TCP_GET_FLAGS(h)        ((h)->OffsetReservedFlags & 0x1FF)
#define TCP_SET_FLAGS(h, f)     \
    ((h)->OffsetReservedFlags = \
        (UINT16)(((h)->OffsetReservedFlags & ~0x1FF) | ((f) & 0x1FF)))
#define TCP_SET_DATA_OFFSET(h, o) \
    ((h)->OffsetReservedFlags = \
        (UINT16)(((h)->OffsetReservedFlags & ~0xF000) | (((o) & 0xF) << 12)))

/*
 * ============================================================================
 * Segment in flight
 *
 * The payload lives in the referenced NetworkBuf, not copied. This makes
 * retransmission cheap (re-send the same buffer) and keeps the send
 * queue out of the fast path.
 * ============================================================================
 */

typedef struct TcpSegment {
    ListHead   Node;
    UINT32     Seq;
    UINT32     Len;
    UINT32     ReadOffset;
    UINT16     Flags;
    NetworkBuf *Buf;               /* owned reference */
    UINT64     SentTime;           /* TimerTicks() */
    UINT32     RetransCount;
} TcpSegment;

/*
 * ============================================================================
 * Socket
 * ============================================================================
 */

typedef struct TcpSocket {
    /* 5-tuple. Addresses are host byte order (0x0A00020F for 10.0.2.15). */
    UINT32 LocalAddr;
    UINT32 RemoteAddr;
    UINT16 LocalPort;
    UINT16 RemotePort;

    /* State */
    TcpState State;
    UINT64   StateExpires;         /* TIME_WAIT, SYN_RCVD timers */

    /* Sequence numbers */
    UINT32 SndUna;
    UINT32 SndNxt;
    UINT32 SndWnd;
    UINT32 Iss;

    UINT32 RcvNxt;
    UINT32 RcvWnd;
    UINT32 Irs;

    /* Congestion control (RFC 5681, simplified) */
    UINT32 Cwnd;
    UINT32 Ssthresh;

    /* RTT estimation (RFC 6298) */
    UINT32 Srtt;
    UINT32 Rttvar;
    UINT32 Rto;
    UINT32 RxtShift;
    UINT32 RetransCount;

    /* Timers */
    UINT64 TimerExpires;
    BOOL   TimerActive;
    UINT64 LastSent;
    UINT64 LastRecv;

    /* Queues. All hold ListHead of TcpSegment / raw data. */
    ListHead SendQueue;            /* payload waiting to be turned into segments */
    ListHead RetransQueue;         /* segments sent but not acked               */
    ListHead RecvQueue;            /* received data ready for the app           */
    ListHead AcceptQueue;          /* pending sockets on a listener             */

    UINT32 SendQueueLen;
    UINT32 RetransQueueLen;
    UINT32 RecvQueueLen;
    UINT32 AcceptQueueLen;

    /* MSS */
    UINT32 MssSend;
    UINT32 MssRecv;

    /* Callbacks */
    VOID (*ConnectCb)(struct TcpSocket *Sock, BOOL Success);
    VOID (*RecvCb)(struct TcpSocket *Sock);
    VOID (*CloseCb)(struct TcpSocket *Sock);
    VOID (*ErrorCb)(struct TcpSocket *Sock, INT Error);

    UINT32 BackLog;
    VOID  *Priv;

    ListHead Node;
} TcpSocket;

#define TCP_ERROR_CONNECTION_RESET   1
#define TCP_ERROR_TIMEOUT            2
#define TCP_ERROR_REFUSED            3

/*
 * ============================================================================
 * API
 * ============================================================================
 */

INT  TcpInit(VOID);
VOID TcpTick(VOID);                   /* call once per 100 ms */

TcpSocket *TcpSocketCreate(VOID);
VOID        TcpSocketDestroy(TcpSocket *Sock);

INT TcpBind(TcpSocket *Sock, UINT32 Addr, UINT16 Port);
INT TcpListen(TcpSocket *Sock, INT BackLog);
INT TcpConnect(TcpSocket *Sock, UINT32 Addr, UINT16 Port);
TcpSocket *TcpAccept(TcpSocket *ListenSock);

INT TcpSend(TcpSocket *Sock, const UINT8 *Data, UINT32 Len);
INT TcpRecv(TcpSocket *Sock, UINT8 *Buf, UINT32 Len);

INT TcpClose(TcpSocket *Sock);
INT TcpAbort(TcpSocket *Sock);

VOID TcpRx(NetworkBuf *Buf, NetworkDevice *Dev,
           UINT32 SrcAddr, UINT32 DstAddr);

UINT16 TcpChecksum(TcpHeader *Tcp, UINT32 Len, UINT32 Src, UINT32 Dst);