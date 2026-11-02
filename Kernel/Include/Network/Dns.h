#pragma once

#include <Network/IpV4.h>
#include <Network/Udp.h>
#include <SpinLock.h>
#include <Types.h>
#include <List.h>

/*
 * ============================================================================
 * DNS (RFC 1035) — client only.
 *
 * A-record resolution with a small cache and synchronous/async entry
 * points. Handles name compression in responses. No DNSSEC, no EDNS0,
 * no AAAA or MX.
 * ============================================================================
 */

#define DNS_PORT            53
#define DNS_MAX_NAME        256
#define DNS_MAX_PACKET      512
#define DNS_TIMEOUT_MS      3000
#define DNS_MAX_RETRIES     2
#define DNS_CACHE_TTL       300          /* seconds */

typedef struct ATTRIBUTE(packed) DnsHeader {
    UINT16 Id;
    UINT16 Flags;
    UINT16 QdCount;
    UINT16 AnCount;
    UINT16 NsCount;
    UINT16 ArCount;
} DnsHeader;

#define DNS_FLAG_QR             0x8000
#define DNS_FLAG_OPCODE_MASK    0x7800
#define DNS_FLAG_AA             0x0400
#define DNS_FLAG_TC             0x0200
#define DNS_FLAG_RD             0x0100
#define DNS_FLAG_RA             0x0080
#define DNS_FLAG_RCODE_MASK     0x000F

#define DNS_RCODE_NOERROR       0
#define DNS_RCODE_FORMERR       1
#define DNS_RCODE_SERVFAIL      2
#define DNS_RCODE_NXDOMAIN      3
#define DNS_RCODE_NOTIMP        4
#define DNS_RCODE_REFUSED       5

typedef struct DnsCacheEntry {
    CHAR   Domain[DNS_MAX_NAME];
    UINT32 Ip;
    UINT64 Expires;
    BOOL   Valid;
    struct DnsCacheEntry *Next;
} DnsCacheEntry;

typedef struct DnsQuery {
    UINT16  Id;
    CHAR    Domain[DNS_MAX_NAME];
    VOID  (*Callback)(UINT32 Ip, VOID *Arg);
    VOID   *Arg;
    UINT64  StartTime;
    UINT32  Retries;
    BOOL    Active;
    struct DnsQuery *Next;
} DnsQuery;

/*
 * ============================================================================
 * API
 * ============================================================================
 */

INT  DnsInit(VOID);
VOID DnsSetNameserver(UINT32 Ns);

INT  DnsResolve(const CHAR *Domain, UINT32 *OutIp);
INT  DnsResolveAsync(const CHAR *Domain,
                     VOID (*Callback)(UINT32 Ip, VOID *Arg),
                     VOID *Arg);

VOID DnsCacheClear(VOID);
VOID DnsTick(VOID);

const CHAR *DnsRCodeString(UINT16 RCode);