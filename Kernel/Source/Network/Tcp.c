#include <Network/Ethernet.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/ICmp.h>
#include <Network/Tcp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Scheduler.h>
#include <Lib/Math.h>
#include <SpinLock.h>
#include <Return.h>

STATIC ListHead   GTcpSockets;
STATIC ListHead   GTcpListeners;
STATIC SpinLock   GTcpLock;
STATIC UINT32     GTcpIss = 0x12345678;
STATIC UINT16     GTcpEphemeralPort = 49152;
STATIC BOOL       GTcpInitialized = FALSE;

/* FIX: forward declaration — TcpAllocEphemeralPort uses TcpPortInUse */
STATIC BOOL TcpPortInUse(UINT16 Port, UINT32 Addr);

/*
 * ============================================================================
 * State string
 * ============================================================================
 */

const CHAR *TcpStateString(TcpState State) {
    switch (State) {
    case TCP_STATE_CLOSED:      return "CLOSED";
    case TCP_STATE_LISTEN:      return "LISTEN";
    case TCP_STATE_SYN_SENT:    return "SYN_SENT";
    case TCP_STATE_SYN_RCVD:    return "SYN_RCVD";
    case TCP_STATE_ESTABLISHED: return "ESTABLISHED";
    case TCP_STATE_FIN_WAIT_1:  return "FIN_WAIT_1";
    case TCP_STATE_FIN_WAIT_2:  return "FIN_WAIT_2";
    case TCP_STATE_CLOSE_WAIT:  return "CLOSE_WAIT";
    case TCP_STATE_CLOSING:     return "CLOSING";
    case TCP_STATE_LAST_ACK:    return "LAST_ACK";
    case TCP_STATE_TIME_WAIT:   return "TIME_WAIT";
    default:                    return "UNKNOWN";
    }
}

/*
 * ============================================================================
 * Checksum
 * ============================================================================
 */

UINT16 TcpChecksum(TcpHeader *Tcp, UINT32 Len, UINT32 Src, UINT32 Dst) {
    UINT32 Sum = 0;
    UINT8 *P;

    P = (UINT8*)&Src;  Sum += (P[0] << 8) | P[1];  Sum += (P[2] << 8) | P[3];
    P = (UINT8*)&Dst;  Sum += (P[0] << 8) | P[1];  Sum += (P[2] << 8) | P[3];
    Sum += 0;
    Sum += IPV4_PROTO_TCP;
    Sum += Len;

    P = (UINT8*)Tcp;
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
 * Segment queue helpers
 * ============================================================================
 */

STATIC TcpSegment *TcpSegAlloc(NetworkBuf *Buf, UINT32 Seq, UINT32 Len,
                               UINT16 Flags) {
    TcpSegment *Seg = (TcpSegment*)MemoryAllocate(sizeof(TcpSegment));
    if (!Seg) return NULLPTR;
    MemSet(Seg, 0, sizeof(TcpSegment));

    Seg->Seq    = Seq;
    Seg->Len    = Len;
    Seg->Flags  = Flags;
    Seg->Buf    = Buf;
    Seg->SentTime = TimerTicks();
    /* FIX: ReadOffset initialised by MemSet to 0 */

    if (Buf) NetworkBufRef(Buf);
    return Seg;
}

STATIC VOID TcpSegFree(TcpSegment *Seg) {
    if (!Seg) return;
    if (Seg->Buf) NetworkBufUnref(Seg->Buf);
    MemoryFree(Seg);
}

STATIC VOID TcpFlushQueue(ListHead *Q) {
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, Q) {
        TcpSegment *Seg = ListEntry(Pos, TcpSegment, Node);
        ListDel(&Seg->Node);
        TcpSegFree(Seg);
    }
}

/*
 * ============================================================================
 * Socket helpers
 * ============================================================================
 */

STATIC UINT16 TcpAllocEphemeralPort(VOID) {
    /* FIX: original code called TcpBind(NULLPTR, ...) which always fails,
     * so port-in-use was never checked. Caller must hold GTcpLock. */
    UINT16 Start = GTcpEphemeralPort;
    do {
        UINT16 Port = GTcpEphemeralPort++;
        if (GTcpEphemeralPort > 65535) GTcpEphemeralPort = 49152;
        if (!TcpPortInUse(Port, 0)) return Port;
    } while (GTcpEphemeralPort != Start);
    return 0;
}

