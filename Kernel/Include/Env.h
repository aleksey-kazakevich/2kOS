#pragma once

#include <Types.h>

#define ENV_MAX_VARS     64
#define ENV_NAME_MAX     32
#define ENV_VALUE_MAX    256
#define ENV_PATH_MAX     (ENV_VALUE_MAX * 4)

#define ENV_FILE_PATH    "/etc/env.conf"

/*
 * Environment for the kernel shell and (in the future) user tasks.
 *
 * Storage is a flat table of NAME=VALUE pairs, loaded from
 * <root>::/etc/env.conf on boot. PATH is treated specially:
 * it is a colon-separated list of directories.
 */

/* Lifecycle. Call EnvInit() early, then EnvLoadRoot() after root is mounted. */
VOID EnvInit(VOID);
VOID EnvLoadRoot(VOID);       /* reads ENV_FILE_PATH from the current root */

/* Read/write a single variable. */
const CHAR *EnvGet(const CHAR *Name);                /* NULLPTR if missing   */
INT         EnvSet(const CHAR *Name, const CHAR *Value);
INT         EnvUnset(const CHAR *Name);

/* PATH helpers. */
const CHAR *EnvGetPath(VOID);                         /* never NULLPTR       */
INT         EnvSetPath(const CHAR *Path);

/*
 * Resolve an executable name against PATH.
 *
 * Searches in this order:
 *   1. each directory in PATH, relative to the current root mount
 *      (e.g. <root>::/bin/<Name>)
 *   2. VfsCurrentDir/<Name>   (legacy behaviour)
 *   3. VfsCurrentDir/<Name>.elf
 *
 * Returns SUCCESS and fills OutPath with the full VFS path
 * (e.g. "SATA1::/bin/echo") if found.
 */
INT EnvResolveExec(const CHAR *Name, CHAR *OutPath, USIZE OutSize);

/* Debug: dump all variables to stdout. */
VOID EnvDump(VOID);