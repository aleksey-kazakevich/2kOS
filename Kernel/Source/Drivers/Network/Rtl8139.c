#include <Drivers/Network/Rtl8139.h>
#include <Network/Ethernet.h>
#include <Network/Network.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Drivers/Pci.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SysDevice.h>
#include <SpinLock.h>
#include <Hal/Irq.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>

/*
 * ============================================================================
 * Register offsets (I/O port, relative to BAR0)
 * ============================================================================
 */

#define RTL_IDR0        0x00   /* MAC[0..5]            */
#define RTL_TSD0        0x10   /* transmit status [0..3] */
#define RTL_TSAD0       0x20   /* transmit start addr [0..3] */
#define RTL_RBSTART     0x30   /* receive buffer start address */
#define RTL_CR          0x37   /* command register     */
#define RTL_CAPR        0x38   /* current address of packet read */
#define RTL_CBR         0x3A   /* current buffer address */
#define RTL_IMR         0x3C   /* interrupt mask       */
#define RTL_ISR         0x3E   /* interrupt status     */
#define RTL_TCR         0x40   /* transmit config      */
#define RTL_RCR         0x44   /* receive config       */
#define RTL_9346CR      0x50   /* 93C46 command        */
#define RTL_CONFIG1     0x52
#define RTL_MSR         0x58   /* media status         */
#define RTL_BMCR        0x62   /* basic mode control   */

/* CR bits */
#define RTL_CR_RST      (1u << 4)
#define RTL_CR_RE       (1u << 3)
#define RTL_CR_TE       (1u << 2)
#define RTL_CR_BUFE     (1u << 0)

/* ISR bits */
#define RTL_ISR_ROK     (1u << 0)
#define RTL_ISR_RER     (1u << 1)
#define RTL_ISR_TOK     (1u << 2)
#define RTL_ISR_TER     (1u << 3)
#define RTL_ISR_RXOVW   (1u << 4)
#define RTL_ISR_PUN     (1u << 5)
#define RTL_ISR_FOVW    (1u << 6)
#define RTL_ISR_LENERR  (1u << 7)
#define RTL_ISR_TIMEOUT (1u << 14)

/* IMR is the same bit layout as ISR. */

/* RCR bits */
#define RTL_RCR_AAP     (1u << 0)
#define RTL_RCR_APM     (1u << 1)
#define RTL_RCR_AM      (1u << 2)
#define RTL_RCR_AB      (1u << 3)
#define RTL_RCR_WRAP    (1u << 7)
#define RTL_RCR_RBLEN_8K (0u << 11)
#define RTL_RCR_RBLEN_16K (1u << 11)
#define RTL_RCR_RBLEN_32K (2u << 11)
#define RTL_RCR_RBLEN_64K (3u << 11)
#define RTL_RCR_MXDMA_1024 (7u << 8)

/* TSD bits */
#define RTL_TSD_OWN     (1u << 13)
#define RTL_TSD_TUN     (1u << 14)
#define RTL_TSD_TOK     (1u << 15)

/* TCR bits */
#define RTL_TCR_CLRABT  (1u << 0)
#define RTL_TCR_IFG96   (3u << 24)

/*
 * ============================================================================
 * Configuration
 * ============================================================================
 */

#define RTL_TX_RING_SIZE      4
#define RTL_RX_BUF_SIZE       32768           /* must be one of 8K..64K */
#define RTL_TX_BUF_SIZE       2048
#define RTL_MAX_DEVICES       4

typedef struct Rtl8139Device {
    NetworkDevice   NetDev;
    PciDevice      *Pci;
    UINT16          IoBase;

    UINT8          *RxBufVirt;
    UINT64          RxBufPhys;
    UINT32          RxOffset;
    UINT32          RxBufLen;

    VOID           *TxBufVirt[RTL_TX_RING_SIZE];
    UINT64          TxBufPhys[RTL_TX_RING_SIZE];
    UINT32          TxCur;

    UINT8           Irq;
    UINT32          Gsi;
    UINT8           Vector;

    KDriver        *Driver;
    SysDevice      *SysDev;
    BOOL            Initialized;

    SpinLock        TxLock;
    SpinLock        RxLock;
} Rtl8139Device;

