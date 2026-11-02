#include <Drivers/Network/Loopback.h>
#include <Network/Ethernet.h>
#include <Mem/Allocator.h>
#include <Network/IpV4.h>
#include <Lib/String.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <SysDevice.h>
#include <KDriver.h>
#include <Return.h>

STATIC NetworkDevice GLoopbackDev;
STATIC KDriver      *GLoopbackDriver = NULLPTR;
STATIC SysDevice    *GLoopbackSysDev = NULLPTR;
STATIC BOOL          GLoopbackInitialized = FALSE;

/*
 * ============================================================================
 * Driver callbacks
 * ============================================================================
 */

STATIC INT LoopbackOpen(NetworkDevice *Dev) {
    (VOID)Dev;
    return SUCCESS;
}

STATIC INT LoopbackStop(NetworkDevice *Dev) {
    (VOID)Dev;
    return SUCCESS;
}

/*
 * The loopback device accepts a fully-formed Ethernet frame, strips
 * nothing, and pushes it straight back into NetworkRx. Every layer
 * above sees the same bytes it would see on a real NIC.
 */
STATIC INT LoopbackXmit(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) return INCORRECT_VALUE;

    /* NetworkRx takes ownership and returns after the packet has been
     * fully processed. That makes ping 127.0.0.1 synchronous, which is
     * exactly what we want for testing. */
    NetworkRx(Buf, Dev);
    return SUCCESS;
}

/*
 * ============================================================================
 * Init
 * ============================================================================
 */

INT LoopbackInit(VOID) {
    if (GLoopbackInitialized) return SUCCESS;

    MemSet(&GLoopbackDev, 0, sizeof(GLoopbackDev));
    StrCpy(GLoopbackDev.Name, "lo");
    GLoopbackDev.Type = NETWORK_TYPE_LOOPBACK;
    GLoopbackDev.Mtu  = NETWORK_MTU;
    GLoopbackDev.Up   = TRUE;
    GLoopbackDev.Running = TRUE;
    GLoopbackDev.Open = LoopbackOpen;
    GLoopbackDev.Stop = LoopbackStop;
    GLoopbackDev.Xmit = LoopbackXmit;

    GLoopbackDev.MacAddr[0] = 0x02;
    GLoopbackDev.MacAddr[1] = 0x00;
    GLoopbackDev.MacAddr[2] = 0x00;
    GLoopbackDev.MacAddr[3] = 0x00;
    GLoopbackDev.MacAddr[4] = 0x00;
    GLoopbackDev.MacAddr[5] = 0x01;

    /* FIX: store the address in network byte order explicitly,
     * byte-by-byte. MemCpy of a host-order UINT32 would work on
     * little-endian only by accident. */
    GLoopbackDev.IpAddr[0] = 127;
    GLoopbackDev.IpAddr[1] = 0;
    GLoopbackDev.IpAddr[2] = 0;
    GLoopbackDev.IpAddr[3] = 1;
    GLoopbackDev.Netmask[0] = 255;
    GLoopbackDev.Netmask[1] = 0;
    GLoopbackDev.Netmask[2] = 0;
    GLoopbackDev.Netmask[3] = 0;
    GLoopbackDev.Gateway[0] = 0;
    GLoopbackDev.Gateway[1] = 0;
    GLoopbackDev.Gateway[2] = 0;
    GLoopbackDev.Gateway[3] = 0;

    INT Rc = NetworkRegisterDevice(&GLoopbackDev);
    if (Rc != SUCCESS) {
        StdErrPrintf("lo: register failed (%d)\n", Rc);
        return Rc;
    }

    IpV4RouteAdd(IPV4(127, 0, 0, 0), IPV4(255, 0, 0, 0), 0,
                 IPV4(127, 0, 0, 1), &GLoopbackDev, 0);

    GLoopbackDriver = KDriverGenerateStruct("Loopback", DCL1, TRUE,
                                            &GLoopbackDev, NULLPTR);
    if (GLoopbackDriver) KDriverRegister(GLoopbackDriver);

    SysDevice *Root = SysDeviceGetRoot();
    if (Root) {
        GLoopbackSysDev = SysDeviceCreate(Root, "lo",
                                          "Loopback interface",
                                          SYSDEV_TYPE_NETWORK);
        if (GLoopbackSysDev) {
            SysDeviceSetPropU32(GLoopbackSysDev, "mtu", GLoopbackDev.Mtu);

            CHAR MacStr[18];
            EthernetMacNTop(GLoopbackDev.MacAddr, MacStr, sizeof(MacStr));
            SysDeviceSetProp(GLoopbackSysDev, "mac", MacStr);

            SysDeviceSetProp(GLoopbackSysDev, "ip", "127.0.0.1");
            SysDeviceSetProp(GLoopbackSysDev, "netmask", "255.0.0.0");

            SysDeviceSetState(GLoopbackSysDev, SYSDEV_STATE_ENABLED);
            if (GLoopbackDriver) {
                SysDeviceBindDriver(GLoopbackSysDev, GLoopbackDriver);
            }
        }
    }

    GLoopbackInitialized = TRUE;
    StdOutPrintf("net: loopback up (127.0.0.1/8)\n");
    return SUCCESS;
}