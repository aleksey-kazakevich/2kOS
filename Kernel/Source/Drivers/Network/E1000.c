#include <Drivers/Network/E1000.h>
#include <Network/Ethernet.h>
#include <Network/Network.h>
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
#include <Asm/Mmio.h>
#include <Hal/Irq.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>

/*
 * ============================================================================
 * Register offsets (subset)
 * ============================================================================
 */

#define E1000_REG_CTRL      0x00000
#define E1000_REG_STATUS    0x00008
#define E1000_REG_EECD      0x00010
#define E1000_REG_ICR       0x000C0
#define E1000_REG_ITR       0x000C4
#define E1000_REG_IMS       0x000D0
#define E1000_REG_IMC       0x000D8
#define E1000_REG_RCTL      0x00100
#define E1000_REG_TCTL      0x00400
#define E1000_REG_TIPG      0x00410
#define E1000_REG_RDBAL     0x02800
#define E1000_REG_RDBAH     0x02804
#define E1000_REG_RDLEN     0x02808
#define E1000_REG_RDH       0x02810
#define E1000_REG_RDT       0x02818
#define E1000_REG_RDTR      0x02820
#define E1000_REG_TDBAL     0x03800
#define E1000_REG_TDBAH     0x03804
#define E1000_REG_TDLEN     0x03808
#define E1000_REG_TDH       0x03810
#define E1000_REG_TDT       0x03818
#define E1000_REG_MTA       0x05200
#define E1000_REG_RAL       0x05400
#define E1000_REG_RAH       0x05404

/* CTRL bits */
#define E1000_CTRL_FD       (1u << 0)    /* full duplex */
#define E1000_CTRL_LRST     (1u << 3)    /* link reset */
#define E1000_CTRL_ASDE     (1u << 5)    /* auto-speed detect */
#define E1000_CTRL_SLU      (1u << 6)    /* set link up */
#define E1000_CTRL_ILOS     (1u << 7)    /* invert loss of signal */
#define E1000_CTRL_RST      (1u << 26)   /* reset */

/* STATUS bits */
#define E1000_STATUS_LU     (1u << 1)    /* link up */

/* RCTL bits */
#define E1000_RCTL_EN       (1u << 1)
#define E1000_RCTL_BAM      (1u << 15)   /* broadcast accept */
#define E1000_RCTL_BSIZE_2K (0u << 16)   /* 2048-byte buffers */
#define E1000_RCTL_SECRC    (1u << 26)   /* strip Ethernet CRC */

/* TCTL bits */
#define E1000_TCTL_EN       (1u << 1)
#define E1000_TCTL_PSP      (1u << 3)    /* pad short packets */
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12
#define E1000_TCTL_RTLC     (1u << 24)

/* ICR bits */
#define E1000_ICR_TXDW      (1u << 0)
#define E1000_ICR_TXQE      (1u << 1)
#define E1000_ICR_LSC       (1u << 2)
#define E1000_ICR_RXSEQ     (1u << 3)
#define E1000_ICR_RXDMT0    (1u << 4)
#define E1000_ICR_RXO       (1u << 6)
#define E1000_ICR_RXT0      (1u << 7)

/* Transmit descriptor */
typedef struct ATTRIBUTE(packed) E1000TxDesc {
    UINT64 Addr;
    UINT16 Length;
    UINT8  Cso;
    UINT8  Cmd;
    UINT8  Status;
    UINT8  Css;
    UINT16 Special;
} E1000TxDesc;

#define E1000_TXD_CMD_EOP   (1u << 0)
#define E1000_TXD_CMD_IFCS  (1u << 1)
#define E1000_TXD_CMD_RS    (1u << 3)
#define E1000_TXD_STAT_DD   (1u << 0)

/* Receive descriptor */
typedef struct ATTRIBUTE(packed) E1000RxDesc {
    UINT64 Addr;
    UINT16 Length;
    UINT16 Checksum;
    UINT8  Status;
    UINT8  Errors;
    UINT16 Special;
} E1000RxDesc;

