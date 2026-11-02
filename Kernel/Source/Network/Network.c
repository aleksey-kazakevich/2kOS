#include <Drivers/Network/Loopback.h>
#include <Drivers/Network/Rtl8139.h>
#include <Drivers/Network/E1000.h>
#include <Time/TimerSubscriber.h>
#include <Network/Ethernet.h>
#include <Network/Network.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Network/Dhcp.h>
#include <Network/ICmp.h>
#include <Network/Arp.h>
#include <Network/Udp.h>
#include <Network/Tcp.h>
#include <Network/Dns.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Lib/StdIo.h>
#include <Return.h>

/*
 * ============================================================================
 * Global state
 * ============================================================================
 */

ListHead GNetworkDevices;
STATIC UINT32 GNetworkDeviceCount = 0;
STATIC BOOL   GNetworkInitialized = FALSE;

/*
 * ============================================================================
 * Buffer management
 * ============================================================================
 */

NetworkBuf *NetworkAllocBuf(UINT32 Size) {
    if (Size > NETWORK_MTU) return NULLPTR;

    NetworkBuf *Buf = (NetworkBuf*)MemoryAllocate(sizeof(NetworkBuf));
    if (!Buf) return NULLPTR;
    MemSet(Buf, 0, sizeof(NetworkBuf));

    UINT32 TotalSize = Size + NETWORK_HEADROOM;

    Buf->Head = (UINT8*)MemoryAllocate(TotalSize);
    if (!Buf->Head) {
        MemoryFree(Buf);
        return NULLPTR;
    }

    Buf->Data     = Buf->Head + NETWORK_HEADROOM;
    Buf->Tail     = Buf->Data;
    Buf->End      = Buf->Head + TotalSize;
    Buf->Len      = 0;
    Buf->HeadRoom = NETWORK_HEADROOM;
    Buf->TrueSize = TotalSize;
    Buf->RefCount = 1;
    return Buf;
}

VOID NetworkBufRef(NetworkBuf *Buf) {
    if (!Buf) return;
    __sync_fetch_and_add(&Buf->RefCount, 1);
}

VOID NetworkBufUnref(NetworkBuf *Buf) {
    if (!Buf) return;

    UINT32 Old = __sync_fetch_and_sub(&Buf->RefCount, 1);
    if (Old != 1) return;

    if (Buf->Head) MemoryFree(Buf->Head);
    MemoryFree(Buf);
}

VOID NetworkFreeBuf(NetworkBuf *Buf) {
    NetworkBufUnref(Buf);
}

NetworkBuf *NetworkBufClone(NetworkBuf *Src) {
    if (!Src) return NULLPTR;

    /* FIX: allocate exactly Src->Len + Src->HeadRoom, no MTU cap. */
    UINT32 Need = Src->Len + Src->HeadRoom;
    if (Need > NETWORK_MTU + NETWORK_HEADROOM) return NULLPTR;

    NetworkBuf *Dst = NetworkAllocBuf(Src->Len + Src->HeadRoom);
    if (!Dst) return NULLPTR;

    UINT32 HeadUsed = (UINT32)(Src->Data - Src->Head);
    Dst->Data     = Dst->Head + HeadUsed;
    Dst->HeadRoom = Src->HeadRoom;
    Dst->Tail     = Dst->Data + Src->Len;
    Dst->Len      = Src->Len;
    Dst->Protocol = Src->Protocol;
    Dst->IpSrc    = Src->IpSrc;
    Dst->IpDst    = Src->IpDst;
    Dst->Flags    = Src->Flags;

    if (Src->Len) MemCpy(Dst->Data, Src->Data, Src->Len);

    return Dst;
}

INT NetworkReserve(NetworkBuf *Buf, UINT32 HeadRoom) {
    if (!Buf) return INCORRECT_VALUE;
    if (HeadRoom > Buf->HeadRoom) return INCORRECT_VALUE;

    Buf->Data     -= HeadRoom;
    Buf->HeadRoom -= HeadRoom;
    Buf->Len      += HeadRoom;
    return SUCCESS;
}

INT NetworkPut(NetworkBuf *Buf, UINT32 Len) {
    if (!Buf) return INCORRECT_VALUE;
    if (Buf->Tail + Len > Buf->End) return INCORRECT_VALUE;

    Buf->Tail += Len;
    Buf->Len  += Len;
    return SUCCESS;
}

INT NetworkPull(NetworkBuf *Buf, UINT32 Len) {
    if (!Buf) return INCORRECT_VALUE;
    if (Len > Buf->Len) return INCORRECT_VALUE;

    Buf->Data     += Len;
    Buf->HeadRoom += Len;
    Buf->Len      -= Len;
    return SUCCESS;
}

INT NetworkTrim(NetworkBuf *Buf, UINT32 Len) {
    if (!Buf) return INCORRECT_VALUE;
    if (Len > Buf->Len) return INCORRECT_VALUE;

    Buf->Tail -= Len;
    Buf->Len  -= Len;
    return SUCCESS;
}

/*
 * ============================================================================
 * Device management
 * ============================================================================
 */