STATIC Rtl8139Device GRtlDevices[RTL_MAX_DEVICES];
STATIC UINT32        GRtlCount = 0;

/*
 * ============================================================================
 * I/O helpers
 * ============================================================================
 */

STATIC INLINE UINT8 RtlRead8(Rtl8139Device *D, UINT16 Reg) {
    return Inb(D->IoBase + Reg);
}
STATIC INLINE UINT16 RtlRead16(Rtl8139Device *D, UINT16 Reg) {
    return Inw(D->IoBase + Reg);
}
STATIC INLINE UINT32 RtlRead32(Rtl8139Device *D, UINT16 Reg) {
    return Inl(D->IoBase + Reg);
}
STATIC INLINE VOID RtlWrite8(Rtl8139Device *D, UINT16 Reg, UINT8 Val) {
    Outb(D->IoBase + Reg, Val);
}
STATIC INLINE VOID RtlWrite16(Rtl8139Device *D, UINT16 Reg, UINT16 Val) {
    Outw(D->IoBase + Reg, Val);
}
STATIC INLINE VOID RtlWrite32(Rtl8139Device *D, UINT16 Reg, UINT32 Val) {
    Outl(D->IoBase + Reg, Val);
}

/*
 * ============================================================================
 * DMA buffer helpers
 * ============================================================================
 */

STATIC VOID *RtlAllocDma(UINT64 *PhysOut) {
    VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys) return NULLPTR;

    UINT64 PhysAddr = (UINT64)Phys;
    VOID  *Virt     = (VOID*)PhysToVirt(PhysAddr);
    MemSet(Virt, 0, PAGE_SIZE);

    if (PhysOut) *PhysOut = PhysAddr;
    return Virt;
}

STATIC VOID RtlFreeDma(VOID *Virt) {
    if (!Virt) return;
    UINT64 Phys = VirtToPhys((UINT64)Virt);
    PhysAllocFreePage(PhysAllocGet(), (VOID*)Phys);
}

/*
 * ============================================================================
 * MAC
 * ============================================================================
 */

STATIC VOID RtlReadMac(Rtl8139Device *D, UINT8 *Mac) {
    for (INT I = 0; I < 6; I++) {
        Mac[I] = RtlRead8(D, RTL_IDR0 + I);
    }
}

/*
 * ============================================================================
 * Reset and bring-up
 * ============================================================================
 */

STATIC BOOL RtlReset(Rtl8139Device *D) {
    RtlWrite8(D, RTL_CR, RTL_CR_RST);
    for (INT I = 0; I < 1000; I++) {
        if (!(RtlRead8(D, RTL_CR) & RTL_CR_RST)) return TRUE;
        TimerMdelay(1);
    }
    return FALSE;
}