#define E1000_RXD_STAT_DD   (1u << 0)
#define E1000_RXD_STAT_EOP  (1u << 1)

/*
 * ============================================================================
 * Per-device state
 * ============================================================================
 */

typedef struct E1000Device {
    NetworkDevice    NetDev;
    PciDevice       *Pci;
    volatile UINT32 *Mmio;
    UINT64           MmioPhys;

    E1000TxDesc      *TxRing;
    E1000RxDesc      *RxRing;
    UINT64           TxRingPhys;
    UINT64           RxRingPhys;

    VOID             *TxBufVirt[E1000_TX_RING_SIZE];
    UINT64            TxBufPhys[E1000_TX_RING_SIZE];
    VOID             *RxBufVirt[E1000_RX_RING_SIZE];
    UINT64            RxBufPhys[E1000_RX_RING_SIZE];

    UINT32            TxHead;         /* next descriptor to clean  */
    UINT32            TxTail;         /* next descriptor to fill   */
    UINT32            RxNext;         /* next descriptor to check  */

    UINT8             Irq;
    UINT32            Gsi;
    UINT8             Vector;

    KDriver          *Driver;
    SysDevice        *SysDev;
    BOOL              Initialized;

    SpinLock          TxLock;
    SpinLock          RxLock;
} E1000Device;

/*
 * Every supported board gets its own slot. Eight is enough for any
 * realistic machine; QEMU guests usually have exactly one.
 */
#define E1000_MAX_DEVICES  8
STATIC E1000Device GE1000Devices[E1000_MAX_DEVICES];
STATIC UINT32      GE1000Count = 0;

/*
 * ============================================================================
 * MMIO helpers
 * ============================================================================
 */

STATIC INLINE UINT32 E1000Read(E1000Device *D, UINT32 Reg) {
    return *(volatile UINT32*)((UINT8*)D->Mmio + Reg);
}

STATIC INLINE VOID E1000Write(E1000Device *D, UINT32 Reg, UINT32 Val) {
    *(volatile UINT32*)((UINT8*)D->Mmio + Reg) = Val;
}

STATIC INLINE VOID E1000Flush(E1000Device *D) {
    (VOID)E1000Read(D, E1000_REG_STATUS);
}

/*
 * ============================================================================
 * Buffer allocation
 * ============================================================================
 */

STATIC VOID *E1000AllocDma(UINT64 *PhysOut) {
    VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys) return NULLPTR;

    UINT64 PhysAddr = (UINT64)Phys;
    VOID  *Virt     = (VOID*)PhysToVirt(PhysAddr);
    MemSet(Virt, 0, PAGE_SIZE);

    if (PhysOut) *PhysOut = PhysAddr;
    return Virt;
}

STATIC VOID E1000FreeDma(VOID *Virt) {
    if (!Virt) return;
    UINT64 Phys = VirtToPhys((UINT64)Virt);
    PhysAllocFreePage(PhysAllocGet(), (VOID*)Phys);
}

/*
 * ============================================================================
 * MAC
 * ============================================================================
 */

STATIC VOID E1000ReadMac(E1000Device *D, UINT8 *Mac) {
    UINT32 Ral = E1000Read(D, E1000_REG_RAL);
    UINT32 Rah = E1000Read(D, E1000_REG_RAH);

    Mac[0] = (UINT8)( Ral        & 0xFF);
    Mac[1] = (UINT8)((Ral >>  8) & 0xFF);
    Mac[2] = (UINT8)((Ral >> 16) & 0xFF);
    Mac[3] = (UINT8)((Ral >> 24) & 0xFF);
    Mac[4] = (UINT8)( Rah        & 0xFF);
    Mac[5] = (UINT8)((Rah >>  8) & 0xFF);
}

/*
 * ============================================================================
 * Reset and bring-up
 * ============================================================================
 */

