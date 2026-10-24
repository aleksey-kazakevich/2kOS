#pragma once

/*
 * Linux-compatible errno values (x86_64).
 *
 * Syscall handlers must return negative values in the range [-4095, -1]
 * to signal an error. Userspace (musl, glibc) interprets these as -errno.
 *
 * Returning plain -1 is WRONG: -1 means -EPERM, not "generic error".
 */

#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
#define ESRCH        3   /* No such process */
#define EINTR        4   /* Interrupted system call */
#define EIO          5   /* I/O error */
#define ENXIO        6   /* No such device or address */
#define E2BIG        7   /* Argument list too long */
#define ENOEXEC      8   /* Exec format error */
#define EBADF        9   /* Bad file descriptor */
#define ECHILD      10   /* No child processes */
#define EAGAIN      11   /* Try again */
#define ENOMEM      12   /* Out of memory */
#define EACCES      13   /* Permission denied */
#define EFAULT      14   /* Bad address */
#define ENOTBLK     15   /* Block device required */
#define EBUSY       16   /* Device or resource busy */
#define EEXIST      17   /* File exists */
#define EXDEV       18   /* Cross-device link */
#define ENODEV      19   /* No such device */
#define ENOTDIR     20   /* Not a directory */
#define EISDIR      21   /* Is a directory */
#define EINVAL      22   /* Invalid argument */
#define ENFILE      23   /* File table overflow */
#define EMFILE      24   /* Too many open files */
#define ENOTTY      25   /* Not a typewriter */
#define ETXTBSY     26   /* Text file busy */
#define EFBIG       27   /* File too large */
#define ENOSPC      28   /* No space left on device */
#define ESPIPE      29   /* Illegal seek */
#define EROFS       30   /* Read-only file system */
#define EMLINK      31   /* Too many links */
#define EPIPE       32   /* Broken pipe */
#define EDOM        33   /* Math argument out of domain */
#define ERANGE      34   /* Math result not representable */

#define ENAMETOOLONG 36

#define ENOSYS      38   /* Function not implemented */
#define ENOTEMPTY   39   /* Directory not empty */
#define ELOOP       40   /* Too many symbolic links */

#define EOVERFLOW   75   /* Value too large for data type */

#define ENOTSUP     EOPNOTSUPP
#define EOPNOTSUPP  95   /* Operation not supported */

/*
 * Syscall-specific helpers.
 *
 * Every syscall handler MUST return:
 *   >= 0        — success value (bytes read/written, fd, address, pid, ...)
 *   -EXXX       — error (ERRNO_IS_ERR(ret) is TRUE)
 *
 * Ranges:
 *   [-4095, -1] — errno (userspace translates to -ret)
 *   [-1]        — alias for -EPERM, do NOT use as "generic error"
 */

#define ERRNO_IS_ERR(Ret)  ((long long)(Ret) < 0 && (long long)(Ret) >= -4095)