STATIC BOOL TcpPortInUse(UINT16 Port, UINT32 Addr) {
    ListHead *Pos;
    ListForEach(Pos, &GTcpSockets) {
        TcpSocket *S = ListEntry(Pos, TcpSocket, Node);
        if (S->LocalPort == Port && S->LocalAddr == Addr) return TRUE;
    }
    ListForEach(Pos, &GTcpListeners) {
        TcpSocket *S = ListEntry(Pos, TcpSocket, Node);
        if (S->LocalPort == Port && S->LocalAddr == Addr) return TRUE;
    }
    return FALSE;
}

/*
 * ============================================================================
 * Socket lifecycle
 * ============================================================================
 */

TcpSocket *TcpSocketCreate(VOID) {
    TcpSocket *S = (TcpSocket*)MemoryAllocate(sizeof(TcpSocket));
    if (!S) return NULLPTR;
    MemSet(S, 0, sizeof(TcpSocket));

    S->State       = TCP_STATE_CLOSED;
    S->RcvWnd      = TCP_WINDOW_DEFAULT;
    S->SndWnd      = TCP_WINDOW_DEFAULT;
    S->MssSend     = TCP_MSS_DEFAULT;
    S->MssRecv     = TCP_MSS_CALCULATED;
    S->Rto         = TCP_RTO_INITIAL;
    S->Srtt        = 0;
    S->Rttvar      = 750;
    S->Cwnd        = TCP_MSS_DEFAULT;
    S->Ssthresh    = 65535;

    ListInit(&S->SendQueue);
    ListInit(&S->RetransQueue);
    ListInit(&S->RecvQueue);
    ListInit(&S->AcceptQueue);
    ListInit(&S->Node);   /* FIX: Node must be initialised */

    return S;
}

VOID TcpSocketDestroy(TcpSocket *S) {
    if (!S) return;

    /* FIX: only unlink if the node is actually in a list */
    if (S->Node.Next && S->Node.Prev) {
        ListDel(&S->Node);
    }

    TcpFlushQueue(&S->SendQueue);
    TcpFlushQueue(&S->RetransQueue);
    TcpFlushQueue(&S->RecvQueue);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &S->AcceptQueue) {
        TcpSocket *A = ListEntry(Pos, TcpSocket, Node);
        ListDel(&A->Node);
        TcpSocketDestroy(A);
    }

    MemoryFree(S);
}

/*
 * ============================================================================
 * Segment transmit
 * ============================================================================
 */

STATIC INT TcpSendSegment(TcpSocket *S, UINT32 Seq, UINT32 Ack,
                          UINT16 Flags, NetworkBuf *Payload, UINT32 DataLen) {
    NetworkBuf *Buf;

    if (Payload) {
        /* Clone the payload. NetworkBufClone preserves headroom, so the
         * TCP header can be prepended without touching the original. */
        Buf = NetworkBufClone(Payload);
        if (!Buf) return NO_MEMORY;

        /* FIX: force Len to exactly DataLen — clone may carry stale Len */
        Buf->Len = DataLen;
        Buf->Tail = Buf->Data + DataLen;
    } else {
        /* FIX: do NOT NetworkPut(TCP_HEADER_MIN) here — NetworkReserve
         * already grows Len by TCP_HEADER_MIN. The old code produced
         * 40-byte control segments with 20 bytes of garbage. */
        Buf = NetworkAllocBuf(0);
        if (!Buf) return NO_MEMORY;
    }

    if (NetworkReserve(Buf, TCP_HEADER_MIN) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    TcpHeader *Tcp = (TcpHeader*)Buf->Data;
    MemSet(Tcp, 0, TCP_HEADER_MIN);

    Tcp->SrcPort  = Htons(S->LocalPort);
    Tcp->DstPort  = Htons(S->RemotePort);
    Tcp->SeqNum   = Htonl(Seq);
    Tcp->AckNum   = Htonl(Ack);
    Tcp->Window   = Htons((UINT16)S->RcvWnd);
    TCP_SET_DATA_OFFSET(Tcp, 5);
    TCP_SET_FLAGS(Tcp, Flags);

    Tcp->Checksum = 0;
    UINT16 Csum = TcpChecksum(Tcp, TCP_HEADER_MIN + DataLen,
                              S->LocalAddr, S->RemoteAddr);
    Tcp->Checksum = Htons(Csum);

    return IpV4Output(Buf, S->RemoteAddr, S->LocalAddr, IPV4_PROTO_TCP);
}

/*
 * ============================================================================
 * Send-from-queue
 * ============================================================================
 */

STATIC VOID TcpSendFromQueue(TcpSocket *S) {
    if (S->State != TCP_STATE_ESTABLISHED &&
        S->State != TCP_STATE_CLOSE_WAIT) return;

    UINT32 Window = S->SndWnd;
    if (Window > S->Cwnd) Window = S->Cwnd;

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &S->SendQueue) {
        TcpSegment *Seg = ListEntry(Pos, TcpSegment, Node);

        if (Seg->Len > Window) break;

        ListDel(&Seg->Node);
        S->SendQueueLen -= Seg->Len;

        TcpSendSegment(S, Seg->Seq, S->RcvNxt,
                       TCP_FLAG_ACK | TCP_FLAG_PSH, Seg->Buf, Seg->Len);

        Seg->SentTime     = TimerTicks();
        Seg->RetransCount = 0;
        ListAddTail(&S->RetransQueue, &Seg->Node);
        S->RetransQueueLen += Seg->Len;

        if (!S->TimerActive) {
            S->TimerExpires = Seg->SentTime + TimerMsToTicks(S->Rto);
            S->TimerActive  = TRUE;
        }

        Window -= Seg->Len;
    }

    S->LastSent = TimerTicks();
}

