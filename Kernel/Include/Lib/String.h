#pragma once

#include <Types.h>

VOID *MemCpy(VOID *Dst_, const VOID *Src_, USIZE N);
VOID *MemSet(VOID *S, INT C, USIZE N);
INT MemCmp(const VOID *Ptr1, const VOID *Ptr2, USIZE Num);
INT SecureMemCmp(const VOID *Ptr1, const VOID *Ptr2, USIZE Num);
VOID SecureMemZero(VOID *Ptr, USIZE Num);
VOID *MemMove(VOID *Dst0, const VOID *Src0, USIZE N);
VOID *MemMem(const VOID *HayStack, USIZE HayStackLen, const VOID *Needle, USIZE NeedleLen);

USIZE StrLen(const CHAR *S);
CHAR *StrCpy(CHAR *Dst, const CHAR *Src);
CHAR *StrnCpy(CHAR *Dst, const CHAR *Src, USIZE N);
CHAR *StrCat(CHAR *Dst, const CHAR *Src);
INT StrCmp(const CHAR *A, const CHAR *B);
INT StrCaseCmp(const CHAR *A, const CHAR *B);
INT StrnCmp(const CHAR *A, const CHAR *B, USIZE N);
CHAR *StrChr(const CHAR *S, INT C);
CHAR *StrrChr(const CHAR *S, INT C);
CHAR *StrnCat(CHAR *Dest, const CHAR *Src, USIZE N);
CHAR *StrTokR(CHAR *Str, const CHAR *Delim, CHAR **SavePtr);
INT NameEq(const CHAR *A, const CHAR *B, USIZE N);
CHAR *StrStr(const CHAR *HayStack, const CHAR *Needle);
CHAR *StrDup(const CHAR *S);

INT AToI(const CHAR* str);
LONG AToL(const CHAR* Str);
CHAR* UToA(UINT32 Value, CHAR* Str, INT Base);
