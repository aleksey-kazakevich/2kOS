#include <Types.h>
#include <Lib/String.h>
#include <Lib/StdArg.h>
#include <Lib/StdIo.h>
#include <Mem/Allocator.h>
#include <Return.h>

/*
 * =================== MEM ===================
 */
VOID *MemCpy(VOID *Dst_, const VOID *Src_, USIZE N)
{
    UINT8 *Dst = (UINT8 *)Dst_;
    const UINT8 *Src = (const UINT8 *)Src_;
    VOID *Ret = Dst_;

    if (N == 0 || Dst == Src)
        return Ret;

#if defined(__x86_64__) || defined(__i386__)
    /*
 * On x86/x86_64 rep movsb is very fast
 */
    asm volatile(
        "rep movsb"
        : "+D"(Dst), "+S"(Src), "+c"(N)
        :
        : "memory");
    return Ret;
#else
    const USIZE W = sizeof(USIZE);
    UINTPTR DstAddr = (UINTPTR)Dst;
    UINTPTR SrcAddr = (UINTPTR)Src;

    /*
 * Align dst to word boundary byte byte
 */
    while (N > 0 && (DstAddr & (W - 1)))
    {
        *Dst++ = *Src++;
        --N;
        DstAddr = (UINTPTR)Dst;
        SrcAddr = (UINTPTR)Src;
    }

    /*
 * If src alignment is equal to dst alignment, you can copy in words
 */
    if ((SrcAddr & (W - 1)) == (DstAddr & (W - 1)))
    {
        USIZE *Dw = (USIZE *)Dst;
        const USIZE *Sw = (const USIZE *)Src;
        USIZE Words = N / W;

        /*
 * Unfolding a cycle of 4 words
 */
        while (Words >= 4)
        {
            Dw[0] = Sw[0];
            Dw[1] = Sw[1];
            Dw[2] = Sw[2];
            Dw[3] = Sw[3];
            Dw += 4;
            Sw += 4;
            Words -= 4;
        }
        while (Words--)
        {
            *Dw++ = *Sw++;
        }

        /*
 * Update pointers and remaining bytes
 */
        Dst = (UINT8 *)dw;
        Src = (const UINT8 *)sw;
        N = N & (W - 1);
    }
    else
    {
        /*
 * If the alignments do not match, we leave everything byte-by-byte.
           This is safe on architectures that require alignment.
 */
    }

    /*
 * Tail bytes
 */
    while (N--)
    {
        *Dst++ = *Src++;
    }

    return Ret;
#endif
}

VOID *MemSet(VOID *S, INT C, USIZE N)
{
    UINT8 *P = (UINT8 *)S;
    UINT8 Val = (UINT8)C;
    VOID *Ret = S;

    const USIZE W = sizeof(UINTPTR);
    UINTPTR Addr = (UINTPTR)P;

    /*
 * Align byte-by-byte to word boundaries
 */
    while (N > 0 && (Addr & (W - 1)))
    {
        *P++ = Val;
        --N;
        Addr = (UINTPTR)P;
    }

    /*
 * Fill in whole words
 */
    if (N >= W)
    {
        UINTPTR Word = Val;
        Word |= Word << 8;
        Word |= Word << 16;
#ifdef __LP64__
        Word |= Word << 32;
#endif
        UINTPTR *Pw = (UINTPTR *)P;
        while (N >= W)
        {
            *Pw++ = Word;
            N -= W;
        }
        P = (UINT8 *)Pw;
    }

    /*
 * Remaining bytes
 */
    while (N--)
        *P++ = Val;

    return Ret;
}

INT MemCmp(const VOID *Ptr1, const VOID *Ptr2, USIZE Num)
{
    const UINT8 *A = (const UINT8 *)Ptr1;
    const UINT8 *B = (const UINT8 *)Ptr2;

    for (USIZE I = 0; I < Num; I++)
    {
        if (A[I] != B[I])
            return (INT)A[I] - (INT)B[I];
    }
    return 0;
}

INT SecureMemCmp(const VOID *Ptr1, const VOID *Ptr2, USIZE Num)
{
    const volatile UINT8 *A = (const volatile UINT8 *)Ptr1;
    const volatile UINT8 *B = (const volatile UINT8 *)Ptr2;
    UINT8 Diff = 0;

    for (USIZE I = 0; I < Num; I++) {
        Diff |= A[I] ^ B[I];
    }
    return Diff ? -1 : 0;
}

VOID SecureMemZero(VOID *Ptr, USIZE Num)
{
    volatile UINT8 *P = (volatile UINT8 *)Ptr;

    while (Num--) {
        *P++ = 0;
    }
}