INT NetworkRegisterDevice(NetworkDevice *Dev) {
    if (!Dev) return NO_OBJECT;
    if (!Dev->Xmit) return INCORRECT_VALUE;
    if (!Dev->Name[0]) return INCORRECT_VALUE;
    if (Dev->Mtu == 0) Dev->Mtu = NETWORK_MTU;

    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Other = ListEntry(Pos, NetworkDevice, Node);
        if (StrCmp(Other->Name, Dev->Name) == 0) {
            return ALREADY_EXISTS;
        }
    }

    Dev->Index = GNetworkDeviceCount++;
    ListAddTail(&GNetworkDevices, &Dev->Node);

    CHAR MacStr[18];
    SnPrintf(MacStr, sizeof(MacStr), "%02x:%02x:%02x:%02x:%02x:%02x",
             Dev->MacAddr[0], Dev->MacAddr[1], Dev->MacAddr[2],
             Dev->MacAddr[3], Dev->MacAddr[4], Dev->MacAddr[5]);
    StdOutPrintf("net: %s up (%s, mtu %u)\n", Dev->Name, MacStr, Dev->Mtu);

    return SUCCESS;
}

INT NetworkUnregisterDevice(NetworkDevice *Dev) {
    if (!Dev) return NO_OBJECT;

    ListDel(&Dev->Node);
    if (GNetworkDeviceCount > 0) GNetworkDeviceCount--;

    StdOutPrintf("net: %s down\n", Dev->Name);
    return SUCCESS;
}

INT NetworkRx(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return NO_OBJECT;
    }

    Buf->Dev = Dev;
    Dev->Stats.RxPackets++;
    Dev->Stats.RxBytes += Buf->Len;

    EthernetInput(Buf, Dev);
    return SUCCESS;
}

INT NetworkTx(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return NO_OBJECT;
    }
    if (!Dev->Xmit) {
        NetworkFreeBuf(Buf);
        return NOT_IMPLEMENTED;
    }

    Dev->Stats.TxPackets++;
    Dev->Stats.TxBytes += Buf->Len;

    /* FIX: contract — Xmit always takes ownership of Buf, both on
     * success and on failure (drivers free it themselves on error). */
    INT Rc = Dev->Xmit(Buf, Dev);
    if (Rc != SUCCESS) {
        Dev->Stats.TxErrors++;
    }
    return Rc;
}

VOID NetworkPollDevices(VOID) {
    if (ListEmpty(&GNetworkDevices)) return;

    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Up && Dev->Running && Dev->Poll) {
            Dev->Poll(Dev);
        }
    }
}

NetworkDevice *NetworkGetDevice(UINT32 Index) {
    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Index == Index) return Dev;
    }
    return NULLPTR;
}

NetworkDevice *NetworkGetDeviceByName(const CHAR *Name) {
    if (!Name) return NULLPTR;

    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (StrCmp(Dev->Name, Name) == 0) return Dev;
    }
    return NULLPTR;
}

NetworkDevice *NetworkGetFirstDevice(VOID) {
    if (ListEmpty(&GNetworkDevices)) return NULLPTR;
    return ListEntry(GNetworkDevices.Next, NetworkDevice, Node);
}

NetworkDevice *NetworkGetFirstActive(VOID) {
    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Up && Dev->Running) return Dev;
    }
    return NULLPTR;
}

NetworkDevice *NetworkGetNext(NetworkDevice *Dev) {
    if (!Dev) return NULLPTR;
    if (Dev->Node.Next == &GNetworkDevices) return NULLPTR;
    return ListEntry(Dev->Node.Next, NetworkDevice, Node);
}

INT NetworkGetDevicesByType(NetworkDeviceType Type,
                            NetworkDevice **Devices, INT Max) {
    if (!Devices || Max <= 0) return INCORRECT_VALUE;

    INT Count = 0;
    ListHead *Pos;
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Type == Type) {
            Devices[Count++] = Dev;
            if (Count >= Max) break;
        }
    }
    return Count;
}

/*
 * ============================================================================
 * Protocol registration (thin wrapper over Ethernet)
 * ============================================================================
 */

INT NetworkRegisterProtocol(UINT16 EthType, NetworkRxHandler Handler) {
    return EthernetRegisterProtocol(EthType, Handler);
}

VOID NetworkUnregisterProtocol(UINT16 EthType) {
    EthernetUnregisterProtocol(EthType);
}

/*
 * ============================================================================
 * Lifecycle
 * ============================================================================
 */

STATIC VOID NetworkTickCallback(VOID *Ud) {
    (VOID)Ud;
    ArpTick();
    DnsTick();
    ICmpTick();
    DhcpTick();
    TcpTick();

    EXTERN(VOID, E1000Poll(VOID));
    E1000Poll();
}

STATIC TimerSubscriber GNetTickSub = {
    .Callback = NetworkTickCallback,
    .UserData = NULLPTR,
    .Interval = 100,
    .Name     = "net-tick"
};

INT NetworkInit(VOID) {
    if (GNetworkInitialized) return SUCCESS;

    ListInit(&GNetworkDevices);
    GNetworkDeviceCount = 0;

    EthernetInit();
    ArpInit();
    IpV4Init();
    ICmpInit();
    UdpInit();
    TcpInit();
    DnsInit();
    DhcpSubsystemInit();
    LoopbackInit();

    TimerSubscribe(&GNetTickSub);

    GNetworkInitialized = TRUE;
    StdOutPrintf("net: stack initialized\n");
    return SUCCESS;
}

INT NetworkLateInit(VOID) {
    E1000Init();
    Rtl8139Init();
    return SUCCESS;
}