STATIC VOID RtlEnableInterrupts(Rtl8139Device *D) {
    RtlWrite16(D, RTL_IMR,
               RTL_ISR_ROK | RTL_ISR_RER | RTL_ISR_TOK |
               RTL_ISR_TER | RTL_ISR_RXOVW | RTL_ISR_FOVW |
               RTL_ISR_LENERR);
    RtlWrite16(D, RTL_ISR, 0xFFFF);
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

STATIC INT RtlTransmit(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    Rtl8139Device *D = (Rtl8139Device*)Dev->Priv;
    if (!D || !D->Initialized) {
        NetworkFreeBuf(Buf);
        return DEVICE_INVALID;
    }

    if (Buf->Len > RTL_TX_BUF_SIZE) {
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    UINTPTR F = SpinLockAcquireIrq(&D->TxLock);

    UINT32 Slot = D->TxCur;
    /* Wait for the previous transmission in this slot to finish. */
    INT Spins = 0;
    while (RtlRead32(D, RTL_TSD0 + Slot * 4) & RTL_TSD_OWN) {
        if (++Spins > 10000) {
            SpinLockReleaseIrq(&D->TxLock, F);
            NetworkFreeBuf(Buf);
            return BUSY;
        }
        CpuPause();
    }

    MemCpy(D->TxBufVirt[Slot], Buf->Data, Buf->Len);

    /* Write the physical address and length (len >= 60, but the ring
     * already padded us up; QEMU does not enforce it). */
    RtlWrite32(D, RTL_TSAD0 + Slot * 4, (UINT32)D->TxBufPhys[Slot]);
    RtlWrite32(D, RTL_TSD0  + Slot * 4,
               (UINT32)Buf->Len | RTL_TSD_OWN);

    D->TxCur = (Slot + 1) % RTL_TX_RING_SIZE;

    SpinLockReleaseIrq(&D->TxLock, F);

    NetworkFreeBuf(Buf);
    return SUCCESS;
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

STATIC BOOL RtlProgramBuffers(Rtl8139Device *D) {
    /* FIX: RTL_RX_BUF_SIZE is 32 KB but we used to allocate only ONE
     * page (4 KB). RtlReceive then indexed up to 32 KB → out-of-bounds
     * reads. We now allocate 8 physically contiguous pages (32 KB). */

    VOID *RxVirt = PhysAllocAllocateRange(PhysAllocGet(), 8);
    if (!RxVirt) return FALSE;
    MemSet(RxVirt, 0, 32768);

    D->RxBufVirt = (UINT8*)RxVirt;
    D->RxBufPhys = VirtToPhys((UINT64)RxVirt);

    for (UINT32 I = 0; I < RTL_TX_RING_SIZE; I++) {
        D->TxBufVirt[I] = RtlAllocDma(&D->TxBufPhys[I]);
        if (!D->TxBufVirt[I]) return FALSE;
    }

    D->RxBufLen = RTL_RX_BUF_SIZE;
    D->RxOffset = 0;

    RtlWrite32(D, RTL_RBSTART, (UINT32)D->RxBufPhys);
    RtlWrite16(D, RTL_CAPR, 0);
    RtlWrite16(D, RTL_CBR,  0);

    /* RCR: accept broadcast/multicast/our MAC, wrap, 32K buffer,
     * unlimited DMA burst. */
    RtlWrite32(D, RTL_RCR,
               RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM |
               RTL_RCR_WRAP | RTL_RCR_RBLEN_32K |
               RTL_RCR_MXDMA_1024);

    RtlWrite32(D, RTL_TCR, RTL_TCR_IFG96);

    RtlWrite8(D, RTL_CR, RTL_CR_RE | RTL_CR_TE);
    return TRUE;
}

STATIC VOID RtlReceive(Rtl8139Device *D) {
    UINTPTR F = SpinLockAcquireIrq(&D->RxLock);

    while (!(RtlRead8(D, RTL_CR) & RTL_CR_BUFE)) {
        UINT16 Status = *(volatile UINT16*)(D->RxBufVirt + D->RxOffset);
        UINT16 Length = *(volatile UINT16*)(D->RxBufVirt + D->RxOffset + 2);

        if (Length == 0 || Length > D->RxBufLen) break;

        if ((Status & 0x0001) && Length > 4) {
            UINT32 PayloadLen = Length - 4;
            UINT32 Wrap = D->RxBufLen - D->RxOffset - 4;
            UINT8 *Src;
            UINT8  Scratch[2048];

            /* FIX: bail out if a wrapped frame won't fit in Scratch. */
            if (PayloadLen > sizeof(Scratch)) {
                DevFreeRxFrame:
                D->RxOffset = (D->RxOffset + Length + 4 + 3) & ~3u;
                D->RxOffset %= D->RxBufLen;
                UINT16 Capr = (UINT16)((D->RxOffset + D->RxBufLen - 16) % D->RxBufLen);
                RtlWrite16(D, RTL_CAPR, Capr);
                continue;
            }

            if (PayloadLen > Wrap) {
                UINT32 FirstPart = Wrap;
                UINT32 SecondPart = PayloadLen - FirstPart;
                MemCpy(Scratch, D->RxBufVirt + D->RxOffset + 4, FirstPart);
                MemCpy(Scratch + FirstPart, D->RxBufVirt, SecondPart);
                Src = Scratch;
            } else {
                Src = D->RxBufVirt + D->RxOffset + 4;
            }

            NetworkBuf *Buf = NetworkAllocBuf(PayloadLen);
            if (Buf && NetworkPut(Buf, PayloadLen) == SUCCESS) {
                MemCpy(Buf->Data, Src, PayloadLen);
                NetworkRx(Buf, &D->NetDev);
            } else if (Buf) {
                NetworkFreeBuf(Buf);
            }
            goto DevFreeRxFrame;
        }

        D->RxOffset = (D->RxOffset + Length + 4 + 3) & ~3u;
        D->RxOffset %= D->RxBufLen;

        UINT16 Capr = (UINT16)((D->RxOffset + D->RxBufLen - 16) % D->RxBufLen);
        RtlWrite16(D, RTL_CAPR, Capr);
    }

    SpinLockReleaseIrq(&D->RxLock, F);
}

/*
 * ============================================================================
 * Interrupt handler
 * ============================================================================
 */

VOID Rtl8139Isr(VOID) {
    for (UINT32 I = 0; I < GRtlCount; I++) {
        Rtl8139Device *D = &GRtlDevices[I];
        if (!D->Initialized) continue;

        UINT16 Status = RtlRead16(D, RTL_ISR);
        if (Status == 0) continue;

        RtlWrite16(D, RTL_ISR, Status);

        if (Status & RTL_ISR_ROK) {
            RtlReceive(D);
        }
        if (Status & (RTL_ISR_RER | RTL_ISR_RXOVW | RTL_ISR_FOVW)) {
            D->NetDev.Stats.RxErrors++;
            /* On overflow the chip may stop receiving; clear the
             * buffer and reset CAPR to 0. */
            RtlWrite16(D, RTL_CAPR, 0);
            D->RxOffset = 0;
        }
        if (Status & RTL_ISR_TOK) {
            /* No action: TxCur already points at the next free slot. */
        }
        if (Status & RTL_ISR_TER) {
            D->NetDev.Stats.TxErrors++;
        }
    }

    HalIrqEoi();
}

EXTERN(VOID, Rtl8139IrqStub(VOID));

/*
 * ============================================================================
 * Registration helpers
 * ============================================================================
 */

STATIC VOID RtlRegisterSysDevice(Rtl8139Device *D, SysDevice *PciDev) {
    SysDevice *Root = PciDev ? PciDev : SysDeviceGetRoot();
    if (!Root) return;

    D->SysDev = SysDeviceCreate(Root, D->NetDev.Name,
                                "Realtek RTL8139 network adapter",
                                SYSDEV_TYPE_NETWORK);
    if (!D->SysDev) return;

    SysDeviceSetPropU32(D->SysDev, "vendor", RTL8139_VENDOR_REALTEK);
    SysDeviceSetPropU32(D->SysDev, "device", D->Pci->DeviceId);

    CHAR MacStr[18];
    EthernetMacNTop(D->NetDev.MacAddr, MacStr, sizeof(MacStr));
    SysDeviceSetProp(D->SysDev, "mac", MacStr);

    SysDeviceSetPropU32(D->SysDev, "mtu", D->NetDev.Mtu);
    SysDeviceSetPropU32(D->SysDev, "io_base", D->IoBase);
    SysDeviceSetPropU32(D->SysDev, "irq_line", D->Irq);
    SysDeviceSetPropU32(D->SysDev, "gsi", D->Gsi);
    SysDeviceSetPropU32(D->SysDev, "vector", D->Vector);

    SysDeviceAddResource(D->SysDev, SYSDEV_RES_IO, D->IoBase, 0x100);
    if (D->Gsi) {
        SysDeviceAddResource(D->SysDev, SYSDEV_RES_IRQ, D->Gsi, 1);
    }

    SysDeviceSetState(D->SysDev, SYSDEV_STATE_ENABLED);
    if (D->Driver) SysDeviceBindDriver(D->SysDev, D->Driver);
}

STATIC INT Rtl8139Probe(PciDevice *Pci) {
    if (!Pci || Pci->VendorId != RTL8139_VENDOR_REALTEK) return NOT_FOUND;

    switch (Pci->DeviceId) {
    case RTL8139_DEVICE_8139:
        return SUCCESS;
    default:
        return NOT_SUPPORTED;
    }
}

STATIC INT Rtl8139Attach(PciDevice *Pci) {
    if (GRtlCount >= RTL_MAX_DEVICES) return NO_MEMORY;

    Rtl8139Device *D = &GRtlDevices[GRtlCount];
    MemSet(D, 0, sizeof(Rtl8139Device));
    SpinLockInit(&D->TxLock);
    SpinLockInit(&D->RxLock);

    D->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    /* BAR0 is the I/O port range. */
    UINT64 Bar = 0;
    for (INT I = 0; I < 6; I++) {
        if (Pci->Bars[I] == 0) continue;
        if (Pci->BarTypes[I] == 2) {       /* 2 = I/O */
            Bar = Pci->Bars[I];
            break;
        }
    }
    if (Bar == 0) return DEVICE_INVALID;
    D->IoBase = (UINT16)(Bar & ~0x3u);

    if (!RtlReset(D)) return DEVICE_ERROR;
    if (!RtlProgramBuffers(D)) return NO_MEMORY;

    NetworkDevice *Nd = &D->NetDev;
    StrCpy(Nd->Name, "eth0");
    if (GRtlCount > 0) {
        SnPrintf(Nd->Name, sizeof(Nd->Name), "eth%u", GRtlCount);
    }
    Nd->Type    = NETWORK_TYPE_ETHERNET;
    Nd->Mtu     = NETWORK_MTU;
    Nd->Up      = TRUE;
    Nd->Running = TRUE;
    Nd->Xmit    = RtlTransmit;
    Nd->Priv    = D;

    RtlReadMac(D, Nd->MacAddr);

    D->Irq    = Pci->InterruptLine;
    D->Vector = 0x62;

    UINT32 Flags = HAL_IRQ_FLAG_EDGE_TRIGGERED | HAL_IRQ_FLAG_ACTIVE_HIGH;
    if (HalIrqResolve(D->Irq, &D->Gsi, &Flags) != SUCCESS) {
        D->Gsi = D->Irq;
    }

    HalIrqInstallHandler(D->Vector, (VOID(*)())Rtl8139IrqStub, 0);
    HalIrqRoute(D->Gsi, D->Vector, HalIrqGetLocalApicId(), Flags);
    HalIrqUnmask(D->Gsi);

    INT Rc = NetworkRegisterDevice(Nd);
    if (Rc != SUCCESS) {
        StdErrPrintf("rtl8139: register failed (%d)\n", Rc);
        return Rc;
    }

    RtlEnableInterrupts(D);
    D->Initialized = TRUE;

    D->Driver = KDriverGenerateStruct("Rtl8139", DCL1, TRUE, D, NULLPTR);
    if (D->Driver) KDriverRegister(D->Driver);

    SysDevice *PciDev = SysDeviceFind("PCI0");
    RtlRegisterSysDevice(D, PciDev);

    StdOutPrintf("rtl8139: %s attached (%02x:%02x:%02x:%02x:%02x:%02x)\n",
                 Nd->Name,
                 Nd->MacAddr[0], Nd->MacAddr[1], Nd->MacAddr[2],
                 Nd->MacAddr[3], Nd->MacAddr[4], Nd->MacAddr[5]);

    GRtlCount++;
    return SUCCESS;
}

/*
 * ============================================================================
 * Entry point
 * ============================================================================
 */

INT Rtl8139Init(VOID) {
    INT Found = 0;

    PciDevice *Pci = PciGetFirst();
    while (Pci) {
        if (Rtl8139Probe(Pci) == SUCCESS) {
            INT Rc = Rtl8139Attach(Pci);
            if (Rc == SUCCESS) Found++;
            else StdErrPrintf("rtl8139: attach failed (%d)\n", Rc);
        }
        Pci = PciGetNext(Pci);
    }

    if (Found == 0) return NOT_FOUND;
    return SUCCESS;
}