/*
 * ============================================================================
 * RTT + ACK handling
 * ============================================================================
 */

STATIC VOID TcpUpdateRtt(TcpSocket *S, UINT32 AckSeq) {
    ListHead *Pos;
    ListForEach(Pos, &S->RetransQueue) {
        TcpSegment *Seg = ListEntry(Pos, TcpSegment, Node);
        if (Seg->Seq + Seg->Len != AckSeq) continue;
        if (Seg->RetransCount > 0) return;   /* Karn's algorithm */

        UINT64 ElapsedUs =
            (TimerTicks() - Seg->SentTime) * 1000000ULL / TimerFreq();
        UINT32 RttMs = (UINT32)(ElapsedUs / 1000);
        if (RttMs == 0) RttMs = 1;

        if (S->Srtt == 0) {
            S->Srtt   = RttMs;
            S->Rttvar = RttMs / 2;
        } else {
            INT Delta = (INT)S->Srtt - (INT)RttMs;
            if (Delta < 0) Delta = -Delta;
            S->Rttvar = (3 * S->Rttvar + (UINT32)Delta) / 4;
            S->Srtt   = (7 * S->Srtt + RttMs) / 8;
        }

        S->Rto = S->Srtt + MAX(4 * S->Rttvar, TCP_RTO_MIN);
        if (S->Rto > TCP_RTO_MAX) S->Rto = TCP_RTO_MAX;
        return;
    }
}

STATIC VOID TcpAckData(TcpSocket *S, UINT32 AckSeq) {
    if (AckSeq <= S->SndUna) return;

    UINT32 Acked = AckSeq - S->SndUna;
    S->SndUna = AckSeq;

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &S->RetransQueue) {
        TcpSegment *Seg = ListEntry(Pos, TcpSegment, Node);
        if (Seg->Seq + Seg->Len <= AckSeq) {
            ListDel(&Seg->Node);
            S->RetransQueueLen -= Seg->Len;
            TcpSegFree(Seg);
        } else {
            break;
        }
    }

    if (ListEmpty(&S->RetransQueue)) {
        S->TimerActive = FALSE;
        S->RetransCount = 0;
        S->RxtShift = 0;
    }

    /* Congestion control (RFC 5681, simplified). */
    if (S->Cwnd < S->Ssthresh) {
        S->Cwnd += Acked;
        if (S->Cwnd > 65535) S->Cwnd = 65535;
    } else {
        UINT32 Inc = (S->MssSend * S->MssSend) / S->Cwnd;
        if (Inc == 0) Inc = 1;
        S->Cwnd += Inc;
        if (S->Cwnd > 65535) S->Cwnd = 65535;
    }

    TcpSendFromQueue(S);
}

