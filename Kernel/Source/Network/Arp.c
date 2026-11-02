#include <Network/Ethernet.h>
#include <Network/Network.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/Arp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Scheduler.h>
#include <SpinLock.h>
#include <Return.h>

STATIC ArpCacheEntry GCache[ARP_CACHE_SIZE];
STATIC ArpPending    GPending[ARP_PENDING_MAX];
STATIC SpinLock      GArpLock;

/* FIX: MAC of the device, resolved under the lock, snapshot for reply
 * transmission outside the lock. */

/*
 * ============================================================================
 * Cache
 * ============================================================================
 */

VOID ArpCacheClear(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    MemSet(GCache, 0, sizeof(GCache));
    SpinLockReleaseIrq(&GArpLock, F);
}

STATIC ArpCacheEntry *ArpCacheFindLocked(UINT32 Ip) {
    for (UINT32 I = 0; I < ARP_CACHE_SIZE; I++) {
        if (GCache[I].Valid && GCache[I].Ip == Ip) return &GCache[I];
    }
    return NULLPTR;
}

STATIC ArpCacheEntry *ArpCacheAllocLocked(VOID) {
    for (UINT32 I = 0; I < ARP_CACHE_SIZE; I++) {
        if (!GCache[I].Valid) return &GCache[I];
    }
    ArpCacheEntry *Oldest = &GCache[0];
    for (UINT32 I = 1; I < ARP_CACHE_SIZE; I++) {
        if (GCache[I].Timestamp < Oldest->Timestamp) Oldest = &GCache[I];
    }
    return Oldest;
}

VOID ArpCacheAdd(UINT32 Ip, const UINT8 *Mac) {
    if (!Mac) return;

    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    ArpCacheEntry *E = ArpCacheFindLocked(Ip);
    if (!E) E = ArpCacheAllocLocked();

    E->Ip        = Ip;
    MemCpy(E->Mac, Mac, NETWORK_ETH_ALEN);
    E->Timestamp = TimerTicks();
    E->Valid     = TRUE;
    SpinLockReleaseIrq(&GArpLock, F);
}

VOID ArpCacheDel(UINT32 Ip) {
    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    ArpCacheEntry *E = ArpCacheFindLocked(Ip);
    if (E) E->Valid = FALSE;
    SpinLockReleaseIrq(&GArpLock, F);
}

VOID ArpCachePurgeExpired(VOID) {
    UINT64 Now = TimerTicks();
    UINT64 TimeoutTicks = TimerMsToTicks(ARP_CACHE_TIMEOUT_MS);

    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    for (UINT32 I = 0; I < ARP_CACHE_SIZE; I++) {
        if (GCache[I].Valid && (Now - GCache[I].Timestamp) > TimeoutTicks) {
            GCache[I].Valid = FALSE;
        }
    }
    SpinLockReleaseIrq(&GArpLock, F);
}

BOOL ArpCacheLookup(UINT32 Ip, UINT8 *MacOut) {
    ArpCachePurgeExpired();

    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    ArpCacheEntry *E = ArpCacheFindLocked(Ip);
    if (E && E->Valid) {
        if (MacOut) MemCpy(MacOut, E->Mac, NETWORK_ETH_ALEN);
        SpinLockReleaseIrq(&GArpLock, F);
        return TRUE;
    }
    SpinLockReleaseIrq(&GArpLock, F);
    return FALSE;
}

/*
 * ============================================================================
 * Pending packet queue
 * ============================================================================
 */

