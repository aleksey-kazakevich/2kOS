#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Fs/Vfs.h>
#include <Types.h>

/*
 * Mount an ISO9660 image stored as a regular file in the VFS.
 *
 * The image is exposed as a virtual Disk (IsVirtual = TRUE) backed
 * by VfsFile reads. The Disk is registered in DiskMgr and then mounted
 * with the standard "iso9660" filesystem under MountName.
 *
 * BaseDir   - starting directory for resolving FilePath
 *             (NULLPTR means VfsCurrentDir).
 * FilePath  - path to the .iso file.
 * MountName - name of the mount point (e.g. "myiso"). If NULLPTR or empty,
 *             the basename of FilePath is used.
 *
 * On success the mount point becomes visible as "<MountName>::/".
 * When the volume is unmounted (via the "unmount" command or
 * VfsUnmountDrive), the virtual Disk is automatically unregistered
 * and the backing file is closed.
 */
INT IsoMountFromFile(VfsInode *BaseDir,
                     const CHAR *FilePath,
                     const CHAR *MountName);;

/*
 * Mount an ISO9660 image that is already resident in memory
 * (e.g. a Limine module). The buffer is NOT copied and NOT freed;
 * the caller must guarantee that it stays valid for the entire
 * lifetime of the mount.
 *
 * Data   - pointer to the image bytes.
 * Size   - size in bytes (rounded up to 2048 internally).
 * Name   - mount point name (e.g. "USER"). Must not be empty.
 */
INT IsoMountFromMemory(const VOID *Data, UINT64 Size, const CHAR *Name);

/*
 * Register an in-memory ISO as a virtual Disk without mounting it.
 * The Disk becomes visible in DiskMgr and can be mounted later with:
 *
 *      mount <Name>              (autodetect iso9660)
 *      mount <Name> iso9660      (explicit)
 *
 * The image bytes are NOT copied and NOT freed here.
 * Returns SUCCESS or an error code.
 */
INT IsoRegisterFromMemory(const VOID *Data, UINT64 Size, const CHAR *Name);