STATIC VOID TcpRetransmit(TcpSocket *S) {
    if (ListEmpty(&S->RetransQueue)) {
        S->TimerActive = FALSE;
        return;
    }

    TcpSegment *Seg = ListEntry(S->RetransQueue.Next, TcpSegment, Node);

    S->Rto = MIN(S->Rto * 2, TCP_RTO_MAX);
    S->RetransCount++;
    S->RxtShift++;

    S->Ssthresh = MAX(S->Cwnd / 2, 2 * S->MssSend);
    S->Cwnd     = S->MssSend;

    TcpSendSegment(S, Seg->Seq, S->RcvNxt, Seg->Flags, Seg->Buf, Seg->Len);

    Seg->SentTime     = TimerTicks();
    Seg->RetransCount++;

    S->TimerExpires = Seg->SentTime + TimerMsToTicks(S->Rto);
    S->TimerActive  = TRUE;

    if (S->RetransCount >= TCP_MAX_RETRANSMIT) {
        StdErrPrintf("tcp: max retransmit reached on port %u\n", S->LocalPort);
        if (S->ErrorCb) S->ErrorCb(S, TCP_ERROR_TIMEOUT);
        TcpAbort(S);
    }
}

/*
 * ============================================================================
 * Control segments
 * ============================================================================
 */

STATIC VOID TcpSendSyn(TcpSocket *S) {
    S->Iss    = __sync_fetch_and_add(&GTcpIss, 1000000);
    S->SndUna = S->Iss;
    S->SndNxt = S->Iss + 1;

    TcpSendSegment(S, S->Iss, 0, TCP_FLAG_SYN, NULLPTR, 0);

    S->LastSent     = TimerTicks();
    S->TimerExpires = S->LastSent + TimerMsToTicks(TCP_SYN_RTO);
    S->TimerActive  = TRUE;
}

STATIC VOID TcpSendSynAck(TcpSocket *S) {
    S->Iss    = __sync_fetch_and_add(&GTcpIss, 1000000);
    S->SndUna = S->Iss;
    S->SndNxt = S->Iss + 1;

    TcpSendSegment(S, S->Iss, S->RcvNxt,
                   TCP_FLAG_SYN | TCP_FLAG_ACK, NULLPTR, 0);

    S->TimerExpires = TimerTicks() + TimerMsToTicks(TCP_SYN_RTO);
    S->TimerActive  = TRUE;
}

STATIC VOID TcpSendFin(TcpSocket *S) {
    TcpSendSegment(S, S->SndNxt, S->RcvNxt,
                   TCP_FLAG_FIN | TCP_FLAG_ACK, NULLPTR, 0);
    S->SndNxt++;
    S->LastSent     = TimerTicks();
    S->TimerExpires = S->LastSent + TimerMsToTicks(S->Rto);
    S->TimerActive  = TRUE;
}

STATIC VOID TcpSendRst(TcpSocket *S) {
    TcpSendSegment(S, S->SndNxt, S->RcvNxt,
                   TCP_FLAG_RST | TCP_FLAG_ACK, NULLPTR, 0);
}

/*
 * ============================================================================
 * API: bind / listen / connect / accept
 * ============================================================================
 */

INT TcpBind(TcpSocket *S, UINT32 Addr, UINT16 Port) {
    if (!S) return NO_OBJECT;
    if (S->State != TCP_STATE_CLOSED) return INCORRECT_VALUE;

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    BOOL InUse = TcpPortInUse(Port, Addr);
    SpinLockReleaseIrq(&GTcpLock, F);

    if (InUse) return ALREADY_EXISTS;

    S->LocalAddr = Addr;
    S->LocalPort = Port;
    return SUCCESS;
}

INT TcpListen(TcpSocket *S, INT BackLog) {
    if (!S) return NO_OBJECT;
    if (S->State != TCP_STATE_CLOSED) return INCORRECT_VALUE;
    if (S->LocalPort == 0) return INCORRECT_VALUE;

    S->State   = TCP_STATE_LISTEN;
    S->BackLog = (BackLog > 0) ? (UINT32)BackLog : 1;

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    ListAddTail(&GTcpListeners, &S->Node);
    SpinLockReleaseIrq(&GTcpLock, F);

    return SUCCESS;
}

INT TcpConnect(TcpSocket *S, UINT32 Addr, UINT16 Port) {
    if (!S) return NO_OBJECT;
    if (S->State != TCP_STATE_CLOSED) return INCORRECT_VALUE;

    S->RemoteAddr = Addr;
    S->RemotePort = Port;

    if (S->LocalPort == 0) {
        UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
        S->LocalPort = TcpAllocEphemeralPort();
        SpinLockReleaseIrq(&GTcpLock, F);
        if (S->LocalPort == 0) return NO_OBJECT;
    }

    S->State = TCP_STATE_SYN_SENT;

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    ListAddTail(&GTcpSockets, &S->Node);
    SpinLockReleaseIrq(&GTcpLock, F);

    TcpSendSyn(S);
    return SUCCESS;
}

