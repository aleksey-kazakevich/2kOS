#pragma once

/*
 * Linux x86_64 open() flags and *at() constants.
 *
 * Values match asm-generic/fcntl.h. They are part of the syscall ABI
 * and must not be renumbered: binaries built against musl or glibc
 * pass these exact numbers to the kernel via open() / openat().
 *
 * Inside the kernel, VfsOpen() and friends use a separate set of
 * internal flags (VFS_O_* in <Vfs.h>). The syscall layer translates
 * Linux O_* into VFS_O_* before calling into the VFS.
 */

/* Access mode (low 2 bits). */
#define O_RDONLY        00000000
#define O_WRONLY        00000001
#define O_RDWR          00000002
#define O_ACCMODE       00000003

/* Open-time flags. */
#define O_CREAT         00000100   /* 0x40    */
#define O_EXCL          00000200   /* 0x80    */
#define O_NOCTTY        00000400
#define O_TRUNC         00001000   /* 0x200   */
#define O_APPEND        00002000   /* 0x400   */
#define O_NONBLOCK      00004000
#define O_DSYNC         00010000
#define O_DIRECT        00040000
#define O_LARGEFILE     00100000
#define O_DIRECTORY     00200000   /* 0x10000 */
#define O_NOFOLLOW      00400000
#define O_NOATIME       01000000
#define O_CLOEXEC       02000000

/* AT_* flags for *at() syscalls. */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_NO_AUTOMOUNT     0x800
#define AT_EMPTY_PATH       0x1000

/* Maximum number of symlinks to follow in one path resolution. */
#define MAXSYMLINKS         40