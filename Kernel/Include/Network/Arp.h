#pragma once

#include <Network/Ethernet.h>
#include <Network/Network.h>
#include <Network/IpV4.h>
#include <SpinLock.h>
#include <Types.h>
#include <List.h>

/*
 * ============================================================================
 * ARP (RFC 826) — IPv4 over Ethernet only.
 *
 * Cache: fixed-size, least-recently-used eviction, entries age out
 * after ARP_CACHE_TIMEOUT_MS. Pending requests: packets that could not
 * be sent because the MAC was unknown. They are queued here, an ARP
 * request is broadcast, and the queued packets are flushed when the
 * reply arrives.
 *
 * This is the minimum a host needs to talk on a LAN. We do not
 * implement gratuitous ARP, proxy ARP, or RARP.
 * ============================================================================
 */

#define ARP_HRD_ETHERNET    0x0001
#define ARP_PRO_IP          0x0800
#define ARP_HLEN_ETHERNET   6
#define ARP_PLEN_IP         4

#define ARP_OP_REQUEST      0x0001
#define ARP_OP_REPLY        0x0002

typedef struct ATTRIBUTE(packed) ArpHeader {
    UINT16 Hrd;
    UINT16 Pro;
    UINT8  Hln;
    UINT8  Pln;
    UINT16 Op;
    UINT8  Sha[NETWORK_ETH_ALEN];
    UINT32 Spa;
    UINT8  Tha[NETWORK_ETH_ALEN];
    UINT32 Tpa;
} ArpHeader;

/*
 * ============================================================================
 * Configuration
 * ============================================================================
 */

#define ARP_CACHE_SIZE          32
#define ARP_CACHE_TIMEOUT_MS    300000   /* 5 minutes */
#define ARP_PENDING_MAX         8
#define ARP_REQUEST_TIMEOUT_MS  1000

typedef struct ArpCacheEntry {
    UINT32  Ip;
    UINT8   Mac[NETWORK_ETH_ALEN];
    UINT64  Timestamp;
    BOOL    Valid;
} ArpCacheEntry;

typedef struct ArpPending {
    UINT32        Ip;
    NetworkDevice *Dev;
    NetworkBuf   *Buf;
    UINT64        ExpiresAt;
    BOOL          Valid;
} ArpPending;

/*
 * ============================================================================
 * API — matching the XxxInput / XxxOutput style used by the rest of
 * the stack.
 * ============================================================================
 */

VOID ArpInit(VOID);

/*
 * ArpInput — called by EthernetInput for ETH_P_ARP frames.
 * Consumes Buf.
 */
VOID ArpInput(NetworkBuf *Buf, NetworkDevice *Dev);

/*
 * ArpOutput — build and send a single ARP packet. Not usually called
 * by upper layers (use ArpResolve instead), but exposed so that a
 * future gratuitous-ARP path can reuse the framing.
 */
INT ArpOutput(NetworkDevice *Dev, UINT16 Op, UINT32 TargetIp,
              const UINT8 *TargetMac);

/*
 * ArpResolve — look up Ip in the cache.
 *
 * Returns SUCCESS and fills MacOut if the entry is valid.
 * Returns NOT_FOUND if the MAC is unknown; in that case an ARP request
 * has been broadcast and the caller should either retry later or queue
 * the packet with ArpQueuePacket.
 */
INT ArpResolve(NetworkDevice *Dev, UINT32 Ip, UINT8 *MacOut);

/*
 * ArpQueuePacket — hold Buf until the MAC for Ip is known. Ownership
 * of Buf is transferred to the ARP layer.
 */
INT ArpQueuePacket(NetworkDevice *Dev, UINT32 Ip, NetworkBuf *Buf);

/*
 * Cache helpers.
 */
VOID ArpCacheAdd(UINT32 Ip, const UINT8 *Mac);
VOID ArpCacheDel(UINT32 Ip);
VOID ArpCacheClear(VOID);
VOID ArpCachePurgeExpired(VOID);
BOOL ArpCacheLookup(UINT32 Ip, UINT8 *MacOut);

/*
 * Periodic housekeeping; call from a timer subscriber once per second.
 */
VOID ArpTick(VOID);