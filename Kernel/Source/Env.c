#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Io/StdWarn.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Env.h>

typedef struct {
    CHAR Name[ENV_NAME_MAX];
    CHAR Value[ENV_VALUE_MAX];
    BOOL Used;
} EnvVar;

STATIC EnvVar GEnv[ENV_MAX_VARS];
STATIC BOOL   GEnvReady = FALSE;
STATIC CHAR   GDefaultPath[] = "/bin";

STATIC EnvVar *EnvFind(const CHAR *Name) {
    if (!Name) return NULLPTR;
    for (UINT32 I = 0; I < ENV_MAX_VARS; I++) {
        if (GEnv[I].Used && StrCmp(GEnv[I].Name, Name) == 0) return &GEnv[I];
    }
    return NULLPTR;
}

STATIC EnvVar *EnvAlloc(const CHAR *Name) {
    for (UINT32 I = 0; I < ENV_MAX_VARS; I++) {
        if (!GEnv[I].Used) {
            MemSet(&GEnv[I], 0, sizeof(EnvVar));
            StrnCpy(GEnv[I].Name, Name, ENV_NAME_MAX - 1);
            GEnv[I].Used = TRUE;
            return &GEnv[I];
        }
    }
    return NULLPTR;
}

VOID EnvInit(VOID) {
    MemSet(GEnv, 0, sizeof(GEnv));
    GEnvReady = TRUE;
    EnvSetPath(GDefaultPath);
}

INT EnvSet(const CHAR *Name, const CHAR *Value) {
    if (!Name || !Name[0]) RETURN(INCORRECT_VALUE);
    EnvVar *V = EnvFind(Name);
    if (!V) V = EnvAlloc(Name);
    if (!V) RETURN(NO_MEMORY);
    StrnCpy(V->Value, Value ? Value : "", ENV_VALUE_MAX - 1);
    V->Value[ENV_VALUE_MAX - 1] = '\0';
    RETURN(SUCCESS);
}

INT EnvUnset(const CHAR *Name) {
    EnvVar *V = EnvFind(Name);
    if (!V) RETURN(NOT_FOUND);
    MemSet(V, 0, sizeof(EnvVar));
    RETURN(SUCCESS);
}

const CHAR *EnvGet(const CHAR *Name) {
    EnvVar *V = EnvFind(Name);
    return V ? V->Value : NULLPTR;
}

const CHAR *EnvGetPath(VOID) {
    const CHAR *P = EnvGet("PATH");
    return P ? P : GDefaultPath;
}

INT EnvSetPath(const CHAR *Path) {
    return EnvSet("PATH", Path ? Path : GDefaultPath);
}

/* ----------------------------------------------------------------------------
 * Parsing /etc/env.conf
 * -------------------------------------------------------------------------- */

STATIC VOID TrimRight(CHAR *S) {
    USIZE L = StrLen(S);
    while (L > 0 && (S[L-1] == ' ' || S[L-1] == '\t' ||
                     S[L-1] == '\r' || S[L-1] == '\n')) {
        S[--L] = '\0';
    }
}

STATIC VOID TrimLeft(CHAR **S) {
    while (**S == ' ' || **S == '\t') (*S)++;
}

STATIC VOID EnvParseLine(CHAR *Line) {
    TrimRight(Line);
    CHAR *P = Line;
    TrimLeft(&P);
    if (!*P || *P == '#' || *P == ';') return;

    CHAR *Eq = StrChr(P, '=');
    if (!Eq) return;

    *Eq = '\0';
    CHAR *Name = P;
    CHAR *Value = Eq + 1;
    TrimRight(Name);
    TrimLeft(&Value);

    USIZE VL = StrLen(Value);
    if (VL >= 2 && ((Value[0] == '"' && Value[VL-1] == '"') ||
                    (Value[0] == '\'' && Value[VL-1] == '\''))) {
        Value[VL-1] = '\0';
        Value++;
    }

    EnvSet(Name, Value);
}

VOID EnvLoadRoot(VOID) {
    if (!GEnvReady) EnvInit();
    if (!VfsRootInode) {
        StdWarnPrintf("env: VFS not ready, using defaults\n");
        return;
    }

    VfsFile *F = NULLPTR;
    if (VfsOpen(VfsRootInode, ENV_FILE_PATH, VFS_O_READ, &F) != SUCCESS) {
        if (!VfsCurrentDir ||
            VfsOpen(VfsCurrentDir, ENV_FILE_PATH, VFS_O_READ, &F) != SUCCESS) {
            StdOutPrintf("env: %s not found, using defaults\n", ENV_FILE_PATH);
            return;
        }
    }

    UINT64 Size = F->FInode ? F->FInode->ISize : 0;
    if (Size == 0 || Size > 64 * 1024) {
        VfsClose(F);
        StdWarnPrintf("env: %s is empty or too large\n", ENV_FILE_PATH);
        return;
    }

    CHAR *Buf = (CHAR *)MemoryAllocate((USIZE)Size + 1);
    if (!Buf) { VfsClose(F); return; }

    UINT32 Got = 0;
    INT Rc = VfsRead(F, Buf, (UINT32)Size, &Got);
    VfsClose(F);
    if (Rc != SUCCESS || Got == 0) { MemoryFree(Buf); return; }
    Buf[Got] = '\0';

    CHAR *Save = NULLPTR;
    CHAR *Line = StrTokR(Buf, "\n", &Save);
    while (Line) {
        EnvParseLine(Line);
        Line = StrTokR(NULLPTR, "\n", &Save);
    }
    MemoryFree(Buf);

    if (!EnvGet("PATH")) EnvSetPath(GDefaultPath);
    StdOutPrintf("env: loaded %s (PATH=%s)\n",
                 ENV_FILE_PATH, EnvGetPath());
}

