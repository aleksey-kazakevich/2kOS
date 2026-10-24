#pragma once

#include <Lib/StdArg.h>
#include <KObj/KObj.h>
#include <Types.h>

/*
 * StdIo — the I/O layer above KObj.
 *
 * Each consumer (Basecon, GUI console, future pipes) has its own KObj
 * under \Kernel\Consoles\<name>, backed by its own ring buffer.
 *
 * Channel binds are per-task:
 *   Cur->StdoutObj, Cur->StderrObj, Cur->StdwarnObj, Cur->StdinObj.
 * If a bind is missing, the DEFAULT console is used (\Kernel\Consoles\Basecon).
 *
 * The consumer (kconsoleout) reads the console object and renders it.
 * Producers never render — they only write to the ring.
 */

typedef enum {
    STDIO_CHAN_STDOUT  = 0,
    STDIO_CHAN_STDERR  = 1,
    STDIO_CHAN_STDWARN = 2,
    STDIO_CHAN_STDIN   = 3,
    STDIO_CHAN_MAX     = 4,
} StdIoChannel;

#define STDIO_MSG_MAX     1024

/* Lifecycle */
VOID  StdIoInit(VOID);
KObj *StdIoCreateConsole(const CHAR *Name);     /* refcount+1 */
KObj *StdIoGetDefaultConsole(VOID);              /* refcount+1 */

/* Channel binding (per current task) */
INT   StdIoBind(StdIoChannel Ch, KObj *Obj);    /* takes a ref */
VOID  StdIoUnbind(StdIoChannel Ch);
KObj *StdIoGetBinding(StdIoChannel Ch);         /* refcount+1 */

/* Channel write/read */
INT StdIoWriteChannel(StdIoChannel Ch, const VOID *Buf, UINT32 Size);
INT StdIoPrintChannel(StdIoChannel Ch, const CHAR *Str);
INT StdIoPrintfChannel(StdIoChannel Ch, const CHAR *Fmt, ...);
INT StdIoReadChannel(StdIoChannel Ch, VOID *Buf, UINT32 Size, UINT32 *Read);

/* Explicit target (for kernel code that has a KObj already) */
INT StdIoWriteTo(KObj *Con, const VOID *Buf, UINT32 Size);
INT StdIoPrintTo(KObj *Con, const CHAR *Str);
INT StdIoPrintfTo(KObj *Con, const CHAR *Fmt, ...);
INT StdIoReadFrom(KObj *Con, VOID *Buf, UINT32 Size, UINT32 *Read);

/* Kernel logs — always go to the DEFAULT console, NEVER to Cur->StdoutObj */
INT KLogPrint(const CHAR *Str);
INT KLogPrintf(const CHAR *Fmt, ...);