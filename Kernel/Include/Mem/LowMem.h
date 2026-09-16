#pragma once

#include <Types.h>

#define LOWMEM_MAX_ADDR     0x1000000ULL    // 16 MB
#define LOWMEM_SIZE         (256 * 1024)    // 256 KB зарезервируем
#define LOWMEM_ALIGN        0x10000ULL      // 64 KB

// Инициализация. Вызывать ОДИН РАЗ, ДО PhysAllocInit.
// Найдёт contiguous region ниже 16 MB, выровненный на 64 KB,
// и запомнит его. PMM об этих страницах не узнает.
VOID LowMemInit(VOID);

// Аллоцировать N страниц ниже 16 MB (выравнивание 64 KB).
// Возвращает HHDM-виртуальный адрес или NULLPTR.
VOID* LowMemAllocate(UINT32 PageCount);

// Освободить (no-op для bump-аллокатора, но API есть).
VOID LowMemFree(VOID *Addr, UINT32 PageCount);

// Физический адрес по HHDM-указателю.
UINT64 LowMemPhys(VOID *VirtAddr);

// Сколько свободно.
UINT32 LowMemFreePages(VOID);

UINT64 LowMemPhysBase(VOID);