/* ----------------------------------------------------------------------------
 * PATH resolution
 * -------------------------------------------------------------------------- */

/* Build "<root>::<Dir>/<Name>" or "<root>::/<Name>" if Dir is empty.
 * Writes into Out (size OutSize). Returns SUCCESS or error. */
STATIC INT EnvBuildCandidate(const CHAR *Dir, const CHAR *Name,
                             CHAR *Out, USIZE OutSize) {
    CHAR Root[DRIVE_NAME_MAX];
    if (VfsGetFirstDriveName(Root, sizeof(Root)) != SUCCESS) {
        RETURN(NO_OBJECT);
    }
    SnPrintf(Out, OutSize, "%s::/%s/%s", Root, Dir, Name);
    RETURN(SUCCESS);
}

STATIC BOOL EnvIsExecutable(const CHAR *Path) {
    VfsInode *Ino = NULLPTR;
    INT Rc = VfsWalk(VfsRootInode, Path, &Ino);
    if (Rc != SUCCESS && VfsCurrentDir) {
        Rc = VfsWalk(VfsCurrentDir, Path, &Ino);
    }
    if (Rc != SUCCESS) return FALSE;
    BOOL Ok = (Ino->IMode == FT_REG_FILE);
    VfsInodeUnref(Ino);
    return Ok;
}

/* Try one PATH entry. Returns SUCCESS and Out if found. */
STATIC INT EnvTryPathEntry(const CHAR *Dir, const CHAR *Name,
                           CHAR *Out, USIZE OutSize) {
    /* Dir may be "/bin" or "bin". Normalize to "bin". */
    const CHAR *Clean = Dir;
    if (Clean[0] == '/') Clean++;

    CHAR Candidate[PATH_MAX];
    INT Rc = EnvBuildCandidate(Clean, Name, Candidate, sizeof(Candidate));
    if (Rc != SUCCESS) RETURN(Rc);

    if (EnvIsExecutable(Candidate)) {
        StrnCpy(Out, Candidate, OutSize - 1);
        Out[OutSize - 1] = '\0';
        RETURN(SUCCESS);
    }

    /* Also try <Name>.elf. */
    CHAR CandidateElf[PATH_MAX];
    EnvBuildCandidate(Clean, Name, CandidateElf, sizeof(CandidateElf));
    {
        CHAR WithExt[PATH_MAX];
        SnPrintf(WithExt, sizeof(WithExt), "%s.elf", CandidateElf);
        if (EnvIsExecutable(WithExt)) {
            StrnCpy(Out, WithExt, OutSize - 1);
            Out[OutSize - 1] = '\0';
            RETURN(SUCCESS);
        }
    }

    RETURN(NOT_FOUND);
}

INT EnvResolveExec(const CHAR *Name, CHAR *OutPath, USIZE OutSize) {
    if (!Name || !OutPath || OutSize == 0) RETURN(INCORRECT_VALUE);
    if (!GEnvReady) EnvInit();

    /* 1. Each PATH entry. */
    const CHAR *Path = EnvGetPath();
    CHAR PathCopy[ENV_PATH_MAX];
    StrnCpy(PathCopy, Path, sizeof(PathCopy) - 1);
    PathCopy[sizeof(PathCopy) - 1] = '\0';

    CHAR *Save = NULLPTR;
    CHAR *Dir = StrTokR(PathCopy, ":", &Save);
    while (Dir) {
        if (Dir[0]) {
            if (EnvTryPathEntry(Dir, Name, OutPath, OutSize) == SUCCESS)
                RETURN(SUCCESS);
        }
        Dir = StrTokR(NULLPTR, ":", &Save);
    }

    /* 2. Absolute path (with ::) — try as-is. */
    if (Name[0] == '/' || StrStr(Name, "::")) {
        if (EnvIsExecutable(Name)) {
            StrnCpy(OutPath, Name, OutSize - 1);
            OutPath[OutSize - 1] = '\0';
            RETURN(SUCCESS);
        }
    }

    /* 3. Relative to cwd (legacy). */
    if (EnvIsExecutable(Name)) {
        StrnCpy(OutPath, Name, OutSize - 1);
        OutPath[OutSize - 1] = '\0';
        RETURN(SUCCESS);
    }
    {
        CHAR WithExt[PATH_MAX];
        SnPrintf(WithExt, sizeof(WithExt), "%s.elf", Name);
        if (EnvIsExecutable(WithExt)) {
            StrnCpy(OutPath, WithExt, OutSize - 1);
            OutPath[OutSize - 1] = '\0';
            RETURN(SUCCESS);
        }
    }

    RETURN(NOT_FOUND);
}

/* ----------------------------------------------------------------------------
 * Diagnostics
 * -------------------------------------------------------------------------- */

VOID EnvDump(VOID) {
    StdOutPrintf("\nEnvironment:\n");
    for (UINT32 I = 0; I < ENV_MAX_VARS; I++) {
        if (!GEnv[I].Used) continue;
        StdOutPrintf("  %-16s = %s\n", GEnv[I].Name, GEnv[I].Value);
    }
    StdOutPrintf("\n");
}