#pragma once

#include <Network/IpV4.h>
#include <Network/Udp.h>
#include <Types.h>

/*
 * ============================================================================
 * DHCP (RFC 2131, RFC 2132) — client only.
 *
 * Implements DISCOVER / OFFER / REQUEST / ACK / NAK and lease renewal
 * (half-life renew, 7/8-life rebind). No DECLINE, no INFORM, no server.
 *
 * One DhcpClient instance per interface. The client keeps its own state
 * and calls back into the caller when the lease is configured.
 * ============================================================================
 */

#define DHCP_SERVER_PORT        67
#define DHCP_CLIENT_PORT        68
#define DHCP_MAGIC_COOKIE       0x63825363

/* Option codes. */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS_SERVER     6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN_NAME    15
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_MAX_MSG_SIZE   57
#define DHCP_OPT_VENDOR_ID      60
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

/* Message types. */
#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_DECLINE            4
#define DHCP_ACK                5
#define DHCP_NAK                6
#define DHCP_RELEASE            7
#define DHCP_INFORM             8

typedef struct ATTRIBUTE(packed) DhcpHeader {
    UINT8  Op;
    UINT8  HType;
    UINT8  HLen;
    UINT8  Hops;
    UINT32 Xid;
    UINT16 Secs;
    UINT16 Flags;
    UINT32 CIAddr;
    UINT32 YIAddr;
    UINT32 SIAddr;
    UINT32 GIAddr;
    UINT8  ChAddr[16];
    UINT8  SName[64];
    UINT8  File[128];
    UINT32 Magic;
    UINT8  Options[];
} DhcpHeader;

struct DhcpClient;

typedef VOID (*DhcpCallback)(struct DhcpClient *Client, BOOL Success);

typedef struct DhcpClient {
    NetworkDevice *Dev;
    BOOL           Running;
    BOOL           Configured;

    UINT32 Xid;
    UINT32 YIAddr;
    UINT32 ServerIp;
    UINT32 SubnetMask;
    UINT32 Gateway;
    UINT32 DnsServer;
    UINT32 LeaseTime;
    UINT64 LeaseExpires;
    UINT64 RenewTime;
    UINT64 RebindTime;

    DhcpCallback Callback;
    struct DhcpClient *Next;
} DhcpClient;

/*
 * ============================================================================
 * API
 * ============================================================================
 */

INT  DhcpSubsystemInit(VOID);
INT  DhcpInit(DhcpClient *Client, NetworkDevice *Dev);
INT  DhcpStart(DhcpClient *Client);
INT  DhcpRenew(DhcpClient *Client);
DhcpClient *DhcpGetClient(NetworkDevice *Dev);
INT  DhcpRelease(DhcpClient *Client);

VOID DhcpStartOnAllInterfaces(DhcpCallback Callback);
VOID DhcpTick(VOID);