STATIC BOOL E1000Reset(E1000Device *D) {
    E1000Write(D, E1000_REG_IMC, 0xFFFFFFFFu);
    E1000Read(D, E1000_REG_ICR);

    E1000Write(D, E1000_REG_CTRL, E1000Read(D, E1000_REG_CTRL) | E1000_CTRL_RST);
    E1000Flush(D);

    for (INT I = 0; I < 1000; I++) {
        if (!(E1000Read(D, E1000_REG_CTRL) & E1000_CTRL_RST)) return TRUE;
        TimerMdelay(1);
    }
    return FALSE;
}

STATIC VOID E1000ProgramRings(E1000Device *D) {
    /* Program multicast table to zero. */
    for (UINT32 I = 0; I < 128; I++) {
        E1000Write(D, E1000_REG_MTA + I * 4, 0);
    }

    /* TX ring */
    E1000Write(D, E1000_REG_TDBAL, (UINT32)(D->TxRingPhys & 0xFFFFFFFF));
    E1000Write(D, E1000_REG_TDBAH, (UINT32)(D->TxRingPhys >> 32));
    E1000Write(D, E1000_REG_TDLEN, E1000_TX_RING_SIZE * sizeof(E1000TxDesc));
    E1000Write(D, E1000_REG_TDH, 0);
    E1000Write(D, E1000_REG_TDT, 0);

    /* RX ring */
    E1000Write(D, E1000_REG_RDBAL, (UINT32)(D->RxRingPhys & 0xFFFFFFFF));
    E1000Write(D, E1000_REG_RDBAH, (UINT32)(D->RxRingPhys >> 32));
    E1000Write(D, E1000_REG_RDLEN, E1000_RX_RING_SIZE * sizeof(E1000RxDesc));
    E1000Write(D, E1000_REG_RDH, 0);
    E1000Write(D, E1000_REG_RDT, E1000_RX_RING_SIZE - 1);

    /* Control registers */
    E1000Write(D, E1000_REG_TCTL,
               E1000_TCTL_EN | E1000_TCTL_PSP |
               (0x10 << E1000_TCTL_CT_SHIFT) |
               (0x40 << E1000_TCTL_COLD_SHIFT) |
               E1000_TCTL_RTLC);
    E1000Write(D, E1000_REG_TIPG, 10 | (8 << 10) | (6 << 20));

    E1000Write(D, E1000_REG_RCTL,
               E1000_RCTL_EN | E1000_RCTL_BAM |
               E1000_RCTL_BSIZE_2K | E1000_RCTL_SECRC);

    /* Link up, full duplex, auto-speed. */
    UINT32 Ctrl = E1000Read(D, E1000_REG_CTRL);
    Ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD;
    Ctrl &= ~E1000_CTRL_LRST;
    E1000Write(D, E1000_REG_CTRL, Ctrl);
    E1000Flush(D);
}

STATIC VOID E1000EnableInterrupts(E1000Device *D) {
    E1000Write(D, E1000_REG_ICR, 0xFFFFFFFFu);
    E1000Write(D, E1000_REG_IMS,
               E1000_ICR_TXDW | E1000_ICR_TXQE |
               E1000_ICR_LSC  |
               E1000_ICR_RXDMT0 | E1000_ICR_RXO | E1000_ICR_RXT0);
}

/*
 * ============================================================================
 * Transmit
 * ============================================================================
 */