VOID *MemMove(VOID *Dst0, const VOID *Src0, USIZE N)
{
    if (N == 0 || Dst0 == Src0)
        return Dst0;

    UINT8 *Dst = (UINT8 *)Dst0;
    const UINT8 *Src = (const UINT8 *)Src0;

    if (Dst < Src) /*
 * copy forward
 */
    {
        USIZE Word = sizeof(UINTPTR);
        while (N && ((UINTPTR)Dst & (Word - 1)))
        {
            *Dst++ = *Src++;
            --N;
        }

        UINTPTR *Dw = (UINTPTR *)Dst;
        const UINTPTR *Sw = (const UINTPTR *)Src;
        while (N >= Word)
        {
            *Dw++ = *Sw++;
            N -= Word;
        }

        Dst = (UINT8 *)Dw;
        Src = (const UINT8 *)Sw;
        while (N--)
            *Dst++ = *Src++;
    }
    else /*
 * dst > src - copy back
 */
    {
        Dst += N;
        Src += N;

        USIZE Word = sizeof(UINTPTR);
        while (N && ((UINTPTR)Dst & (Word - 1)))
        {
            *--Dst = *--Src;
            --N;
        }

        UINTPTR *Dw = (UINTPTR *)Dst;
        const UINTPTR *Sw = (const UINTPTR *)Src;
        while (N >= Word)
        {
            *--Dw = *--Sw;
            N -= Word;
        }

        Dst = (UINT8 *)Dw;
        Src = (const UINT8 *)Sw;
        while (N--)
            *--Dst = *--Src;
    }

    return Dst0;
}

VOID *MemMem(const VOID *HayStack, USIZE HayStackLen, const VOID *Needle, USIZE NeedleLen) {
    const UINT8 *H = (const UINT8 *)HayStack;
    const UINT8 *N = (const UINT8 *)Needle;
    
    if (NeedleLen == 0) return (VOID *)HayStack;
    if (HayStackLen < NeedleLen) return NULLPTR;
    
    for (USIZE I = 0; I <= HayStackLen - NeedleLen; I++) {
        USIZE J;
        for (J = 0; J < NeedleLen; J++) {
            if (H[I + J] != N[J]) break;
        }
        if (J == NeedleLen) return (VOID *)(H + I);
    }
    return NULLPTR;
}

/*
 * =================== STR ===================
 */
USIZE StrLen(const CHAR *S)
{
    const CHAR *P = S;
    while (*P)
        P++;
    return P - S;
}

CHAR *StrCpy(CHAR *Dst, const CHAR *Src)
{
    CHAR *D = Dst;
    while ((*D++ = *Src++))
        ;
    return Dst;
}

CHAR *StrnCpy(CHAR *Dst, const CHAR *Src, USIZE N)
{
    USIZE I = 0;
    for (; I < N && Src[I]; I++)
        Dst[I] = Src[I];
    for (; I < N; I++)
        Dst[I] = '\0';
    return Dst;
}

CHAR *StrCat(CHAR *Dst, const CHAR *Src)
{
    CHAR *D = Dst;
    while (*D)
        D++;
    while ((*D++ = *Src++))
        ;
    return Dst;
}

INT StrCmp(const CHAR *A, const CHAR *B)
{
    while (*A == *B && *A != '\0')
    {
        A++;
        B++;
    }
    return *(UINT8 *)A - *(UINT8 *)B;
}

static CHAR StrCaseFold(CHAR C) {
    if (C >= 'A' && C <= 'Z') {
        return (CHAR)(C + ('a' - 'A'));
    }
    return C;
}

INT StrCaseCmp(const CHAR *A, const CHAR *B)
{
    while (StrCaseFold(*A) == StrCaseFold(*B) && *A != '\0')
    {
        A++;
        B++;
    }
    return (UINT8)StrCaseFold(*A) - (UINT8)StrCaseFold(*B);
}

INT StrnCmp(const CHAR *A, const CHAR *B, USIZE N)
{
    for (USIZE I = 0; I < N; I++)
    {
        if (A[I] != B[I])
            return (UINT8)A[I] - (UINT8)B[I];
        if (A[I] == '\0')
            RETURN(SUCCESS);
    }
    RETURN(SUCCESS);
}

CHAR *StrChr(const CHAR *S, INT C)
{
    while (*S)
    {
        if (*S == (CHAR)C)
            return (CHAR *)S;
        S++;
    }
    if ((CHAR)C == '\0')
        return (CHAR *)S;
    return NULLPTR;
}

CHAR *StrrChr(const CHAR *S, INT C)
{
    const CHAR *Last = NULLPTR;
    while (*S)
    {
        if (*S == (CHAR)C)
            Last = S;
        S++;
    }
    if ((CHAR)C == '\0')
        return (CHAR *)S;
    return (CHAR *)Last;
}

CHAR *StrnCat(CHAR *Dest, const CHAR *Src, USIZE N)
{
    CHAR *D = Dest;
    while (*D)
        ++D; /*
 * let's find the end dest
 */
    USIZE I = 0;
    while (I < N && Src[I] != '\0')
    {
        D[I] = Src[I];
        ++I;
    }
    D[I] = '\0';
    return Dest;
}

