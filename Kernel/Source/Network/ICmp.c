#include <Mem/Allocator.h>
#include <Network/ICmp.h>
#include <Network/IpV4.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Scheduler.h>
#include <SpinLock.h>
#include <Return.h>

/*
 * ============================================================================
 * Ping request state
 * ============================================================================
 */

typedef struct PingRequest {
    UINT16       Id;
    UINT16       Seq;
    UINT32       Dest;
    UINT32       Remaining;
    UINT32       Total;
    UINT32       TimeoutMs;
    UINT64       LastSentTick;
    PingStats   *Stats;
    PingCallback Callback;
    VOID        *Arg;
    BOOL         Active;
    struct PingRequest *Next;
} PingRequest;

STATIC PingRequest *GPingRequests = NULLPTR;
STATIC UINT16       GNextPingId   = 0x1234;
STATIC SpinLock     GICmpLock;
STATIC BOOL         GICmpInitialized = FALSE;

/*
 * ============================================================================
 * String helpers
 * ============================================================================
 */

const CHAR *ICmpTypeString(UINT8 Type) {
    switch (Type) {
        case ICMP_TYPE_ECHO_REPLY:      return "Echo Reply";
        case ICMP_TYPE_DEST_UNREACH:    return "Destination Unreachable";
        case ICMP_TYPE_SOURCE_QUENCH:   return "Source Quench";
        case ICMP_TYPE_REDIRECT:        return "Redirect";
        case ICMP_TYPE_ECHO_REQUEST:    return "Echo Request";
        case ICMP_TYPE_TIME_EXCEEDED:   return "Time Exceeded";
        case ICMP_TYPE_PARAM_PROBLEM:   return "Parameter Problem";
        case ICMP_TYPE_TIMESTAMP:       return "Timestamp";
        case ICMP_TYPE_TIMESTAMP_REPLY: return "Timestamp Reply";
        default:                        return "Unknown";
    }
}

const CHAR *ICmpCodeString(UINT8 Type, UINT8 Code) {
    if (Type == ICMP_TYPE_DEST_UNREACH) {
        switch (Code) {
            case ICMP_CODE_NET_UNREACH:   return "Network Unreachable";
            case ICMP_CODE_HOST_UNREACH:  return "Host Unreachable";
            case ICMP_CODE_PROT_UNREACH:  return "Protocol Unreachable";
            case ICMP_CODE_PORT_UNREACH:  return "Port Unreachable";
            case ICMP_CODE_FRAG_NEEDED:   return "Fragmentation Needed";
            default:                      return "Unknown";
        }
    }
    if (Type == ICMP_TYPE_TIME_EXCEEDED) {
        switch (Code) {
            case ICMP_CODE_TTL_EXCEEDED: return "TTL Exceeded";
            case ICMP_CODE_REASSEMBLY:   return "Reassembly Time Exceeded";
            default:                     return "Unknown";
        }
    }
    return "";
}

/*
 * ============================================================================
 * Checksum (RFC 1071)
 * ============================================================================
 */

