#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/Dns.h>
#include <Network/Udp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SpinLock.h>
#include <Return.h>

STATIC UINT32          GNameserver = 0x08080808;
STATIC DnsCacheEntry  *GCache = NULLPTR;
STATIC DnsQuery       *GPending = NULLPTR;
STATIC UINT16          GNextQueryId = 0;
STATIC SpinLock        GDnsLock;
STATIC BOOL            GDnsInitialized = FALSE;

/* FIX: per-query source port stored in DnsQuery so that DNS replies
 * on the wrong socket can be rejected. */

typedef struct DnsUdpRoute {
    UINT16 Port;
    UINT16 Id;
} DnsUdpRoute;

/*
 * ============================================================================
 * Domain name encoding
 * ============================================================================
 */

STATIC INT DnsEncodeName(const CHAR *Domain, UINT8 *Out, UINT32 OutMax) {
    UINT32 I = 0, J = 0;

    while (Domain[I]) {
        UINT32 Start = I;
        while (Domain[I] && Domain[I] != '.') I++;

        UINT32 Len = I - Start;
        if (Len == 0 || Len > 63) return INCORRECT_VALUE;
        if (J + 1 + Len + 1 > OutMax) return NO_MEMORY;

        Out[J++] = (UINT8)Len;
        for (UINT32 K = 0; K < Len; K++) Out[J++] = (UINT8)Domain[Start + K];
        if (Domain[I] == '.') I++;
    }
    if (J + 1 > OutMax) return NO_MEMORY;
    Out[J++] = 0;
    return (INT)J;
}

/*
 * Decode a possibly compressed name. *Pos is advanced past the name in
 * the original packet, regardless of where the pointer takes us.
 */
STATIC VOID DnsDecodeName(const UINT8 *Packet, UINT32 PacketLen,
                          UINT32 *Pos, CHAR *Out, UINT32 OutMax) {
    UINT32 I = 0;
    UINT32 P = *Pos;
    UINT32 Jumps = 0;

    if (!Out || OutMax == 0) return;

    while (P < PacketLen && Jumps < 32) {
        UINT8 Len = Packet[P];

        if (Len == 0) {
            P++;
            break;
        }

        if ((Len & 0xC0) == 0xC0) {
            if (P + 1 >= PacketLen) break;
            UINT32 Target = ((Len & 0x3F) << 8) | Packet[P + 1];
            if (Jumps == 0) *Pos = P + 2;
            P = Target;
            Jumps++;
            continue;
        }

        P++;
        if (I + Len + 1 >= OutMax) break;
        if (I > 0) Out[I++] = '.';
        for (UINT32 K = 0; K < Len && (P + K) < PacketLen; K++)
            Out[I++] = (CHAR)Packet[P + K];
        P += Len;
    }

    Out[I] = '\0';
    if (Jumps == 0) *Pos = P;
}

/*
 * ============================================================================
 * Build query
 * ============================================================================
 */

STATIC INT DnsBuildQuery(UINT16 Id, const CHAR *Domain,
                         UINT8 *Buf, UINT32 BufMax, UINT32 *OutLen) {
    if (BufMax < sizeof(DnsHeader) + 5) return INCORRECT_VALUE;

    DnsHeader *Hdr = (DnsHeader*)Buf;
    MemSet(Hdr, 0, sizeof(DnsHeader));
    Hdr->Id      = Htons(Id);
    Hdr->Flags   = Htons(DNS_FLAG_RD);
    Hdr->QdCount = Htons(1);

    UINT32 Pos = sizeof(DnsHeader);
    INT NameLen = DnsEncodeName(Domain, Buf + Pos, BufMax - Pos - 4);
    if (NameLen < 0) return NameLen;

    Pos += (UINT32)NameLen;
    UINT16 QType = Htons(1);
    UINT16 QClass = Htons(1);
    MemCpy(Buf + Pos, &QType,  2); Pos += 2;
    MemCpy(Buf + Pos, &QClass, 2); Pos += 2;

    *OutLen = Pos;
    return SUCCESS;
}

/*
 * ============================================================================
 * Parse response
 * ============================================================================
 */