TcpSocket *TcpAccept(TcpSocket *ListenSock) {
    if (!ListenSock || ListenSock->State != TCP_STATE_LISTEN) return NULLPTR;

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    if (ListEmpty(&ListenSock->AcceptQueue)) {
        SpinLockReleaseIrq(&GTcpLock, F);
        return NULLPTR;
    }
    TcpSocket *New = ListEntry(ListenSock->AcceptQueue.Next, TcpSocket, Node);
    ListDel(&New->Node);
    ListenSock->AcceptQueueLen--;
    ListAddTail(&GTcpSockets, &New->Node);
    SpinLockReleaseIrq(&GTcpLock, F);

    return New;
}

/*
 * ============================================================================
 * API: send / recv
 * ============================================================================
 */

INT TcpSend(TcpSocket *S, const UINT8 *Data, UINT32 Len) {
    if (!S || !Data || Len == 0) return INCORRECT_VALUE;
    if (S->State != TCP_STATE_ESTABLISHED &&
        S->State != TCP_STATE_CLOSE_WAIT) return INCORRECT_VALUE;

    UINT32 Off = 0;
    while (Off < Len) {
        UINT32 Chunk = Len - Off;
        if (Chunk > S->MssSend) Chunk = S->MssSend;

        NetworkBuf *Buf = NetworkAllocBuf(Chunk);
        if (!Buf) return NO_MEMORY;
        if (NetworkPut(Buf, Chunk) != SUCCESS) {
            NetworkFreeBuf(Buf);
            return NO_MEMORY;
        }
        MemCpy(Buf->Data, Data + Off, Chunk);

        TcpSegment *Seg = TcpSegAlloc(Buf, S->SndNxt, Chunk,
                                      TCP_FLAG_ACK | TCP_FLAG_PSH);
        if (!Seg) {
            NetworkFreeBuf(Buf);
            return NO_MEMORY;
        }

        UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
        ListAddTail(&S->SendQueue, &Seg->Node);
        S->SendQueueLen += Chunk;
        SpinLockReleaseIrq(&GTcpLock, F);

        S->SndNxt += Chunk;
        Off += Chunk;
    }

    TcpSendFromQueue(S);
    return (INT)Len;
}

INT TcpRecv(TcpSocket *S, UINT8 *Buf, UINT32 Len) {
    if (!S || !Buf) return NO_OBJECT;
    if (S->State == TCP_STATE_CLOSED) return INCORRECT_VALUE;

    UINT32 Copied = 0;

    while (Copied < Len) {
        UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
        if (ListEmpty(&S->RecvQueue)) {
            SpinLockReleaseIrq(&GTcpLock, F);
            break;
        }
        TcpSegment *Seg = ListEntry(S->RecvQueue.Next, TcpSegment, Node);
        ListDel(&Seg->Node);
        SpinLockReleaseIrq(&GTcpLock, F);

        /* FIX: use explicit ReadOffset; never mutate Seg->Buf->Data. */
        UINT32 Remaining = Seg->Len - Seg->ReadOffset;
        UINT32 ToCopy = (Remaining > (Len - Copied)) ? (Len - Copied) : Remaining;

        MemCpy(Buf + Copied, Seg->Buf->Data + Seg->ReadOffset, ToCopy);
        Seg->ReadOffset += ToCopy;
        Copied += ToCopy;

        if (Seg->ReadOffset >= Seg->Len) {
            TcpSegFree(Seg);
        } else {
            UINTPTR F2 = SpinLockAcquireIrq(&GTcpLock);
            ListAdd(&S->RecvQueue, &Seg->Node);
            SpinLockReleaseIrq(&GTcpLock, F2);
            break;
        }

        /* FIX: advance RcvNxt/RcvWnd once per consumed chunk. */
        UINTPTR F3 = SpinLockAcquireIrq(&GTcpLock);
        S->RcvNxt += ToCopy;
        S->RcvWnd += ToCopy;
        SpinLockReleaseIrq(&GTcpLock, F3);
    }

    return (INT)Copied;
}

/*
 * ============================================================================
 * API: close / abort
 * ============================================================================
 */