STATIC INT E1000Transmit(NetworkBuf *Buf, NetworkDevice *Dev) {
    /* FIX: this function always consumes Buf — both on success and on
     * error, so callers (EthernetOutput/NetworkTx) must not free it. */
    if (!Buf || !Dev) {
        if (Buf) NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    E1000Device *D = (E1000Device*)Dev->Priv;
    if (!D || !D->Initialized) {
        NetworkFreeBuf(Buf);
        return DEVICE_INVALID;
    }

    if (Buf->Len > E1000_BUF_SIZE) {
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    UINTPTR F = SpinLockAcquireIrq(&D->TxLock);

    UINT32 Tail = D->TxTail;
    E1000TxDesc *Desc = &D->TxRing[Tail];

    INT Spins = 0;
    while (!(Desc->Status & E1000_TXD_STAT_DD)) {
        if (++Spins > 10000) {
            SpinLockReleaseIrq(&D->TxLock, F);
            NetworkFreeBuf(Buf);
            return BUSY;
        }
        CpuPause();
    }

    MemCpy(D->TxBufVirt[Tail], Buf->Data, Buf->Len);

    Desc->Addr   = D->TxBufPhys[Tail];
    Desc->Length = (UINT16)Buf->Len;
    Desc->Cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    Desc->Status = 0;

    D->TxTail = (Tail + 1) % E1000_TX_RING_SIZE;
    E1000Write(D, E1000_REG_TDT, D->TxTail);

    SpinLockReleaseIrq(&D->TxLock, F);

    NetworkFreeBuf(Buf);
    return SUCCESS;
}

/*
 * ============================================================================
 * Receive
 * ============================================================================
 */

STATIC VOID E1000Receive(E1000Device *D) {
    UINTPTR F = SpinLockAcquireIrq(&D->RxLock);

    while (1) {
        E1000RxDesc *Desc = &D->RxRing[D->RxNext];

        if (!(Desc->Status & E1000_RXD_STAT_DD)) break;

        if (Desc->Errors == 0 && Desc->Length > 0) {
            NetworkBuf *Buf = NetworkAllocBuf(Desc->Length);
            if (Buf && NetworkPut(Buf, Desc->Length) == SUCCESS) {
                MemCpy(Buf->Data, D->RxBufVirt[D->RxNext], Desc->Length);
                NetworkRx(Buf, &D->NetDev);
            } else if (Buf) {
                NetworkFreeBuf(Buf);
            }
        } else {
            D->NetDev.Stats.RxErrors++;
        }

        Desc->Status = 0;
        Desc->Length = 0;
        __sync_synchronize();

        /* FIX: hand this descriptor back to the hardware by advancing
         * RDT one slot. Writing RDT = (RxNext - Processed) only works
         * when Processed < RingSize and is fragile; advancing one slot
         * per consumed descriptor is the canonical pattern. */
        UINT32 PrevIdx = D->RxNext;
        D->RxNext = (D->RxNext + 1) % E1000_RX_RING_SIZE;
        E1000Write(D, E1000_REG_RDT, PrevIdx);
    }

    SpinLockReleaseIrq(&D->RxLock, F);
}


/*
 * ============================================================================
 * Interrupt handler
 * ============================================================================
 */

VOID E1000Isr(VOID) {
    for (UINT32 I = 0; I < GE1000Count; I++) {
        E1000Device *D = &GE1000Devices[I];
        if (!D->Initialized) continue;

        UINT32 Cause = E1000Read(D, E1000_REG_ICR);
        if (Cause == 0) continue;

        E1000Write(D, E1000_REG_ICR, Cause);

        if (Cause & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0 | E1000_ICR_RXO)) {
            E1000Receive(D);
        }
        if (Cause & E1000_ICR_LSC) {
            UINT32 Status = E1000Read(D, E1000_REG_STATUS);
            D->NetDev.Up = (Status & E1000_STATUS_LU) ? TRUE : FALSE;
            StdOutPrintf("e1000: %s link %s\n", D->NetDev.Name,
                         D->NetDev.Up ? "up" : "down");
        }
        /* TXDW / TXQE need no action: the DD bit in descriptors is
         * already sufficient for the next transmit. */
    }

    HalIrqEoi();
}

/* Assembly stub, mirrors the pattern from Ps2/Sata/etc. */
EXTERN(VOID, E1000IrqStub(VOID));

/*
 * ============================================================================
 * Registration helpers
 * ============================================================================
 */

STATIC VOID E1000RegisterSysDevice(E1000Device *D, SysDevice *PciDev) {
    SysDevice *Root = PciDev ? PciDev : SysDeviceGetRoot();
    if (!Root) return;

    D->SysDev = SysDeviceCreate(Root, D->NetDev.Name,
                                "Intel E1000 network adapter",
                                SYSDEV_TYPE_NETWORK);
    if (!D->SysDev) return;

    SysDeviceSetPropU32(D->SysDev, "vendor", E1000_VENDOR_INTEL);
    SysDeviceSetPropU32(D->SysDev, "device", D->Pci->DeviceId);

    CHAR MacStr[18];
    EthernetMacNTop(D->NetDev.MacAddr, MacStr, sizeof(MacStr));
    SysDeviceSetProp(D->SysDev, "mac", MacStr);

    SysDeviceSetPropU32(D->SysDev, "mtu", D->NetDev.Mtu);
    SysDeviceSetPropU32(D->SysDev, "irq_line", D->Irq);
    SysDeviceSetPropU32(D->SysDev, "gsi", D->Gsi);
    SysDeviceSetPropU32(D->SysDev, "vector", D->Vector);
    SysDeviceSetPropHex(D->SysDev, "mmio_phys", D->MmioPhys);

    SysDeviceAddResource(D->SysDev, SYSDEV_RES_MMIO, D->MmioPhys, 0x20000);
    if (D->Gsi) {
        SysDeviceAddResource(D->SysDev, SYSDEV_RES_IRQ, D->Gsi, 1);
    }

    SysDeviceSetState(D->SysDev, SYSDEV_STATE_ENABLED);
    if (D->Driver) SysDeviceBindDriver(D->SysDev, D->Driver);
}

STATIC INT E1000Probe(PciDevice *Pci) {
    if (!Pci || Pci->VendorId != E1000_VENDOR_INTEL) return NOT_FOUND;

    switch (Pci->DeviceId) {
    case E1000_DEVICE_82540EM:
    case E1000_DEVICE_82545EM:
    case E1000_DEVICE_82574L:
    case E1000_DEVICE_82567LM:
    case E1000_DEVICE_82583V:
        return SUCCESS;
    default:
        return NOT_SUPPORTED;
    }
}

STATIC INT E1000Attach(PciDevice *Pci) {
    if (GE1000Count >= E1000_MAX_DEVICES) return NO_MEMORY;

    E1000Device *D = &GE1000Devices[GE1000Count];
    MemSet(D, 0, sizeof(E1000Device));
    SpinLockInit(&D->TxLock);
    SpinLockInit(&D->RxLock);

    D->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    /* BAR0 is the MMIO range for 8254x. */
    UINT64 Bar = Pci->Bars[0];
    if (Bar == 0) return DEVICE_INVALID;

    D->MmioPhys = Bar & ~0xF;
    D->Mmio     = (volatile UINT32*)PhysToVirt(D->MmioPhys);
    if (!D->Mmio) return NO_MEMORY;

    if (!E1000Reset(D)) return DEVICE_ERROR;

    /* Allocate rings. One page each, 16-byte aligned by construction. */
    UINT64 TxRingPhys = 0, RxRingPhys = 0;
    D->TxRing = (E1000TxDesc*)E1000AllocDma(&TxRingPhys);
    D->RxRing = (E1000RxDesc*)E1000AllocDma(&RxRingPhys);
    if (!D->TxRing || !D->RxRing) return NO_MEMORY;

    D->TxRingPhys = TxRingPhys;
    D->RxRingPhys = RxRingPhys;

    for (UINT32 I = 0; I < E1000_TX_RING_SIZE; I++) {
        D->TxBufVirt[I] = E1000AllocDma(&D->TxBufPhys[I]);
        if (!D->TxBufVirt[I]) return NO_MEMORY;
        D->TxRing[I].Status = E1000_TXD_STAT_DD;   /* reclaimed */
    }

    for (UINT32 I = 0; I < E1000_RX_RING_SIZE; I++) {
        D->RxBufVirt[I] = E1000AllocDma(&D->RxBufPhys[I]);
        if (!D->RxBufVirt[I]) return NO_MEMORY;
        D->RxRing[I].Addr   = D->RxBufPhys[I];
        D->RxRing[I].Status = 0;
    }

    E1000ProgramRings(D);

    /* Read the MAC the hardware kept in EEPROM and copy it into the
     * NetworkDevice template. */
    NetworkDevice *Nd = &D->NetDev;
    StrCpy(Nd->Name, "eth0");
    /* Append the index if there is more than one NIC. */
    if (GE1000Count > 0) {
        SnPrintf(Nd->Name, sizeof(Nd->Name), "eth%u", GE1000Count);
    }
    Nd->Type  = NETWORK_TYPE_ETHERNET;
    Nd->Mtu   = NETWORK_MTU;
    Nd->Up    = TRUE;
    Nd->Running = TRUE;
    Nd->Xmit  = E1000Transmit;
    Nd->Priv  = D;

    E1000ReadMac(D, Nd->MacAddr);

    /* IRQ routing: PCI line -> GSI -> vector. Vector 0x61 is chosen
     * above the standard set (32..38), below the resched IPI (0xF1). */
    D->Irq    = Pci->InterruptLine;
    D->Vector = 0x61;

    UINT32 Flags = HAL_IRQ_FLAG_EDGE_TRIGGERED | HAL_IRQ_FLAG_ACTIVE_HIGH;
    if (HalIrqResolve(D->Irq, &D->Gsi, &Flags) != SUCCESS) {
        D->Gsi = D->Irq;
    }

    HalIrqInstallHandler(D->Vector, (VOID(*)())E1000IrqStub, 0);
    HalIrqRoute(D->Gsi, D->Vector, HalIrqGetLocalApicId(), Flags);
    HalIrqUnmask(D->Gsi);

    INT Rc = NetworkRegisterDevice(Nd);
    if (Rc != SUCCESS) {
        StdErrPrintf("e1000: register failed (%d)\n", Rc);
        return Rc;
    }

    D->Initialized = TRUE;
    E1000EnableInterrupts(D);

    /* KDriver and SysDevice registration. */
    D->Driver = KDriverGenerateStruct("E1000", DCL1, TRUE, D, NULLPTR);
    if (D->Driver) KDriverRegister(D->Driver);

    SysDevice *PciDev = SysDeviceFind("PCI0");
    E1000RegisterSysDevice(D, PciDev);

    StdOutPrintf("e1000: %s attached (%02x:%02x:%02x:%02x:%02x:%02x)\n",
                 Nd->Name,
                 Nd->MacAddr[0], Nd->MacAddr[1], Nd->MacAddr[2],
                 Nd->MacAddr[3], Nd->MacAddr[4], Nd->MacAddr[5]);

    GE1000Count++;
    return SUCCESS;
}

VOID E1000Poll(VOID) {
    for (UINT32 I = 0; I < GE1000Count; I++) {
        if (GE1000Devices[I].Initialized) {
            E1000Receive(&GE1000Devices[I]);
        }
    }
}

/*
 * ============================================================================
 * Entry point
 * ============================================================================
 */

INT E1000Init(VOID) {
    INT Found = 0;

    PciDevice *Pci = PciGetFirst();
    while (Pci) {
        if (E1000Probe(Pci) == SUCCESS) {
            INT Rc = E1000Attach(Pci);
            if (Rc == SUCCESS) Found++;
            else StdErrPrintf("e1000: attach failed (%d)\n", Rc);
        }
        Pci = PciGetNext(Pci);
    }

    if (Found == 0) return NOT_FOUND;
    return SUCCESS;
}