UINT16 ICmpChecksum(UINT16 *Data, UINT32 Len) {
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
 * Build and send an echo request
 * ============================================================================
 */

STATIC INT ICmpSendEchoRequest(UINT32 Dest, UINT16 Id, UINT16 Seq) {
    NetworkBuf *Buf = NetworkAllocBuf(sizeof(ICmpHeader) + ICMP_DATA_SIZE);
    if (!Buf) return NO_MEMORY;

    if (NetworkPut(Buf, sizeof(ICmpHeader) + ICMP_DATA_SIZE) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    ICmpHeader *ICmp = (ICmpHeader*)Buf->Data;
    MemSet(ICmp, 0, sizeof(ICmpHeader) + ICMP_DATA_SIZE);

    ICmp->Type     = ICMP_TYPE_ECHO_REQUEST;
    ICmp->Code     = 0;
    ICmp->Id       = Htons(Id);
    ICmp->Sequence = Htons(Seq);

    UINT64 NowUs = (TimerTicks() * 1000000ULL) / TimerFreq();
    UINT8 *Payload = (UINT8*)ICmp + sizeof(ICmpHeader);
    MemCpy(Payload, &NowUs, sizeof(NowUs));

    ICmp->Checksum = 0;
    ICmp->Checksum = Htons(ICmpChecksum((UINT16*)ICmp,
                                        sizeof(ICmpHeader) + ICMP_DATA_SIZE));

    return IpV4Output(Buf, Dest, IPV4_ANY, IPV4_PROTO_ICMP);
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

STATIC VOID ICmpHandleEchoReply(UINT16 Id, UINT16 Seq, UINT64 RttUs) {
    (VOID)Seq;
    UINTPTR F = SpinLockAcquireIrq(&GICmpLock);
    PingRequest *Req = GPingRequests;
    while (Req) {
        if (Req->Active && Req->Id == Id) {
            Req->Stats->Received++;
            if (RttUs < Req->Stats->MinRttUs || Req->Stats->MinRttUs == 0) {
                Req->Stats->MinRttUs = RttUs;
            }
            if (RttUs > Req->Stats->MaxRttUs) {
                Req->Stats->MaxRttUs = RttUs;
            }
            Req->Stats->TotalRttUs += RttUs;

            if (Req->Remaining > 0) Req->Remaining--;
            break;
        }
        Req = Req->Next;
    }
    SpinLockReleaseIrq(&GICmpLock, F);
}

VOID ICmpRx(NetworkBuf *Buf, NetworkDevice *Dev,
            UINT32 SrcAddr, UINT32 DstAddr) {
    (VOID)Dev;

    if (!Buf) return;

    if (Buf->Len < sizeof(ICmpHeader)) {
        NetworkFreeBuf(Buf);
        return;
    }

    ICmpHeader *ICmp = (ICmpHeader*)Buf->Data;

    UINT16 Received = ICmp->Checksum;
    ICmp->Checksum = 0;
    UINT16 Computed = Htons(ICmpChecksum((UINT16*)ICmp, Buf->Len));
    ICmp->Checksum = Received;
    if (Computed != Received) {
        NetworkFreeBuf(Buf);
        return;
    }

    switch (ICmp->Type) {
    case ICMP_TYPE_ECHO_REQUEST: {
        /* FIX: send the reply outside any lock; Buf itself is freed
         * by IpV4Output whether the reply succeeds or fails. */
        ICmp->Type = ICMP_TYPE_ECHO_REPLY;
        ICmp->Checksum = 0;
        ICmp->Checksum = Htons(ICmpChecksum((UINT16*)ICmp, Buf->Len));
        IpV4Output(Buf, SrcAddr, DstAddr, IPV4_PROTO_ICMP);
        return;
    }

    case ICMP_TYPE_ECHO_REPLY: {
        UINT16 Id  = Ntohs(ICmp->Id);
        UINT16 Seq = Ntohs(ICmp->Sequence);

        UINT64 NowUs = (TimerTicks() * 1000000ULL) / TimerFreq();
        UINT64 SentUs = 0;
        if (Buf->Len >= sizeof(ICmpHeader) + sizeof(UINT64)) {
            MemCpy(&SentUs, (UINT8*)ICmp + sizeof(ICmpHeader), sizeof(SentUs));
        }
        UINT64 RttUs = (NowUs > SentUs) ? (NowUs - SentUs) : 0;

        ICmpHandleEchoReply(Id, Seq, RttUs);
        NetworkFreeBuf(Buf);
        return;
    }

    case ICMP_TYPE_DEST_UNREACH:
    case ICMP_TYPE_TIME_EXCEEDED:
    default:
        NetworkFreeBuf(Buf);
        return;
    }
}

/*
 * ============================================================================
 * Output (arbitrary message)
 * ============================================================================
 */

INT ICmpOutput(NetworkBuf *Buf, UINT32 DestAddr, UINT32 SrcAddr,
               UINT8 Type, UINT8 Code) {
    if (!Buf || Buf->Len < sizeof(ICmpHeader)) {
        if (Buf) NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    ICmpHeader *ICmp = (ICmpHeader*)Buf->Data;
    ICmp->Type     = Type;
    ICmp->Code     = Code;
    ICmp->Checksum = 0;
    ICmp->Checksum = Htons(ICmpChecksum((UINT16*)ICmp, Buf->Len));

    return IpV4Output(Buf, DestAddr, SrcAddr, IPV4_PROTO_ICMP);
}

/*
 * ============================================================================
 * Blocking ping
 * ============================================================================
 */

INT ICmpPing(UINT32 DestAddr, UINT32 Count, UINT32 TimeoutMs,
             PingStats *Stats) {
    if (!Stats || Count == 0) return INCORRECT_VALUE;

    CHAR DestStr[16];
    IpV4NTop(DestAddr, DestStr, sizeof(DestStr));

    MemSet(Stats, 0, sizeof(PingStats));
    Stats->MinRttUs = UINT64MAX;

    UINT16 Id = __sync_fetch_and_add(&GNextPingId, 1);

    PingRequest *Req = (PingRequest*)MemoryAllocate(sizeof(PingRequest));
    if (!Req) return NO_MEMORY;
    MemSet(Req, 0, sizeof(PingRequest));
    Req->Id           = Id;
    Req->Dest         = DestAddr;
    Req->Remaining    = Count;
    Req->Total        = Count;
    Req->TimeoutMs    = TimeoutMs;
    Req->LastSentTick = TimerTicks();
    Req->Stats        = Stats;
    Req->Active       = TRUE;

    UINTPTR F = SpinLockAcquireIrq(&GICmpLock);
    Req->Next = GPingRequests;
    GPingRequests = Req;
    SpinLockReleaseIrq(&GICmpLock, F);

    for (UINT32 I = 0; I < Count; I++) {
        UINT16 Seq = (UINT16)(I + 1);

        Stats->Sent++;
        ICmpSendEchoRequest(DestAddr, Id, Seq);

        UINT64 Deadline = TimerTicks() + TimerMsToTicks(TimeoutMs);
        BOOL Got = FALSE;

        while (TimerTicks() < Deadline) {
            F = SpinLockAcquireIrq(&GICmpLock);
            BOOL Have = (Stats->Received > I);
            SpinLockReleaseIrq(&GICmpLock, F);
            if (Have) { Got = TRUE; break; }
            TimerSleep(1);
        }

        if (Got) {
            UINT32 RttMs = (UINT32)(Stats->TotalRttUs / Stats->Received / 1000);
            StdOutPrintf("%u bytes from %s: icmp_seq=%u time=%u.%03u ms\n",
                         ICMP_DATA_SIZE + 8, DestStr, Seq,
                         RttMs / 1000, RttMs % 1000);
        } else {
            StdOutPrintf("Request timeout for icmp_seq=%u\n", Seq);
        }
    }

    F = SpinLockAcquireIrq(&GICmpLock);
    PingRequest **Prev = &GPingRequests;
    while (*Prev && *Prev != Req) Prev = &(*Prev)->Next;
    if (*Prev == Req) *Prev = Req->Next;
    SpinLockReleaseIrq(&GICmpLock, F);
    MemoryFree(Req);

    Stats->Lost = Stats->Sent - Stats->Received;
    Stats->AvgRttUs = (Stats->Received > 0)
                      ? (Stats->TotalRttUs / Stats->Received) : 0;

    StdOutPrintf("\n--- %s ping statistics ---\n", DestStr);
    StdOutPrintf("%u transmitted, %u received, %u%% loss\n",
                 Stats->Sent, Stats->Received,
                 Stats->Sent ? (Stats->Lost * 100 / Stats->Sent) : 0);
    if (Stats->Received > 0) {
        StdOutPrintf("rtt min/avg/max = %llu/%llu/%llu us\n",
                     Stats->MinRttUs, Stats->AvgRttUs, Stats->MaxRttUs);
    }

    return SUCCESS;
}

/*
 * ============================================================================
 * Async ping
 * ============================================================================
 */

INT ICmpPingAsync(UINT32 DestAddr, UINT32 Count, UINT32 TimeoutMs,
                  PingCallback Callback, VOID *Arg) {
    if (!Callback || Count == 0) return INCORRECT_VALUE;

    PingStats *Stats = (PingStats*)MemoryAllocate(sizeof(PingStats));
    if (!Stats) return NO_MEMORY;
    MemSet(Stats, 0, sizeof(PingStats));
    Stats->MinRttUs = UINT64MAX;

    PingRequest *Req = (PingRequest*)MemoryAllocate(sizeof(PingRequest));
    if (!Req) {
        MemoryFree(Stats);
        return NO_MEMORY;
    }
    MemSet(Req, 0, sizeof(PingRequest));

    UINT16 Id = __sync_fetch_and_add(&GNextPingId, 1);

    Req->Id           = Id;
    Req->Seq          = 1;
    Req->Dest         = DestAddr;
    Req->Remaining    = Count;
    Req->Total        = Count;
    Req->TimeoutMs    = TimeoutMs;
    Req->LastSentTick = TimerTicks();
    Req->Stats        = Stats;
    Req->Callback     = Callback;
    Req->Arg          = Arg;
    Req->Active       = TRUE;

    UINTPTR F = SpinLockAcquireIrq(&GICmpLock);
    Req->Next = GPingRequests;
    GPingRequests = Req;
    SpinLockReleaseIrq(&GICmpLock, F);

    ICmpSendEchoRequest(DestAddr, Id, 1);
    Stats->Sent = 1;

    return SUCCESS;
}

VOID ICmpPingCancel(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GICmpLock);
    PingRequest *Req = GPingRequests;
    while (Req) {
        Req->Active = FALSE;
        Req = Req->Next;
    }
    SpinLockReleaseIrq(&GICmpLock, F);
}

/*
 * ============================================================================
 * Periodic timeout handling
 * ============================================================================
 */

VOID ICmpTick(VOID) {
    UINT64 Now = TimerTicks();

    UINTPTR F = SpinLockAcquireIrq(&GICmpLock);
    PingRequest *Req = GPingRequests;
    PingRequest *Prev = NULLPTR;

    while (Req) {
        if (!Req->Active) {
            Prev = Req;
            Req = Req->Next;
            continue;
        }

        UINT64 ElapsedMs = (Now - Req->LastSentTick) * 1000 / TimerFreq();

        if (ElapsedMs >= Req->TimeoutMs) {
            if (Req->Stats) Req->Stats->Lost++;
            Req->Remaining--;
            Req->Seq++;
            Req->LastSentTick = Now;

            if (Req->Remaining > 0) {
                /* FIX: copy destination and id under the lock so that
                 * another CPU cannot free Req between release and use. */
                UINT32 Dest = Req->Dest;
                UINT16 Id   = Req->Id;
                UINT16 Seq  = Req->Seq;
                PingStats *St = Req->Stats;

                SpinLockReleaseIrq(&GICmpLock, F);
                ICmpSendEchoRequest(Dest, Id, Seq);
                if (St) St->Sent++;
                F = SpinLockAcquireIrq(&GICmpLock);
            }
        }

        if (Req->Remaining == 0) {
            if (Req->Stats && Req->Stats->Received > 0) {
                Req->Stats->AvgRttUs =
                    Req->Stats->TotalRttUs / Req->Stats->Received;
            }
            PingCallback Cb = Req->Callback;
            PingStats *St   = Req->Stats;
            VOID *Arg       = Req->Arg;

            if (Prev) Prev->Next = Req->Next;
            else      GPingRequests = Req->Next;

            SpinLockReleaseIrq(&GICmpLock, F);
            MemoryFree(Req);
            if (Cb) Cb(St, Arg);
            /* FIX: free Stats only for async pings. For blocking pings
             * the caller owns Stats. */
            if (Cb) MemoryFree(St);

            F = SpinLockAcquireIrq(&GICmpLock);
            Req = Prev ? Prev->Next : GPingRequests;
            continue;
        }

        Prev = Req;
        Req = Req->Next;
    }

    SpinLockReleaseIrq(&GICmpLock, F);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT ICmpInit(VOID) {
    if (GICmpInitialized) return SUCCESS;

    GPingRequests = NULLPTR;
    GNextPingId = 0x1234;
    SpinLockInit(&GICmpLock);

    IpV4RegisterProtocol(IPV4_PROTO_ICMP, ICmpRx);

    GICmpInitialized = TRUE;
    StdOutPrintf("net: icmp initialized\n");
    return SUCCESS;
}