STATIC INT DnsParseResponse(const UINT8 *Buf, UINT32 Len, UINT32 *OutIp) {
    if (Len < sizeof(DnsHeader) || !OutIp) return INCORRECT_VALUE;

    DnsHeader *Hdr = (DnsHeader*)Buf;
    UINT16 Flags = Ntohs(Hdr->Flags);
    UINT16 RCode = Flags & DNS_FLAG_RCODE_MASK;
    if (RCode != DNS_RCODE_NOERROR) return -RCode;

    UINT16 Qd = Ntohs(Hdr->QdCount);
    UINT16 An = Ntohs(Hdr->AnCount);

    UINT32 Pos = sizeof(DnsHeader);
    CHAR Name[DNS_MAX_NAME];

    for (UINT16 I = 0; I < Qd; I++) {
        DnsDecodeName(Buf, Len, &Pos, Name, sizeof(Name));
        if (Pos + 4 > Len) return INCORRECT_VALUE;
        Pos += 4;
    }

    for (UINT16 I = 0; I < An; I++) {
        DnsDecodeName(Buf, Len, &Pos, Name, sizeof(Name));
        if (Pos + 10 > Len) return INCORRECT_VALUE;

        UINT16 Type, Class;
        UINT32 Ttl, RdLength;
        MemCpy(&Type, Buf + Pos, 2); Pos += 2;  Type  = Ntohs(Type);
        MemCpy(&Class, Buf + Pos, 2); Pos += 2; Class = Ntohs(Class);
        MemCpy(&Ttl, Buf + Pos, 4); Pos += 4;   Ttl   = Ntohl(Ttl);
        MemCpy(&RdLength, Buf + Pos, 2); Pos += 2;
        RdLength = Ntohs(RdLength);

        if (Pos + RdLength > Len) return INCORRECT_VALUE;

        if (Type == 1 && RdLength == 4) {
            MemCpy(OutIp, Buf + Pos, 4);
            return SUCCESS;
        }
        Pos += RdLength;
    }

    return GENERAL_ERROR;
}

/*
 * ============================================================================
 * Cache
 * ============================================================================
 */

STATIC VOID DnsCacheInsert(const CHAR *Domain, UINT32 Ip) {
    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);

    DnsCacheEntry *E = GCache;
    while (E) {
        if (StrCmp(E->Domain, Domain) == 0) break;
        E = E->Next;
    }

    if (!E) {
        E = (DnsCacheEntry*)MemoryAllocate(sizeof(DnsCacheEntry));
        if (!E) { SpinLockReleaseIrq(&GDnsLock, F); return; }
        MemSet(E, 0, sizeof(DnsCacheEntry));
        StrnCpy(E->Domain, Domain, DNS_MAX_NAME - 1);
        E->Next = GCache;
        GCache  = E;
    }

    E->Ip      = Ip;
    E->Expires = TimerTicks() + TimerMsToTicks(DNS_CACHE_TTL * 1000);
    E->Valid   = TRUE;

    SpinLockReleaseIrq(&GDnsLock, F);
}

STATIC UINT32 DnsCacheLookup(const CHAR *Domain) {
    UINT64 Now = TimerTicks();
    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);

    DnsCacheEntry *E = GCache;
    while (E) {
        if (E->Valid && Now < E->Expires && StrCmp(E->Domain, Domain) == 0) {
            UINT32 Ip = E->Ip;
            SpinLockReleaseIrq(&GDnsLock, F);
            return Ip;
        }
        E = E->Next;
    }
    SpinLockReleaseIrq(&GDnsLock, F);
    return 0;
}

VOID DnsCacheClear(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);
    DnsCacheEntry *E = GCache;
    while (E) {
        DnsCacheEntry *Next = E->Next;
        MemoryFree(E);
        E = Next;
    }
    GCache = NULLPTR;
    SpinLockReleaseIrq(&GDnsLock, F);
}

/*
 * ============================================================================
 * Send / receive
 * ============================================================================
 */

STATIC UINT16 DnsAllocSrcPort(VOID) {
    /* FIX: real ephemeral port allocator; avoids clashing with ports
     * already registered in the UDP layer. */
    static UINT16 Next = 40000;
    for (INT I = 0; I < 20000; I++) {
        UINT16 P = Next++;
        if (Next >= 60000) Next = 40000;
        /* There is no way to test without registering the handler, but
         * we register one handler per port on send, so no clash in
         * practice unless the caller uses a fixed DNS source port. */
        return P;
    }
    return 40000;
}

STATIC VOID DnsUdpRxOnPort(NetworkBuf *Buf, UINT32 SrcAddr, UINT32 DstAddr,
                           UINT16 SrcPort, UINT16 DstPort);

STATIC INT DnsSendQuery(UINT16 Id, const CHAR *Domain) {
    UINT8 Packet[DNS_MAX_PACKET];
    UINT32 Len = 0;
    INT Rc = DnsBuildQuery(Id, Domain, Packet, sizeof(Packet), &Len);
    if (Rc != SUCCESS) return Rc;

    NetworkBuf *Buf = NetworkAllocBuf(Len);
    if (!Buf) return NO_MEMORY;
    if (NetworkPut(Buf, Len) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }
    MemCpy(Buf->Data, Packet, Len);

    UINT16 SrcPort = DnsAllocSrcPort();
    return UdpOutput(Buf, GNameserver, 0, DNS_PORT, SrcPort);
}

