#include <Types.h>
#include <Limine/LimineParse.h>
#include <Limine/limine.h>
#include <Drivers/Framebuffer.h>

VOID InitFramebuffer(VOID) {
    if (!LimineFramebufferAvailable()) {
        return;
    }

    struct limine_framebuffer *Fb = LimineGetFramebuffer();
    if (!Fb || !Fb->address) {
        return;
    }

    // Getting HHDM offset
    UINT64 HhdmOffset = LimineGetHHDMOffset();
    UINT64 RawAddr = (UINT64)Fb->address;
    
    // Convert to physical address if necessary
    UINT64 PhysAddr = (RawAddr >= HhdmOffset) ? (RawAddr - HhdmOffset) : RawAddr;
    
    // Calculating bytes per pixel
    UINT8 BytesPerPixel = (UINT8)((Fb->bpp + 7) / 8);
    if (BytesPerPixel == 0 || BytesPerPixel > 4) {
        BytesPerPixel = 4;
    }
    
    // Initializing the framebuffer
    FramebufferInit(PhysAddr, Fb->width, Fb->height, Fb->pitch, BytesPerPixel);

    FramebufferClear(RGB_BLACK);
}