INT TcpClose(TcpSocket *S) {
    if (!S) return NO_OBJECT;

    switch (S->State) {
    case TCP_STATE_ESTABLISHED:
        S->State = TCP_STATE_FIN_WAIT_1;
        TcpSendFin(S);
        return SUCCESS;

    case TCP_STATE_CLOSE_WAIT:
        S->State = TCP_STATE_LAST_ACK;
        TcpSendFin(S);
        return SUCCESS;

    default:
        return INCORRECT_VALUE;
    }
}

INT TcpAbort(TcpSocket *S) {
    if (!S) return NO_OBJECT;

    if (S->State != TCP_STATE_CLOSED) TcpSendRst(S);

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    if (S->Node.Next && S->Node.Prev) ListDel(&S->Node);
    SpinLockReleaseIrq(&GTcpLock, F);

    TcpFlushQueue(&S->SendQueue);
    TcpFlushQueue(&S->RetransQueue);
    TcpFlushQueue(&S->RecvQueue);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &S->AcceptQueue) {
        TcpSocket *A = ListEntry(Pos, TcpSocket, Node);
        ListDel(&A->Node);
        TcpSocketDestroy(A);
    }

    MemoryFree(S);
    return SUCCESS;
}

/*
 * ============================================================================
 * Timer tick
 * ============================================================================
 */

VOID TcpTick(VOID) {
    UINT64 Now = TimerTicks();

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GTcpSockets) {
        TcpSocket *S = ListEntry(Pos, TcpSocket, Node);
        if (!S->TimerActive) continue;
        if (Now < S->TimerExpires) continue;

        switch (S->State) {
        case TCP_STATE_SYN_SENT:
        case TCP_STATE_SYN_RCVD:
            if (S->RetransCount >= TCP_MAX_RETRANSMIT) {
                if (S->ErrorCb) S->ErrorCb(S, TCP_ERROR_TIMEOUT);
                SpinLockReleaseIrq(&GTcpLock, F);
                TcpAbort(S);
                F = SpinLockAcquireIrq(&GTcpLock);
            } else {
                if (S->State == TCP_STATE_SYN_SENT) TcpSendSyn(S);
                else TcpSendSynAck(S);
            }
            break;

        case TCP_STATE_ESTABLISHED:
        case TCP_STATE_FIN_WAIT_1:
        case TCP_STATE_LAST_ACK:
            SpinLockReleaseIrq(&GTcpLock, F);
            TcpRetransmit(S);
            F = SpinLockAcquireIrq(&GTcpLock);
            break;

        case TCP_STATE_TIME_WAIT:
            if (Now >= S->StateExpires) {
                S->State = TCP_STATE_CLOSED;
                SpinLockReleaseIrq(&GTcpLock, F);
                TcpSocketDestroy(S);
                F = SpinLockAcquireIrq(&GTcpLock);
            }
            break;

        default:
            S->TimerActive = FALSE;
            break;
        }
    }
    SpinLockReleaseIrq(&GTcpLock, F);
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

STATIC TcpSocket *TcpFindSocket(UINT16 LocalPort, UINT16 RemotePort,
                                UINT32 LocalAddr, UINT32 RemoteAddr) {
    ListHead *Pos;

    /* Exact 5-tuple first. */
    ListForEach(Pos, &GTcpSockets) {
        TcpSocket *S = ListEntry(Pos, TcpSocket, Node);
        if (S->LocalPort == LocalPort &&
            S->RemotePort == RemotePort &&
            S->LocalAddr == LocalAddr &&
            S->RemoteAddr == RemoteAddr) {
            return S;
        }
    }

    /* Listener with wildcard or matching address. */
    ListForEach(Pos, &GTcpListeners) {
        TcpSocket *S = ListEntry(Pos, TcpSocket, Node);
        if (S->LocalPort == LocalPort &&
            (S->LocalAddr == 0 || S->LocalAddr == LocalAddr)) {
            return S;
        }
    }

    return NULLPTR;
}

STATIC VOID TcpSendResetFor(UINT32 SrcAddr, UINT32 DstAddr,
                            UINT16 SrcPort, UINT16 DstPort,
                            UINT32 Seq, UINT32 Ack) {
    NetworkBuf *Buf = NetworkAllocBuf(0);
    if (!Buf) return;
    /* FIX: no NetworkPut — NetworkReserve grows Len. */
    if (NetworkReserve(Buf, TCP_HEADER_MIN) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return;
    }

    TcpHeader *Tcp = (TcpHeader*)Buf->Data;
    MemSet(Tcp, 0, TCP_HEADER_MIN);
    Tcp->SrcPort  = Htons(DstPort);
    Tcp->DstPort  = Htons(SrcPort);
    Tcp->SeqNum   = Htonl(Seq);
    Tcp->AckNum   = Htonl(Ack);
    TCP_SET_DATA_OFFSET(Tcp, 5);
    TCP_SET_FLAGS(Tcp, TCP_FLAG_RST | TCP_FLAG_ACK);
    Tcp->Window   = 0;
    Tcp->Checksum = 0;

    UINT16 Csum = TcpChecksum(Tcp, TCP_HEADER_MIN, DstAddr, SrcAddr);
    Tcp->Checksum = Htons(Csum);

    IpV4Output(Buf, SrcAddr, DstAddr, IPV4_PROTO_TCP);
}

