#pragma once

#include <Types.h>

/* ============================================================================
 * Linux-compatible system call numbers (x86_64).
 *
 * Only the base ABI subset is implemented. Numbers match
 * arch/x86/entry/syscalls/syscall_64.tbl so that unmodified
 * Linux binaries (musl, glibc, busybox) can call into the kernel.
 * ========================================================================== */

#define SYS_READ           0    /* read(fd, buf, count)         */
#define SYS_WRITE          1    /* write(fd, buf, count)        */
#define SYS_OPEN           2    /* open(path, flags, mode)      */
#define SYS_CLOSE          3    /* close(fd)                    */
#define SYS_STAT           4    /* stat(path, statbuf)          */
#define SYS_FSTAT          5    /* fstat(fd, statbuf)           */
#define SYS_LSEEK          8    /* lseek(fd, offset, whence)    */
#define SYS_MMAP           9    /* mmap(addr, len, prot, flags, fd, off) */
#define SYS_MUNMAP        11    /* munmap(addr, len)            */
#define SYS_BRK           12    /* brk(addr)                    */
#define SYS_GETPID        39    /* getpid()                     */
#define SYS_EXIT          60    /* exit(code)                   */
#define SYS_GETCWD        79    /* getcwd(buf, size)            */
#define SYS_CHDIR         80    /* chdir(path)                  */
#define SYS_FCHDIR        81    /* fchdir(fd)                   */
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_UMOUNT2      166
#define SYS_REBOOT       169
#define SYS_GETDENTS64   217    /* getdents64(fd, dirp, count)  */
#define SYS_OPENAT       257    /* openat(dirfd, path, flags, mode)  */
#define SYS_MKDIRAT      258
#define SYS_NEWFSTATAT   262    /* newfstatat(dirfd, path, buf, flags) */
#define SYS_UNLINKAT     263

/* ============================================================================
 * 2KOS-specific extensions.
 *
 * Range 1100+ is used to avoid any collision with Linux and the x32 ABI.
 * These are NOT Linux-compatible and must not be called by Linux binaries.
 * ========================================================================== */

#define SYS_2KOS_KOBJ_OPEN     1100
#define SYS_2KOS_KOBJ_CLOSE    1101
#define SYS_2KOS_KOBJ_READ     1102
#define SYS_2KOS_KOBJ_WRITE    1103
#define SYS_2KOS_STDOUT_WRITE  1110
#define SYS_2KOS_STDERR_WRITE  1111
#define SYS_2KOS_STDWARN_WRITE 1112
#define SYS_2KOS_STDIN_READ    1113
#define SYS_2KOS_GETTICKS      1120
#define SYS_2KOS_ELOG          1121
#define SYS_2KOS_SLOG          1122
#define SYS_2KOS_SMPCOUNT      1123
#define SYS_2KOS_SMPINFO       1124

#define SYSCALL_MAX            2048

typedef UINT64 (*SyscallFunction)(UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                                  UINT64 Arg4, UINT64 Arg5, UINT64 Arg6);

/* Raw syscall instruction wrapper. Matches the Linux x86_64 calling convention:
 *   rax = nr, rdi, rsi, rdx, r10, r8, r9 = args, rax = return. */
STATIC INLINE UINT64 Syscall(UINT64 Num, UINT64 Arg1, UINT64 Arg2,
                              UINT64 Arg3, UINT64 Arg4, UINT64 Arg5,
                              UINT64 Arg6) {
    UINT64 Result;

    __asm__ volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "mov %5, %%r10\n\t"
        "mov %6, %%r8\n\t"
        "mov %7, %%r9\n\t"
        "syscall\n\t"
        "mov %%rax, %0"
        : "=r"(Result)
        : "r"(Num), "r"(Arg1), "r"(Arg2), "r"(Arg3),
          "r"(Arg4), "r"(Arg5), "r"(Arg6)
        : "rax", "rcx", "r11", "rdi", "rsi", "rdx", "r10", "r8", "r9", "memory"
    );

    return Result;
}

VOID SyscallInit(VOID);
UINT64 SyscallHandler(UINT64 Num, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                       UINT64 Arg4, UINT64 Arg5, UINT64 Arg6);
INT SyscallRegister(UINT64 Num, SyscallFunction Func);
INT CopyToUser(VOID *Dest, const VOID *Src, USIZE Size);