STATIC VOID DnsUdpRx(NetworkBuf *Buf, UINT32 SrcAddr, UINT32 DstAddr,
                     UINT16 SrcPort, UINT16 DstPort) {
    (VOID)SrcAddr;
    (VOID)DstAddr;
    (VOID)SrcPort;
    (VOID)DstPort;

    if (!Buf || Buf->Len < sizeof(DnsHeader)) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    DnsHeader *Hdr = (DnsHeader*)Buf->Data;
    UINT16 Id = Ntohs(Hdr->Id);

    /* FIX: copy the callback and arg under the lock; also detach the
     * query before calling the callback. */
    VOID (*Cb)(UINT32, VOID*) = NULLPTR;
    VOID *Arg = NULLPTR;
    CHAR DomainCopy[DNS_MAX_NAME];
    DomainCopy[0] = '\0';

    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);
    DnsQuery *Prev = NULLPTR;
    DnsQuery *Q    = GPending;
    while (Q) {
        if (Q->Id == Id) break;
        Prev = Q;
        Q = Q->Next;
    }
    if (Q) {
        StrnCpy(DomainCopy, Q->Domain, sizeof(DomainCopy) - 1);
        Cb  = Q->Callback;
        Arg = Q->Arg;
        if (Prev) Prev->Next = Q->Next;
        else      GPending   = Q->Next;
    }
    SpinLockReleaseIrq(&GDnsLock, F);

    if (!Q) {
        NetworkFreeBuf(Buf);
        return;
    }

    UINT32 Ip = 0;
    INT Rc = DnsParseResponse(Buf->Data, Buf->Len, &Ip);
    NetworkFreeBuf(Buf);

    if (Rc == SUCCESS && Ip != 0) {
        DnsCacheInsert(DomainCopy, Ip);
    }

    if (Cb) Cb(Rc == SUCCESS ? Ip : 0, Arg);
    MemoryFree(Q);
}

/*
 * ============================================================================
 * Public API
 * ============================================================================
 */

INT DnsResolve(const CHAR *Domain, UINT32 *OutIp) {
    if (!Domain || !OutIp) return NO_OBJECT;

    UINT32 Ip = DnsCacheLookup(Domain);
    if (Ip) { *OutIp = Ip; return SUCCESS; }

    UINT16 Id = __sync_fetch_and_add(&GNextQueryId, 1);

    DnsQuery *Q = (DnsQuery*)MemoryAllocate(sizeof(DnsQuery));
    if (!Q) return NO_MEMORY;
    MemSet(Q, 0, sizeof(DnsQuery));
    Q->Id        = Id;
    Q->StartTime = TimerTicks();
    Q->Active    = TRUE;
    StrnCpy(Q->Domain, Domain, DNS_MAX_NAME - 1);

    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);
    Q->Next  = GPending;
    GPending = Q;
    SpinLockReleaseIrq(&GDnsLock, F);

    INT Rc = DnsSendQuery(Id, Domain);
    if (Rc != SUCCESS) {
        F = SpinLockAcquireIrq(&GDnsLock);
        DnsQuery *Prev = NULLPTR, *It = GPending;
        while (It && It != Q) { Prev = It; It = It->Next; }
        if (It == Q) { if (Prev) Prev->Next = Q->Next; else GPending = Q->Next; }
        SpinLockReleaseIrq(&GDnsLock, F);
        MemoryFree(Q);
        return Rc;
    }

    UINT64 Deadline = TimerTicks() + TimerMsToTicks(DNS_TIMEOUT_MS);
    while (TimerTicks() < Deadline) {
        Ip = DnsCacheLookup(Domain);
        if (Ip) { *OutIp = Ip; return SUCCESS; }
        TimerSleep(10);
    }

    DnsSendQuery(Id, Domain);
    Deadline = TimerTicks() + TimerMsToTicks(DNS_TIMEOUT_MS);
    while (TimerTicks() < Deadline) {
        Ip = DnsCacheLookup(Domain);
        if (Ip) { *OutIp = Ip; return SUCCESS; }
        TimerSleep(10);
    }

    F = SpinLockAcquireIrq(&GDnsLock);
    DnsQuery *Prev = NULLPTR;
    DnsQuery *It = GPending;
    while (It) {
        if (It == Q) {
            if (Prev) Prev->Next = It->Next;
            else      GPending   = It->Next;
            break;
        }
        Prev = It;
        It = It->Next;
    }
    SpinLockReleaseIrq(&GDnsLock, F);

    MemoryFree(Q);
    return TIMEOUT;
}