STATIC VOID TcpHandleSegment(TcpSocket *S, TcpHeader *Tcp,
                             UINT32 Seq, UINT32 Ack, UINT16 Flags,
                             UINT32 DataLen, UINT32 SrcAddr, UINT32 DstAddr,
                             NetworkBuf *Buf) {
    (VOID)DstAddr;
    S->LastRecv = TimerTicks();
    S->SndWnd   = Ntohs(Tcp->Window);

    UINT8 *Payload = (UINT8*)Tcp + TCP_GET_DATA_OFFSET(Tcp) * 4;

    switch (S->State) {
    case TCP_STATE_LISTEN: {
        if (!(Flags & TCP_FLAG_SYN)) break;

        TcpSocket *New = TcpSocketCreate();
        if (!New) break;

        New->LocalAddr  = S->LocalAddr;
        New->LocalPort  = S->LocalPort;
        New->RemoteAddr = SrcAddr;
        New->RemotePort = Ntohs(Tcp->SrcPort);

        New->Irs    = Seq;
        New->RcvNxt = Seq + 1;
        New->Iss    = __sync_fetch_and_add(&GTcpIss, 1000000);
        New->SndUna = New->Iss;
        New->SndNxt = New->Iss + 1;
        New->State  = TCP_STATE_SYN_RCVD;

        TcpSendSynAck(New);

        UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
        if (S->AcceptQueueLen < S->BackLog) {
            ListAddTail(&S->AcceptQueue, &New->Node);
            S->AcceptQueueLen++;
            SpinLockReleaseIrq(&GTcpLock, F);
        } else {
            SpinLockReleaseIrq(&GTcpLock, F);
            TcpSendRst(New);
            TcpSocketDestroy(New);
        }
        break;
    }

    case TCP_STATE_SYN_SENT:
        if ((Flags & TCP_FLAG_SYN) && (Flags & TCP_FLAG_ACK)) {
            S->RcvNxt = Seq + 1;
            S->Irs    = Seq;
            S->SndUna = Ack;

            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);

            S->State       = TCP_STATE_ESTABLISHED;
            S->TimerActive = FALSE;
            if (S->ConnectCb) S->ConnectCb(S, TRUE);
        } else if (Flags & TCP_FLAG_RST) {
            if (S->ConnectCb) S->ConnectCb(S, FALSE);
            TcpAbort(S);
            return;
        }
        break;

    case TCP_STATE_SYN_RCVD:
        if (Flags & TCP_FLAG_ACK) {
            S->State       = TCP_STATE_ESTABLISHED;
            S->TimerActive = FALSE;
            if (S->ConnectCb) S->ConnectCb(S, TRUE);
            TcpSendFromQueue(S);
        }
        break;

    case TCP_STATE_ESTABLISHED:
        if (Flags & TCP_FLAG_ACK) {
            TcpUpdateRtt(S, Ack);
            TcpAckData(S, Ack);
        }
        if (Flags & TCP_FLAG_FIN) {
            S->RcvNxt++;
            S->State = TCP_STATE_CLOSE_WAIT;
            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);
            if (S->CloseCb) S->CloseCb(S);
        }
        if (DataLen > 0) {
            NetworkBuf *Data = NetworkAllocBuf(DataLen);
            if (!Data) break;
            if (NetworkPut(Data, DataLen) != SUCCESS) {
                NetworkFreeBuf(Data);
                break;
            }
            MemCpy(Data->Data, Payload, DataLen);

            TcpSegment *Seg = TcpSegAlloc(Data, Seq, DataLen,
                                          TCP_FLAG_ACK | TCP_FLAG_PSH);
            if (!Seg) {
                NetworkFreeBuf(Data);
                break;
            }

            UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
            ListAddTail(&S->RecvQueue, &Seg->Node);
            S->RecvQueueLen += DataLen;
            SpinLockReleaseIrq(&GTcpLock, F);

            S->RcvNxt += DataLen;
            if (S->RcvWnd >= DataLen) S->RcvWnd -= DataLen;
            else S->RcvWnd = 0;

            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);
            if (S->RecvCb) S->RecvCb(S);
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if ((Flags & TCP_FLAG_FIN) && (Flags & TCP_FLAG_ACK)) {
            S->RcvNxt++;
            S->State        = TCP_STATE_TIME_WAIT;
            S->StateExpires = TimerTicks() + TimerMsToTicks(TCP_2MSL);
            S->TimerActive  = TRUE;
            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);
        } else if (Flags & TCP_FLAG_ACK) {
            S->State = TCP_STATE_FIN_WAIT_2;
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (Flags & TCP_FLAG_FIN) {
            S->RcvNxt++;
            S->State        = TCP_STATE_TIME_WAIT;
            S->StateExpires = TimerTicks() + TimerMsToTicks(TCP_2MSL);
            S->TimerActive  = TRUE;
            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);
        }
        break;

    case TCP_STATE_LAST_ACK:
        if (Flags & TCP_FLAG_ACK) {
            /* FIX: do not free S while its Node is still linked via
             * ListForEachSafe in TcpTick. Detach under the lock, then
             * let the caller (TcpHandleSegment) free it via return path. */
            UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
            if (S->Node.Next && S->Node.Prev) ListDel(&S->Node);
            SpinLockReleaseIrq(&GTcpLock, F);

            NetworkFreeBuf(Buf);
            TcpSocketDestroy(S);
            return;
        }
        break;

    case TCP_STATE_TIME_WAIT:
        if (Flags & TCP_FLAG_FIN) {
            TcpSendSegment(S, S->SndNxt, S->RcvNxt, TCP_FLAG_ACK,
                           NULLPTR, 0);
        }
        break;

    default:
        break;
    }

    NetworkFreeBuf(Buf);
}