CHAR *StrTokR(CHAR *Str, const CHAR *Delim, CHAR **SavePtr)
{
    CHAR *Token;

    if (Str)
        *SavePtr = Str;
    if (*SavePtr == NULLPTR)
        return NULLPTR;

    // Skip leading delimiter characters
    CHAR *Start = *SavePtr;
    while (*Start && StrChr(Delim, *Start))
        Start++;
    if (*Start == '\0')
    {
        *SavePtr = NULLPTR;
        return NULLPTR;
    }

    // Find the end of the token
    Token = Start;
    CHAR *P = Start;
    while (*P && !StrChr(Delim, *P))
        P++;

    if (*P)
    {
        *P = '\0';
        *SavePtr = P + 1;
    }
    else
    {
        *SavePtr = NULLPTR;
    }

    return Token;
}

INT NameEq(const CHAR *A, const CHAR *B, USIZE N)
{
    for (USIZE I = 0; I < N; ++I)
    {
        CHAR Ca = A[I], Cb = B[I];
        if (!Ca && !Cb)
            return 1;
        if (Ca != Cb)
            RETURN(SUCCESS);
    }
    return 1;
}

static CHAR* IToA(INT Value, CHAR* Str, INT Base) {
    CHAR* Ptr = Str;
    CHAR* Ptr1 = Str;
    CHAR TmpChar;
    INT TmpValue;
    
    if (Base < 2 || Base > 36) {
        *Str = '\0';
        return Str;
    }
    
    do {
        TmpValue = Value;
        Value /= Base;
        *Ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[TmpValue - Value * Base];
    } while (Value);
    
    *Ptr-- = '\0';
    while (Ptr1 < Ptr) {
        TmpChar = *Ptr;
        *Ptr-- = *Ptr1;
        *Ptr1++ = TmpChar;
    }
    return Str;
}

CHAR* UToA(UINT32 Value, CHAR* Str, INT Base) {
    CHAR* Ptr = Str;
    CHAR* Ptr1 = Str;
    CHAR TmpChar;
    UINT32 TmpValue;
    
    if (Base < 2 || Base > 36) {
        *Str = '\0';
        return Str;
    }
    
    do {
        TmpValue = Value;
        Value /= Base;
        *Ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[TmpValue - Value * Base];
    } while (Value);
    
    *Ptr-- = '\0';
    while (Ptr1 < Ptr) {
        TmpChar = *Ptr;
        *Ptr-- = *Ptr1;
        *Ptr1++ = TmpChar;
    }
    return Str;
}

CHAR *StrStr(const CHAR *HayStack, const CHAR *Needle)
{
    if (!HayStack || !Needle) {
        return NULLPTR;
    }
    
    // If needle is an empty string, return haystack
    if (*Needle == '\0') {
        return (CHAR *)HayStack;
    }
    
    // Quick check in case needle is longer than haystack
    USIZE NeedleLen = StrLen(Needle);
    USIZE HayStackLen = StrLen(HayStack);
    
    if (NeedleLen > HayStackLen) {
        return NULLPTR;
    }
    
    // Optimized algorithm with pre-check of the first character
    CHAR First = Needle[0];
    
    for (USIZE I = 0; I <= HayStackLen - NeedleLen; I++) {
        // Quick first character check
        if (HayStack[I] != First) {
            continue;
        }
        
        // Checking other characters
        USIZE J;
        for (J = 1; J < NeedleLen; J++) {
            if (HayStack[I + J] != Needle[J]) {
                break;
            }
        }
        
        // If all characters match, return a pointer
        if (J == NeedleLen) {
            return (CHAR *)(HayStack + I);
        }
    }
    
    return NULLPTR;
}

INT AToI(const CHAR* Str) {
    INT Result = 0;
    INT Sign = 1;
    
    if (!Str) return 0;
    
    // Skip spaces
    while (*Str == ' ' || *Str == '\t') Str++;
    
    // Sign Processing
    if (*Str == '-') {
        Sign = -1;
        Str++;
    } else if (*Str == '+') {
        Str++;
    }
    
    // Converting numbers
    while (*Str >= '0' && *Str <= '9') {
        Result = Result * 10 + (*Str - '0');
        Str++;
    }
    
    return Result * Sign;
}

// Converting a string to a long integer
LONG AToL(const CHAR* Str) {
    LONG Result = 0;
    INT Sign = 1;
    
    if (!Str) return 0;
    
    // Skip spaces
    while (*Str == ' ' || *Str == '\t') Str++;
    
    // Sign Processing
    if (*Str == '-') {
        Sign = -1;
        Str++;
    } else if (*Str == '+') {
        Str++;
    }
    
    // Converting numbers
    while (*Str >= '0' && *Str <= '9') {
        Result = Result * 10 + (*Str - '0');
        Str++;
    }
    
    return Result * Sign;
}

CHAR *StrDup(const CHAR *S) {
    USIZE Len = StrLen(S) + 1; // +1 for '\0'
    CHAR *Dup = (CHAR *)MemoryAllocate(Len);

    if (!Dup)
        return NULLPTR;

    MemCpy(Dup, S, Len);
    return Dup;
}
