#pragma once

#include <Network/Network.h>
#include <Types.h>

/*
 * ============================================================================
 * Ethernet II framing
 *
 *   +-------------------+-------------------+-----------+
 *   | Dst MAC (6)       | Src MAC (6)       | Type (2)  | payload...
 *   +-------------------+-------------------+-----------+
 *
 * Type is big-endian on the wire. We convert to host order on Rx and
 * back on Tx, so every other layer works with host-order values only.
 *
 * Minimum frame is 60 bytes (14 header + 46 payload) WITHOUT the FCS.
 * The NIC appends the 4-byte FCS on the wire; we never see it and
 * never generate it.
 * ============================================================================
 */

#define ETH_HLEN            14
#define ETH_FRAME_LEN       1514
#define ETH_MIN_FRAME       64         /* including FCS, 60 without */

#define ETH_P_IP            0x0800
#define ETH_P_ARP           0x0806

typedef struct ATTRIBUTE(packed) EthernetHeader {
    UINT8  DstMac[NETWORK_ETH_ALEN];
    UINT8  SrcMac[NETWORK_ETH_ALEN];
    UINT16 Type;
} EthernetHeader;

/*
 * Well-known MAC addresses. Declared as arrays so they can be used
 * directly in MemCpy / MemCmp without taking their address.
 */
extern const UINT8 ETH_MAC_BROADCAST[NETWORK_ETH_ALEN];
extern const UINT8 ETH_MAC_ZERO[NETWORK_ETH_ALEN];

/*
 * ============================================================================
 * Functions
 * ============================================================================
 */

VOID EthernetInit(VOID);

/*
 * EthernetOutput — build a frame around Buf and hand it to the device.
 *
 * On entry Buf contains the payload (IPv4/ARP) only. This function
 * prepends the Ethernet header, pads to the minimum frame size, and
 * transmits via Dev->Xmit.
 *
 * On SUCCESS the device takes ownership of Buf. On failure Buf is
 * freed here; the caller must not touch it either way.
 */
INT EthernetOutput(NetworkBuf *Buf, NetworkDevice *Dev,
                   UINT8 *DstMac, UINT16 Type);

/*
 * EthernetInput — called by NetworkRx with a full frame in Buf.
 *
 * Validates the length, applies the address filter, strips the Ethernet
 * header and dispatches by ethertype. This function always consumes Buf.
 */
VOID EthernetInput(NetworkBuf *Buf, NetworkDevice *Dev);

INT  EthernetRegisterProtocol(UINT16 Type, NetworkRxHandler Handler);
VOID EthernetUnregisterProtocol(UINT16 Type);

/*
 * MAC helpers.
 */
VOID EthernetMacNTop(UINT8 *Mac, CHAR *Buf, UINT32 Size);
INT  EthernetMacPTon(const CHAR *Str, UINT8 *Mac);
BOOL EthernetMacEqual(UINT8 *Mac1, UINT8 *Mac2);
BOOL EthernetIsMulticast(const UINT8 *Mac);
BOOL EthernetIsBroadcast(const UINT8 *Mac);