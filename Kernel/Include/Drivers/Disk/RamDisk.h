#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Types.h>

#define RAMDISK_MAX_COUNT  8
#define RAMDISK_NAME_MAX   32

// Create a RAMDisk of SizeBytes size.
// Returns a pointer to Disk, or NULLPTR in case of an error.
Disk *RamDiskCreate(const CHAR *Name, UINT64 SizeBytes);

// Destroy the RAMDisk (free up memory).
VOID RamDiskDestroy(Disk *RamDisk);

// Initialization of the RAMDisk subsystem.
VOID RamDiskInit(VOID);