VOID TcpRx(NetworkBuf *Buf, NetworkDevice *Dev,
           UINT32 SrcAddr, UINT32 DstAddr) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    if (Buf->Len < TCP_HEADER_MIN) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    TcpHeader *Tcp = (TcpHeader*)Buf->Data;
    UINT32 HdrLen = TCP_GET_DATA_OFFSET(Tcp) * 4;
    if (HdrLen < TCP_HEADER_MIN || HdrLen > Buf->Len) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    UINT16 Received = Tcp->Checksum;
    Tcp->Checksum = 0;
    UINT16 Computed = Htons(TcpChecksum(Tcp, Buf->Len, SrcAddr, DstAddr));
    Tcp->Checksum = Received;
    if (Computed != Received) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }
    Buf->Flags |= NETBUF_F_TCP_CSUM_OK;

    UINT32 Seq     = Ntohl(Tcp->SeqNum);
    UINT32 Ack     = Ntohl(Tcp->AckNum);
    UINT16 Flags   = TCP_GET_FLAGS(Tcp);
    UINT32 DataLen = Buf->Len - HdrLen;

    UINT16 SrcPort = Ntohs(Tcp->SrcPort);
    UINT16 DstPort = Ntohs(Tcp->DstPort);

    UINTPTR F = SpinLockAcquireIrq(&GTcpLock);
    TcpSocket *S = TcpFindSocket(DstPort, SrcPort, DstAddr, SrcAddr);
    SpinLockReleaseIrq(&GTcpLock, F);

    if (!S) {
        if (Flags & TCP_FLAG_ACK) {
            TcpSendResetFor(SrcAddr, DstAddr, SrcPort, DstPort, Ack, Seq + DataLen);
        }
        NetworkFreeBuf(Buf);
        return;
    }

    TcpHandleSegment(S, Tcp, Seq, Ack, Flags, DataLen,
                     SrcAddr, DstAddr, Buf);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT TcpInit(VOID) {
    if (GTcpInitialized) return SUCCESS;

    ListInit(&GTcpSockets);
    ListInit(&GTcpListeners);
    SpinLockInit(&GTcpLock);

    GTcpIss = 0x12345678;
    GTcpEphemeralPort = 49152;

    IpV4RegisterProtocol(IPV4_PROTO_TCP, TcpRx);

    GTcpInitialized = TRUE;
    StdOutPrintf("net: tcp initialized\n");
    return SUCCESS;
}