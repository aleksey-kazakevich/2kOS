#include <Network/Ethernet.h>
#include <Network/IpV4.h>
#include <Network/Arp.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Return.h>

const UINT8 ETH_MAC_BROADCAST[NETWORK_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const UINT8 ETH_MAC_ZERO[NETWORK_ETH_ALEN]      = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

STATIC NetworkRxHandler GEthProtocols[65536];

/*
 * ============================================================================
 * MAC helpers
 * ============================================================================
 */

VOID EthernetMacNTop(UINT8 *Mac, CHAR *Buf, UINT32 Size) {
    if (!Mac || !Buf || Size == 0) return;
    SnPrintf(Buf, Size, "%02x:%02x:%02x:%02x:%02x:%02x",
             Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]);
}

STATIC INT EthHexDigit(CHAR C) {
    if (C >= '0' && C <= '9') return C - '0';
    if (C >= 'a' && C <= 'f') return C - 'a' + 10;
    if (C >= 'A' && C <= 'F') return C - 'A' + 10;
    return -1;
}

INT EthernetMacPTon(const CHAR *Str, UINT8 *Mac) {
    if (!Str || !Mac) return INCORRECT_VALUE;

    for (INT I = 0; I < NETWORK_ETH_ALEN; I++) {
        INT Hi = EthHexDigit(Str[0]);
        INT Lo = EthHexDigit(Str[1]);
        if (Hi < 0 || Lo < 0) return INCORRECT_VALUE;

        Mac[I] = (UINT8)((Hi << 4) | Lo);
        Str += 2;

        if (I < NETWORK_ETH_ALEN - 1) {
            if (*Str != ':' && *Str != '-') return INCORRECT_VALUE;
            Str++;
        }
    }
    return SUCCESS;
}

BOOL EthernetMacEqual(UINT8 *Mac1, UINT8 *Mac2) {
    if (!Mac1 || !Mac2) return FALSE;
    return MemCmp(Mac1, Mac2, NETWORK_ETH_ALEN) == 0;
}

BOOL EthernetIsBroadcast(const UINT8 *Mac) {
    if (!Mac) return FALSE;
    for (INT I = 0; I < NETWORK_ETH_ALEN; I++) {
        if (Mac[I] != 0xFF) return FALSE;
    }
    return TRUE;
}

BOOL EthernetIsMulticast(const UINT8 *Mac) {
    if (!Mac) return FALSE;
    return (Mac[0] & 0x01) != 0;
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

INT EthernetOutput(NetworkBuf *Buf, NetworkDevice *Dev,
                   UINT8 *DstMac, UINT16 Type) {
    /* FIX: contract — on SUCCESS the driver owns Buf; on failure we
     * free it here. The drivers (E1000/Rtl8139/VirtioNet) also free
     * Buf on their own error paths, so we must NOT free it after
     * NetworkTx returns an error. NetworkTx owns that decision. */
    if (!Buf || !Dev || !DstMac) {
        if (Buf) NetworkFreeBuf(Buf);
        return NO_OBJECT;
    }

    if (Buf->Len > Dev->Mtu) {
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    if (NetworkReserve(Buf, ETH_HLEN) != SUCCESS) {
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    EthernetHeader *Eth = (EthernetHeader*)Buf->Data;
    MemCpy(Eth->DstMac, DstMac, NETWORK_ETH_ALEN);
    MemCpy(Eth->SrcMac, Dev->MacAddr, NETWORK_ETH_ALEN);
    Eth->Type = Htons(Type);

    if (Buf->Len < ETH_MIN_FRAME - 4) {
        UINT32 Pad = (ETH_MIN_FRAME - 4) - Buf->Len;
        if (NetworkPut(Buf, Pad) == SUCCESS) {
            MemSet(Buf->Tail - Pad, 0, Pad);
        }
    }

    /* NetworkTx / Xmit take ownership on success AND on failure —
     * the drivers free Buf on error internally. So do nothing here. */
    return NetworkTx(Buf, Dev);
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

VOID EthernetInput(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return;
    }

    if (Buf->Len < ETH_HLEN) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    EthernetHeader *Eth = (EthernetHeader*)Buf->Data;

    if (!EthernetMacEqual(Eth->DstMac, Dev->MacAddr) &&
        !EthernetIsBroadcast(Eth->DstMac) &&
        !EthernetIsMulticast(Eth->DstMac)) {
        NetworkFreeBuf(Buf);
        return;
    }

    UINT16 Type = Ntohs(Eth->Type);

    if (NetworkPull(Buf, ETH_HLEN) != SUCCESS) {
        Dev->Stats.RxErrors++;
        NetworkFreeBuf(Buf);
        return;
    }

    if (Type < 65536 && GEthProtocols[Type]) {
        GEthProtocols[Type](Buf, Dev);
        return;
    }

    Dev->Stats.RxDropped++;
    NetworkFreeBuf(Buf);
}

/*
 * ============================================================================
 * Protocol registration
 * ============================================================================
 */

INT EthernetRegisterProtocol(UINT16 Type, NetworkRxHandler Handler) {
    if (Type >= 65536) return INCORRECT_VALUE;
    if (!Handler) return NO_OBJECT;
    if (GEthProtocols[Type]) return ALREADY_EXISTS;

    GEthProtocols[Type] = Handler;
    return SUCCESS;
}

VOID EthernetUnregisterProtocol(UINT16 Type) {
    if (Type < 65536) GEthProtocols[Type] = NULLPTR;
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

VOID EthernetInit(VOID) {
    MemSet(GEthProtocols, 0, sizeof(GEthProtocols));

    EthernetRegisterProtocol(ETH_P_IP,   (NetworkRxHandler)IpV4Input);
    EthernetRegisterProtocol(ETH_P_ARP,  (NetworkRxHandler)ArpInput);

    StdOutPrintf("net: ethernet initialized\n");
}