INT ArpQueuePacket(NetworkDevice *Dev, UINT32 Ip, NetworkBuf *Buf) {
    if (!Dev || !Buf) return INCORRECT_VALUE;

    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    for (UINT32 I = 0; I < ARP_PENDING_MAX; I++) {
        if (GPending[I].Valid) continue;

        GPending[I].Ip        = Ip;
        GPending[I].Dev       = Dev;
        GPending[I].Buf       = Buf;
        GPending[I].ExpiresAt = TimerTicks() +
                                TimerMsToTicks(ARP_REQUEST_TIMEOUT_MS);
        GPending[I].Valid     = TRUE;

        SpinLockReleaseIrq(&GArpLock, F);
        return SUCCESS;
    }
    SpinLockReleaseIrq(&GArpLock, F);
    return NO_MEMORY;
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

INT ArpOutput(NetworkDevice *Dev, UINT16 Op, UINT32 TargetIp,
              const UINT8 *TargetMac) {
    if (!Dev) return NO_OBJECT;

    NetworkBuf *Buf = NetworkAllocBuf(sizeof(ArpHeader));
    if (!Buf) return NO_MEMORY;
    if (NetworkPut(Buf, sizeof(ArpHeader)) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    ArpHeader *Arp = (ArpHeader*)Buf->Data;
    MemSet(Arp, 0, sizeof(ArpHeader));

    Arp->Hrd = Htons(ARP_HRD_ETHERNET);
    Arp->Pro = Htons(ARP_PRO_IP);
    Arp->Hln = ARP_HLEN_ETHERNET;
    Arp->Pln = ARP_PLEN_IP;
    Arp->Op  = Htons(Op);

    MemCpy(Arp->Sha, Dev->MacAddr, NETWORK_ETH_ALEN);
    Arp->Spa = *(UINT32*)Dev->IpAddr;

    if (TargetMac) {
        MemCpy(Arp->Tha, TargetMac, NETWORK_ETH_ALEN);
    } else {
        MemSet(Arp->Tha, 0, NETWORK_ETH_ALEN);
    }
    Arp->Tpa = TargetIp;

    UINT8 DestMac[NETWORK_ETH_ALEN];
    if (TargetMac) {
        MemCpy(DestMac, TargetMac, NETWORK_ETH_ALEN);
    } else {
        MemCpy(DestMac, ETH_MAC_BROADCAST, NETWORK_ETH_ALEN);
    }

    return EthernetOutput(Buf, Dev, DestMac, ETH_P_ARP);
}

/*
 * ============================================================================
 * Resolve
 * ============================================================================
 */

INT ArpResolve(NetworkDevice *Dev, UINT32 Ip, UINT8 *MacOut) {
    if (!Dev || !MacOut) return INCORRECT_VALUE;

        if (ArpCacheLookup(Ntohl(Ip), MacOut)) return SUCCESS;

    INT Rc = ArpOutput(Dev, ARP_OP_REQUEST, Ip, NULLPTR);
    if (Rc != SUCCESS) return Rc;

    return NOT_FOUND;
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

VOID ArpInput(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    if (Buf->Len < sizeof(ArpHeader)) {
        NetworkFreeBuf(Buf);
        return;
    }

    ArpHeader *Arp = (ArpHeader*)Buf->Data;

    UINT16 Hrd = Ntohs(Arp->Hrd);
    UINT16 Pro = Ntohs(Arp->Pro);
    UINT16 Op  = Ntohs(Arp->Op);

    if (Hrd != ARP_HRD_ETHERNET || Pro != ARP_PRO_IP ||
        Arp->Hln != ARP_HLEN_ETHERNET || Arp->Pln != ARP_PLEN_IP) {
        NetworkFreeBuf(Buf);
        return;
    }

    UINT32 SenderIp     = Arp->Spa;                          /* host */
    UINT32 TargetIpHost = Arp->Tpa;                          /* host */
    UINT32 MyIpH        = Ntohl(*(UINT32*)Dev->IpAddr);      /* host */

    UINT8 SenderMac[NETWORK_ETH_ALEN];
    MemCpy(SenderMac, Arp->Sha, NETWORK_ETH_ALEN);

    if (SenderIp != 0) {
        ArpCacheAdd(SenderIp, SenderMac);
    }

    if (Op == ARP_OP_REQUEST) {
        if (TargetIpHost == MyIpH) {
            ArpOutput(Dev, ARP_OP_REPLY, Htonl(SenderIp), SenderMac);
        }
    } else if (Op == ARP_OP_REPLY) {
        /* FIX: collect all matching pending entries first, then flush
         * each one outside the lock. The previous version released
         * and re-acquired the lock inside the loop which is correct
         * but fragile (a spurious iteration could see the same slot
         * twice). */
        NetworkDevice *PendDev[ARP_PENDING_MAX];
        NetworkBuf    *PendBuf[ARP_PENDING_MAX];
        UINT8          PendMac[ARP_PENDING_MAX][NETWORK_ETH_ALEN];
        UINT32         PendCount = 0;

        UINTPTR F = SpinLockAcquireIrq(&GArpLock);
        for (UINT32 I = 0; I < ARP_PENDING_MAX && PendCount < ARP_PENDING_MAX; I++) {
            if (!GPending[I].Valid || GPending[I].Ip != SenderIp) continue;

            PendDev[PendCount] = GPending[I].Dev;
            PendBuf[PendCount] = GPending[I].Buf;
            MemCpy(PendMac[PendCount], SenderMac, NETWORK_ETH_ALEN);
            PendCount++;

            GPending[I].Valid = FALSE;
            GPending[I].Buf   = NULLPTR;
        }
        SpinLockReleaseIrq(&GArpLock, F);

        for (UINT32 I = 0; I < PendCount; I++) {
            if (PendBuf[I]) {
                EthernetOutput(PendBuf[I], PendDev[I], PendMac[I], ETH_P_IP);
            }
        }
    }

    NetworkFreeBuf(Buf);
}

/*
 * ============================================================================
 * Housekeeping
 * ============================================================================
 */

VOID ArpTick(VOID) {
    ArpCachePurgeExpired();

    UINT64 Now = TimerTicks();
    UINTPTR F = SpinLockAcquireIrq(&GArpLock);
    for (UINT32 I = 0; I < ARP_PENDING_MAX; I++) {
        if (GPending[I].Valid && Now >= GPending[I].ExpiresAt) {
            NetworkBuf *B = GPending[I].Buf;
            GPending[I].Valid = FALSE;
            GPending[I].Buf   = NULLPTR;

            /* FIX: free outside the lock — NetworkFreeBuf calls into
             * the heap allocator which has its own spinlock. */
            SpinLockReleaseIrq(&GArpLock, F);
            if (B) NetworkFreeBuf(B);
            F = SpinLockAcquireIrq(&GArpLock);
        }
    }
    SpinLockReleaseIrq(&GArpLock, F);
}

/*
 * ============================================================================
 * Init
 * ============================================================================
 */

VOID ArpInit(VOID) {
    MemSet(GCache,   0, sizeof(GCache));
    MemSet(GPending, 0, sizeof(GPending));
    SpinLockInit(&GArpLock);
    StdOutPrintf("net: arp initialized\n");
}