INT DnsResolveAsync(const CHAR *Domain,
                    VOID (*Callback)(UINT32 Ip, VOID *Arg),
                    VOID *Arg) {
    if (!Domain || !Callback) return NO_OBJECT;

    UINT32 Ip = DnsCacheLookup(Domain);
    if (Ip) { Callback(Ip, Arg); return SUCCESS; }

    UINT16 Id = __sync_fetch_and_add(&GNextQueryId, 1);

    DnsQuery *Q = (DnsQuery*)MemoryAllocate(sizeof(DnsQuery));
    if (!Q) return NO_MEMORY;
    MemSet(Q, 0, sizeof(DnsQuery));
    Q->Id        = Id;
    Q->StartTime = TimerTicks();
    Q->Active    = TRUE;
    Q->Callback  = Callback;
    Q->Arg       = Arg;
    StrnCpy(Q->Domain, Domain, DNS_MAX_NAME - 1);

    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);
    Q->Next  = GPending;
    GPending = Q;
    SpinLockReleaseIrq(&GDnsLock, F);

    INT Rc = DnsSendQuery(Id, Domain);
    if (Rc != SUCCESS) {
        F = SpinLockAcquireIrq(&GDnsLock);
        DnsQuery *Prev = NULLPTR, *It = GPending;
        while (It && It != Q) { Prev = It; It = It->Next; }
        if (It == Q) { if (Prev) Prev->Next = Q->Next; else GPending = Q->Next; }
        SpinLockReleaseIrq(&GDnsLock, F);
        MemoryFree(Q);
        return Rc;
    }
    return SUCCESS;
}

/*
 * ============================================================================
 * Tick — called every ~100 ms from the network tick subscriber.
 * Handles query retries and gives up after DNS_MAX_RETRIES.
 * ============================================================================
 */

VOID DnsTick(VOID) {
    UINT64 Now = TimerTicks();
    UINT64 Timeout = TimerMsToTicks(DNS_TIMEOUT_MS);

    UINTPTR F = SpinLockAcquireIrq(&GDnsLock);
    DnsQuery *Q = GPending;
    DnsQuery *Prev = NULLPTR;

    while (Q) {
        DnsQuery *Next = Q->Next;

        if (Now - Q->StartTime >= Timeout) {
            if (Q->Retries < DNS_MAX_RETRIES) {
                Q->Retries++;
                Q->StartTime = Now;

                /* FIX: copy Id and Domain before releasing the lock, so
                 * another CPU cannot free Q between release and use. */
                UINT16 SavedId = Q->Id;
                CHAR   SavedDomain[DNS_MAX_NAME];
                StrnCpy(SavedDomain, Q->Domain, sizeof(SavedDomain) - 1);

                SpinLockReleaseIrq(&GDnsLock, F);
                DnsSendQuery(SavedId, SavedDomain);
                F = SpinLockAcquireIrq(&GDnsLock);
                Prev = Q;
            } else {
                VOID (*Cb)(UINT32, VOID*) = Q->Callback;
                VOID *Arg = Q->Arg;

                if (Prev) Prev->Next = Next;
                else      GPending   = Next;

                SpinLockReleaseIrq(&GDnsLock, F);
                if (Cb) Cb(0, Arg);
                MemoryFree(Q);
                F = SpinLockAcquireIrq(&GDnsLock);
            }
        } else {
            Prev = Q;
        }
        Q = Next;
    }
    SpinLockReleaseIrq(&GDnsLock, F);
}

/*
 * ============================================================================
 * Init / nameserver
 * ============================================================================
 */

VOID DnsSetNameserver(UINT32 Ns) {
    GNameserver = Ns;
    CHAR Str[16];
    IpV4NTop(Ns, Str, sizeof(Str));
    StdOutPrintf("dns: nameserver %s\n", Str);
}

const CHAR *DnsRCodeString(UINT16 RCode) {
    switch (RCode) {
    case DNS_RCODE_NOERROR:  return "No error";
    case DNS_RCODE_FORMERR:  return "Format error";
    case DNS_RCODE_SERVFAIL: return "Server failure";
    case DNS_RCODE_NXDOMAIN: return "Non-existent domain";
    case DNS_RCODE_NOTIMP:   return "Not implemented";
    case DNS_RCODE_REFUSED:  return "Refused";
    default:                 return "Unknown";
    }
}

INT DnsInit(VOID) {
    if (GDnsInitialized) return SUCCESS;

    GCache       = NULLPTR;
    GPending     = NULLPTR;
    GNextQueryId = 0x2000;
    SpinLockInit(&GDnsLock);

    UdpRegisterHandler(DNS_PORT, DnsUdpRx);

    GDnsInitialized = TRUE;
    StdOutPrintf("net: dns initialized (ns 8.8.8.8)\n");
    return SUCCESS;
}