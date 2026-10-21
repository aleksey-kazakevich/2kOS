#include <Drivers/Acpi/AcpiTables.h>
#include <Drivers/Acpi/Aml.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Basecon.h>
#include <Asm/Io.h>
#include <Types.h>

// ============================================================================
// AML constants
// ============================================================================

#define AML_ZERO_OP             0x00
#define AML_ONE_OP              0x01
#define AML_ALIAS_OP            0x06
#define AML_NAME_OP             0x08
#define AML_BYTE_PREFIX         0x0A
#define AML_WORD_PREFIX         0x0B
#define AML_DWORD_PREFIX        0x0C
#define AML_STRING_PREFIX       0x0D
#define AML_QWORD_PREFIX        0x0E
#define AML_SCOPE_OP            0x10
#define AML_BUFFER_OP           0x11
#define AML_PACKAGE_OP          0x12
#define AML_VAR_PACKAGE_OP      0x13
#define AML_METHOD_OP           0x14
#define AML_EXTERNAL_OP         0x15
#define AML_RETURN_OP           0xA4

#define AML_LOCAL0_OP           0x60
#define AML_LOCAL1_OP           0x61
#define AML_LOCAL2_OP           0x62
#define AML_LOCAL3_OP           0x63
#define AML_LOCAL4_OP           0x64
#define AML_LOCAL5_OP           0x65
#define AML_LOCAL6_OP           0x66
#define AML_LOCAL7_OP           0x67
#define AML_ARG0_OP             0x68
#define AML_ARG1_OP             0x69
#define AML_ARG2_OP             0x6A
#define AML_ARG3_OP             0x6B
#define AML_ARG4_OP             0x6C
#define AML_ARG5_OP             0x6D
#define AML_ARG6_OP             0x6E
#define AML_STORE_OP            0x70
#define AML_REF_OF_OP           0x71
#define AML_ADD_OP              0x72
#define AML_CONCAT_OP           0x73
#define AML_SUBTRACT_OP         0x74
#define AML_INCREMENT_OP        0x75
#define AML_DECREMENT_OP        0x76
#define AML_MULTIPLY_OP         0x77
#define AML_DIVIDE_OP           0x78
#define AML_SHIFT_LEFT_OP       0x79
#define AML_SHIFT_RIGHT_OP      0x7A
#define AML_AND_OP              0x7B
#define AML_NAND_OP             0x7C
#define AML_OR_OP               0x7D
#define AML_NOR_OP              0x7E
#define AML_XOR_OP              0x7F
#define AML_NOT_OP              0x80
#define AML_FIND_SET_LEFT_BIT_OP 0x81
#define AML_FIND_SET_RIGHT_BIT_OP 0x82
#define AML_DEREF_OF_OP         0x83
#define AML_CONCAT_RES_OP       0x84
#define AML_MOD_OP              0x85
#define AML_NOTIFY_OP           0x86
#define AML_SIZE_OF_OP          0x87
#define AML_INDEX_OP            0x88
#define AML_MATCH_OP            0x89
#define AML_CREATE_DWORD_FIELD_OP 0x8A
#define AML_CREATE_WORD_FIELD_OP 0x8B
#define AML_CREATE_BYTE_FIELD_OP 0x8C
#define AML_CREATE_BIT_FIELD_OP  0x8D
#define AML_OBJECT_TYPE_OP      0x8E
#define AML_CREATE_QWORD_FIELD_OP 0x8F

#define AML_LAND_OP             0x90
#define AML_LOR_OP              0x91
#define AML_LNOT_OP             0x92
#define AML_LEQUAL_OP           0x93
#define AML_LGREATER_OP         0x94
#define AML_LLESS_OP            0x95
#define AML_TO_BUFFER_OP        0x96
#define AML_TO_DECIMAL_STRING_OP 0x97
#define AML_TO_HEX_STRING_OP    0x98
#define AML_TO_INTEGER_OP       0x99
#define AML_TO_STRING_OP        0x9C
#define AML_COPY_OBJECT_OP      0x9D
#define AML_MID_OP              0x9E
#define AML_CONTINUE_OP         0x9F
#define AML_IF_OP               0xA0
#define AML_ELSE_OP             0xA1
#define AML_WHILE_OP            0xA2
#define AML_NOOP_OP             0xA3
#define AML_BREAK_OP            0xA5
#define AML_BREAK_POINT_OP      0xCC
#define AML_ONES_OP             0xFF

#define AML_EXT_OP_PREFIX       0x5B
#define AML_EXT_MUTEX_OP        0x01
#define AML_EXT_EVENT_OP        0x02
#define AML_EXT_COND_REF_OF_OP  0x12
#define AML_EXT_CREATE_FIELD_OP 0x13
#define AML_EXT_LOAD_TABLE_OP   0x1F
#define AML_EXT_LOAD_OP         0x20
#define AML_EXT_STALL_OP        0x21
#define AML_EXT_SLEEP_OP        0x22
#define AML_EXT_ACQUIRE_OP      0x23
#define AML_EXT_SIGNAL_OP       0x24
#define AML_EXT_WAIT_OP         0x25
#define AML_EXT_RESET_OP        0x26
#define AML_EXT_RELEASE_OP      0x27
#define AML_EXT_FROM_BCD_OP     0x28
#define AML_EXT_TO_BCD_OP       0x29
#define AML_EXT_UNLOAD_OP       0x2A
#define AML_EXT_REVISION_OP     0x30
#define AML_EXT_DEBUG_OP        0x31
#define AML_EXT_FATAL_OP        0x32
#define AML_EXT_TIMER_OP        0x33
#define AML_EXT_OPREGION_OP     0x80
#define AML_EXT_FIELD_OP        0x81
#define AML_EXT_DEVICE_OP       0x82
#define AML_EXT_PROCESSOR_OP    0x83
#define AML_EXT_POWERRES_OP     0x84
#define AML_EXT_THERMALZONE_OP  0x85
#define AML_EXT_INDEXFIELD_OP   0x86
#define AML_EXT_BANKFIELD_OP    0x87

#define AML_MAX_NAMESPACE       2048
#define AML_MAX_LOCALS          8
#define AML_MAX_ARGS            7
#define AML_MAX_OPREGIONS       64
#define AML_MAX_FIELDS          256
#define AML_MAX_METHOD_DEPTH    32
#define AML_MAX_INSTRUCTIONS    100000   // anti-freeze protection

// ============================================================================
// Namespace entry
// ============================================================================

typedef enum {
    AML_ENTRY_SCOPE = 0,
    AML_ENTRY_NAME,
    AML_ENTRY_METHOD,
    AML_ENTRY_DEVICE,
    AML_ENTRY_ALIAS,
    AML_ENTRY_OPREGION,
    AML_ENTRY_FIELD,
    AML_ENTRY_INDEXFIELD,
    AML_ENTRY_BANKFIELD,
    AML_ENTRY_PROCESSOR,
    AML_ENTRY_POWERRES,
    AML_ENTRY_THERMALZONE,
} AmlEntryKind;

struct AmlName {
    CHAR Segment[5];
    UINT32 Parent;
    AmlEntryKind Kind;
    const UINT8 *Code;
    UINT32 CodeLen;
    AmlValue Value;
    BOOL ValueCached;
};

// Operation Region
typedef struct {
    CHAR   RegionName[5];
    UINT8  SpaceId;      // 0=Mem, 1=IO, 2=PCI_Config, 3=EC, ...
    UINT64 Offset;
    UINT64 Length;
} AmlOpRegion;

// Field inside the OpRegion
typedef struct {
    CHAR   FieldName[5];
    UINT32 RegionIdx;
    UINT32 BitOffset;
    UINT32 BitWidth;
    UINT8  AccessType;
    UINT8  LockRule;
    UINT8  UpdateRule;
} AmlField;

typedef struct {
    AmlName Entries[AML_MAX_NAMESPACE];
    UINT32 Count;

    AmlOpRegion OpRegions[AML_MAX_OPREGIONS];
    UINT32 OpRegionCount;

    AmlField Fields[AML_MAX_FIELDS];
    UINT32 FieldCount;
} AmlNamespace;

STATIC AmlNamespace GAml = {0};
STATIC UINT32 GInstructionCounter = 0;

// ============================================================================
// Helpers
// ============================================================================

STATIC VOID CopyNameSeg4(const UINT8 *Src, CHAR *Dst) {
    Dst[0] = (CHAR)Src[0];
    Dst[1] = (CHAR)Src[1];
    Dst[2] = (CHAR)Src[2];
    Dst[3] = (CHAR)Src[3];
    Dst[4] = '\0';
    for (INT I = 3; I >= 0; I--) {
        if (Dst[I] == '_') Dst[I] = '\0';
        else break;
    }
}

STATIC BOOL NameSegEq(const CHAR *A, const CHAR *B) {
    for (INT I = 0; I < 4; I++) {
        CHAR Ca = A[I] ? A[I] : '_';
        CHAR Cb = B[I] ? B[I] : '_';
        if (Ca != Cb) return FALSE;
    }
    return TRUE;
}

STATIC UINT32 ReadPkgLength(const UINT8 **Ptr, const UINT8 *End) {
    const UINT8 *P = *Ptr;
    if (P >= End) { *Ptr = End; return 0; }
    UINT8 Lead = *P++;
    UINT32 Follow = (Lead >> 6) & 0x03;
    UINT32 Length = Lead & 0x3F;

    if (Follow > 0) {
        Length = 0;
        for (UINT32 I = 0; I < Follow && P < End; I++) {
            Length |= ((UINT32)(*P++)) << (8 * I);
        }
    }
    *Ptr = P;
    return Length;
}

STATIC INT ReadNameString(const UINT8 **Ptr, const UINT8 *End,
                          CHAR Segments[][5], INT MaxSegs,
                          BOOL *IsRoot) {
    const UINT8 *P = *Ptr;
    INT Count = 0;
    *IsRoot = FALSE;

    if (P >= End) { *Ptr = P; return 0; }

    if (*P == '\\') {
        *IsRoot = TRUE;
        P++;
    }

    while (P < End && *P == '^') {
        if (Count < MaxSegs) {
            StrCpy(Segments[Count], "^");
            Count++;
        }
        P++;
    }

    if (P < End && *P == 0x00) {
        P++;
        *Ptr = P;
        return Count;
    }

    INT NameSegCount = 1;
    if (P < End && *P == 0x2F) {
        NameSegCount = 2;
        P++;
    } else if (P < End && *P == 0x2E) {
        P++;
        if (P >= End) { *Ptr = P; return Count; }
        NameSegCount = *P++;
    }

    for (INT I = 0; I < NameSegCount && Count < MaxSegs; I++) {
        if (P + 4 > End) break;
        UINT8 C = P[0];
        BOOL IsLeadChar = (C == '_' || (C >= 'A' && C <= 'Z'));
        if (!IsLeadChar) break;
        CopyNameSeg4(P, Segments[Count]);
        Count++;
        P += 4;
    }

    *Ptr = P;
    return Count;
}

// ============================================================================
// Namespace construction
// ============================================================================

STATIC UINT32 NsAdd(UINT32 Parent, const CHAR *Segment, AmlEntryKind Kind,
                    const UINT8 *Code, UINT32 CodeLen) {
    if (GAml.Count >= AML_MAX_NAMESPACE) return (UINT32)-1;
    UINT32 Idx = GAml.Count++;
    AmlName *E = &GAml.Entries[Idx];
    StrnCpy(E->Segment, Segment, 4);
    E->Segment[4] = '\0';
    E->Parent = Parent;
    E->Kind = Kind;
    E->Code = Code;
    E->CodeLen = CodeLen;
    E->Value.Kind = AMLV_UNINITIALIZED;
    E->Value.Integer = 0;
    E->Value.NameRef = NULLPTR;
    E->Value.Data = NULLPTR;
    E->Value.DataLen = 0;
    E->Value.NumElements = 0;
    E->ValueCached = FALSE;
    return Idx;
}

STATIC UINT32 NsFindChild(UINT32 Parent, const CHAR *Segment) {
    for (UINT32 I = 0; I < GAml.Count; I++) {
        if (GAml.Entries[I].Parent == Parent &&
            NameSegEq(GAml.Entries[I].Segment, Segment)) {
            return I;
        }
    }
    return (UINT32)-1;
}

STATIC UINT32 NsResolveFrom(UINT32 StartScope,
                            CHAR Segments[][5], INT Count, BOOL IsRoot) {
    UINT32 Scope = IsRoot ? 0 : StartScope;

    INT I = 0;
    while (I < Count && Segments[I][0] == '^') {
        if (GAml.Entries[Scope].Parent != (UINT32)-1) {
            Scope = GAml.Entries[Scope].Parent;
        }
        I++;
    }

    for (; I < Count; I++) {
        UINT32 Child = NsFindChild(Scope, Segments[I]);
        if (Child == (UINT32)-1) return (UINT32)-1;
        Scope = Child;
    }
    return Scope;
}

// ============================================================================
// Forward declarations
// ============================================================================

typedef struct AmlFrame AmlFrame;

STATIC VOID ParseTermList(UINT32 Parent, const UINT8 *Ptr, const UINT8 *End);
STATIC BOOL ExecTermList(UINT32 Scope, const UINT8 **Ptr, const UINT8 *End,
                         struct AmlFrame *Frame, AmlValue *Out);
STATIC BOOL CallMethod(AmlName *Method, AmlValue *Args, INT ArgCount,
                       AmlValue *Out);

typedef struct AmlFrame {
    AmlValue Locals[AML_MAX_LOCALS];
    AmlValue Args[AML_MAX_ARGS];
    BOOL Break;
    BOOL Continue;
    BOOL Returned;
    AmlValue ReturnValue;
} AmlFrame;

// ============================================================================
// OpRegion / Field — access to hardware
// ============================================================================

STATIC UINT64 OpRegionRead(AmlOpRegion *Or, UINT64 ByteOffset, UINT32 Bytes) {
    if (!Or) return 0;
    UINT64 Phys = Or->Offset + ByteOffset;

    switch (Or->SpaceId) {
        case 0: {   // SystemMemory — MMIO
            UINT64 Virt = PhysToVirt(Phys);
            if (!Virt) return 0;
            volatile UINT8 *P = (volatile UINT8 *)Virt;
            UINT64 Val = 0;
            for (UINT32 I = 0; I < Bytes; I++) {
                Val |= ((UINT64)P[I]) << (8 * I);
            }
            return Val;
        }
        case 1:     // SystemIO — ports
            if (Bytes == 1) return Inb((UINT16)Phys);
            if (Bytes == 2) return Inw((UINT16)Phys);
            if (Bytes == 4) return Inl((UINT16)Phys);
            return 0;
        case 2:     // PCI_Config — we need a PCI accessor, we don't support it yet
        case 3:     // EmbeddedController
        case 4:     // SMBus
        case 5:     // SystemCMOS
        case 6:     // PCI_BAR
        case 7:     // IPMI
        case 8:     // GPIO
        case 9:     // GenericSerialBus
        default:
            return 0;
    }
}

STATIC VOID OpRegionWrite(AmlOpRegion *Or, UINT64 ByteOffset, UINT32 Bytes, UINT64 Val) {
    if (!Or) return;
    UINT64 Phys = Or->Offset + ByteOffset;

    switch (Or->SpaceId) {
        case 0: {
            UINT64 Virt = PhysToVirt(Phys);
            if (!Virt) return;
            volatile UINT8 *P = (volatile UINT8 *)Virt;
            for (UINT32 I = 0; I < Bytes; I++) {
                P[I] = (UINT8)(Val >> (8 * I));
            }
            return;
        }
        case 1:
            if (Bytes == 1) Outb((UINT16)Phys, (UINT8)Val);
            else if (Bytes == 2) Outw((UINT16)Phys, (UINT16)Val);
            else if (Bytes == 4) Outl((UINT16)Phys, (UINT32)Val);
            return;
        default:
            return;
    }
}

STATIC BOOL FieldRead(AmlField *F, UINT64 *Out) {
    if (!F || F->RegionIdx >= GAml.OpRegionCount) return FALSE;
    AmlOpRegion *Or = &GAml.OpRegions[F->RegionIdx];

    UINT32 ByteOffset = F->BitOffset / 8;
    UINT32 BitShift = F->BitOffset & 7;
    UINT32 ByteWidth = (F->BitWidth + BitShift + 7) / 8;
    if (ByteWidth == 0) ByteWidth = 1;
    if (ByteWidth > 8) return FALSE;

    UINT64 Val = OpRegionRead(Or, ByteOffset, ByteWidth);

    UINT64 Mask = (F->BitWidth >= 64) ? ~0ULL : ((1ULL << F->BitWidth) - 1);
    Val >>= BitShift;
    Val &= Mask;

    *Out = Val;
    return TRUE;
}

STATIC BOOL FieldWrite(AmlField *F, UINT64 Val) {
    if (!F || F->RegionIdx >= GAml.OpRegionCount) return FALSE;
    AmlOpRegion *Or = &GAml.OpRegions[F->RegionIdx];

    UINT32 ByteOffset = F->BitOffset / 8;
    UINT32 BitShift = F->BitOffset & 7;
    UINT32 ByteWidth = (F->BitWidth + BitShift + 7) / 8;
    if (ByteWidth == 0) ByteWidth = 1;
    if (ByteWidth > 8) return FALSE;

    UINT64 Cur = OpRegionRead(Or, ByteOffset, ByteWidth);

    UINT64 Mask = (F->BitWidth >= 64) ? ~0ULL : ((1ULL << F->BitWidth) - 1);
    UINT64 Masked = (Val & Mask) << BitShift;
    UINT64 ClearMask = ~(Mask << BitShift);

    UINT64 New = (Cur & ClearMask) | Masked;
    OpRegionWrite(Or, ByteOffset, ByteWidth, New);
    return TRUE;
}

// ============================================================================
// Namespace parser
// ============================================================================

STATIC VOID ParseFieldList(UINT32 Parent, UINT32 RegionIdx,
                           UINT32 Flags,
                           const UINT8 **PtrRef, const UINT8 *End) {
    const UINT8 *Ptr = *PtrRef;
    UINT8 AccessType = Flags & 0x0F;
    UINT8 LockRule   = (Flags >> 4) & 0x01;
    UINT8 UpdateRule = (Flags >> 5) & 0x03;

    UINT32 BitOffset = 0;
    while (Ptr < End && GAml.FieldCount < AML_MAX_FIELDS) {
        UINT8 B = *Ptr;

        if (B == 0x00) {
            Ptr++;
            const UINT8 *P2 = Ptr;
            UINT32 Bits = ReadPkgLength(&P2, End);
            Ptr = P2;
            BitOffset += Bits;
            continue;
        }
        if (B == 0x01) {
            Ptr++;
            if (Ptr + 2 <= End) Ptr += 2;
            continue;
        }
        if (B == 0x02) {
            Ptr++;
            if (Ptr < End && (*Ptr == '\\' || *Ptr == '^' ||
                *Ptr == '_' || (*Ptr >= 'A' && *Ptr <= 'Z') ||
                *Ptr == 0x2E || *Ptr == 0x2F)) {
                CHAR SkipSeg[8][5]; BOOL SkipRoot = FALSE;
                ReadNameString(&Ptr, End, SkipSeg, 8, &SkipRoot);
            } else {
                // BufferData — skip through SkipDataObject (announced below)
                // For simplicity: we are looking for the end of buffer by PkgLength
                if (Ptr < End) {
                    const UINT8 *P2 = Ptr + 1;
                    UINT32 PkgLen = ReadPkgLength(&P2, End);
                    Ptr = P2 + PkgLen;
                    if (Ptr > End) Ptr = End;
                }
            }
            continue;
        }
        if (B == 0x03) {
            Ptr++;
            if (Ptr + 3 <= End) Ptr += 3;
            continue;
        }

        // NamedField: NameSeg PkgLength
        if (Ptr + 4 > End) break;
        UINT8 C = Ptr[0];
        BOOL IsLeadChar = (C == '_' || (C >= 'A' && C <= 'Z'));
        if (!IsLeadChar) { Ptr++; continue; }

        CHAR FieldName[5];
        CopyNameSeg4(Ptr, FieldName);
        Ptr += 4;

        const UINT8 *P2 = Ptr;
        UINT32 Bits = ReadPkgLength(&P2, End);
        Ptr = P2;

        AmlField *F = &GAml.Fields[GAml.FieldCount++];
        StrnCpy(F->FieldName, FieldName, 4);
        F->FieldName[4] = '\0';
        F->RegionIdx = RegionIdx;
        F->BitOffset = BitOffset;
        F->BitWidth = Bits;
        F->AccessType = AccessType;
        F->LockRule = LockRule;
        F->UpdateRule = UpdateRule;

        NsAdd(Parent, FieldName, AML_ENTRY_FIELD,
              (const UINT8 *)F, sizeof(AmlField));

        BitOffset += Bits;
    }

    *PtrRef = Ptr;
}

STATIC VOID ParseTermList(UINT32 Parent, const UINT8 *Ptr, const UINT8 *End) {
    while (Ptr < End) {
        UINT8 Op = *Ptr++;

        switch (Op) {
            case AML_SCOPE_OP: {
                UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                const UINT8 *ScopeEnd = Ptr + PkgLen;
                if (ScopeEnd > End) ScopeEnd = End;

                CHAR Segs[8][5];
                BOOL IsRoot = FALSE;
                INT N = ReadNameString(&Ptr, ScopeEnd, Segs, 8, &IsRoot);

                UINT32 NewParent = Parent;
                if (N > 0 && !(N == 1 && Segs[0][0] == '^')) {
                    UINT32 Idx = NsResolveFrom(Parent, Segs, N, IsRoot);
                    if (Idx == (UINT32)-1) {
                        UINT32 Cur = Parent;
                        for (INT K = 0; K < N; K++) {
                            if (Segs[K][0] == '^') continue;
                            UINT32 Next = NsFindChild(Cur, Segs[K]);
                            if (Next == (UINT32)-1) {
                                Next = NsAdd(Cur, Segs[K], AML_ENTRY_SCOPE, NULLPTR, 0);
                            }
                            Cur = Next;
                        }
                        NewParent = Cur;
                    } else {
                        NewParent = Idx;
                    }
                }

                ParseTermList(NewParent, Ptr, ScopeEnd);
                Ptr = ScopeEnd;
                break;
            }

            case AML_EXT_OP_PREFIX: {
                if (Ptr >= End) return;
                UINT8 Ext = *Ptr++;
                if (Ext == AML_EXT_DEVICE_OP ||
                    Ext == AML_EXT_THERMALZONE_OP ||
                    Ext == AML_EXT_PROCESSOR_OP ||
                    Ext == AML_EXT_POWERRES_OP) {

                    UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                    const UINT8 *SubEnd = Ptr + PkgLen;
                    if (SubEnd > End) SubEnd = End;

                    CHAR Segs[8][5];
                    BOOL IsRoot = FALSE;
                    INT N = ReadNameString(&Ptr, SubEnd, Segs, 8, &IsRoot);

                    AmlEntryKind Kind;
                    if (Ext == AML_EXT_DEVICE_OP) Kind = AML_ENTRY_DEVICE;
                    else if (Ext == AML_EXT_THERMALZONE_OP) Kind = AML_ENTRY_THERMALZONE;
                    else if (Ext == AML_EXT_PROCESSOR_OP) Kind = AML_ENTRY_PROCESSOR;
                    else Kind = AML_ENTRY_POWERRES;

                    UINT32 Cur = Parent;
                    for (INT K = 0; K < N; K++) {
                        if (Segs[K][0] == '^') continue;
                        UINT32 Next = NsFindChild(Cur, Segs[K]);
                        if (Next == (UINT32)-1) {
                            Next = NsAdd(Cur, Segs[K], Kind, NULLPTR, 0);
                        }
                        Cur = Next;
                    }

                    if (Ext == AML_EXT_PROCESSOR_OP) {
                        if (Ptr + 6 <= SubEnd) Ptr += 6;
                    }

                    ParseTermList(Cur, Ptr, SubEnd);
                    Ptr = SubEnd;
                }
                else if (Ext == AML_EXT_OPREGION_OP) {
                    UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                    const UINT8 *OrEnd = Ptr + PkgLen;
                    if (OrEnd > End) OrEnd = End;

                    CHAR Segs[8][5];
                    BOOL IsRoot = FALSE;
                    INT N = ReadNameString(&Ptr, OrEnd, Segs, 8, &IsRoot);

                    if (N > 0 && GAml.OpRegionCount < AML_MAX_OPREGIONS) {
                        AmlOpRegion *Or = &GAml.OpRegions[GAml.OpRegionCount];
                        StrnCpy(Or->RegionName, Segs[N - 1], 4);
                        Or->RegionName[4] = '\0';
                        Or->SpaceId = 0;
                        Or->Offset = 0;
                        Or->Length = 0;

                        if (Ptr < OrEnd) Or->SpaceId = *Ptr++;

                        // Offset — TermArg
                        if (Ptr < OrEnd) {
                            // Quick way: let's look at the prefix
                            if (*Ptr == AML_ZERO_OP) { Or->Offset = 0; Ptr++; }
                            else if (*Ptr == AML_ONE_OP) { Or->Offset = 1; Ptr++; }
                            else if (*Ptr == AML_BYTE_PREFIX && Ptr + 2 <= OrEnd) {
                                Or->Offset = Ptr[1]; Ptr += 2;
                            }
                            else if (*Ptr == AML_WORD_PREFIX && Ptr + 3 <= OrEnd) {
                                Or->Offset = Ptr[1] | (Ptr[2] << 8); Ptr += 3;
                            }
                            else if (*Ptr == AML_DWORD_PREFIX && Ptr + 5 <= OrEnd) {
                                Or->Offset = Ptr[1] | (Ptr[2] << 8) |
                                             (Ptr[3] << 16) | ((UINT32)Ptr[4] << 24);
                                Ptr += 5;
                            }
                            else if (*Ptr == AML_QWORD_PREFIX && Ptr + 9 <= OrEnd) {
                                UINT64 Q = 0;
                                for (INT I = 0; I < 8; I++) Q |= ((UINT64)Ptr[1+I]) << (8*I);
                                Or->Offset = Q; Ptr += 9;
                            }
                            else {
                                // Deferred — we will not perform, we will skip the term
                                AmlValue tmp = AMLV_MAKE(AMLV_UNINITIALIZED);
                                AmlFrame Sub = {0};
                                if (!ExecTermList(Parent, &Ptr, OrEnd, &Sub, &tmp)) {
                                    Ptr = OrEnd;
                                } else {
                                    Or->Offset = tmp.Integer;
                                }
                            }
                        }

                        if (Ptr < OrEnd) {
                            if (*Ptr == AML_ZERO_OP) { Or->Length = 0; Ptr++; }
                            else if (*Ptr == AML_ONE_OP) { Or->Length = 1; Ptr++; }
                            else if (*Ptr == AML_BYTE_PREFIX && Ptr + 2 <= OrEnd) {
                                Or->Length = Ptr[1]; Ptr += 2;
                            }
                            else if (*Ptr == AML_WORD_PREFIX && Ptr + 3 <= OrEnd) {
                                Or->Length = Ptr[1] | (Ptr[2] << 8); Ptr += 3;
                            }
                            else if (*Ptr == AML_DWORD_PREFIX && Ptr + 5 <= OrEnd) {
                                Or->Length = Ptr[1] | (Ptr[2] << 8) |
                                             (Ptr[3] << 16) | ((UINT32)Ptr[4] << 24);
                                Ptr += 5;
                            }
                            else if (*Ptr == AML_QWORD_PREFIX && Ptr + 9 <= OrEnd) {
                                UINT64 Q = 0;
                                for (INT I = 0; I < 8; I++) Q |= ((UINT64)Ptr[1+I]) << (8*I);
                                Or->Length = Q; Ptr += 9;
                            }
                        }

                        // Registering in the namespace
                        UINT32 ParentScope = IsRoot ? 0 : Parent;
                        UINT32 Cur = ParentScope;
                        for (INT K = 0; K < N - 1; K++) {
                            if (Segs[K][0] == '^') continue;
                            UINT32 Next = NsFindChild(Cur, Segs[K]);
                            if (Next == (UINT32)-1) {
                                Next = NsAdd(Cur, Segs[K], AML_ENTRY_SCOPE, NULLPTR, 0);
                            }
                            Cur = Next;
                        }
                        NsAdd(Cur, Segs[N - 1], AML_ENTRY_OPREGION,
                              (const UINT8 *)Or, sizeof(AmlOpRegion));
                        GAml.OpRegionCount++;
                    }
                    Ptr = OrEnd;
                }
                else if (Ext == AML_EXT_FIELD_OP) {
                    UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                    const UINT8 *FldEnd = Ptr + PkgLen;
                    if (FldEnd > End) FldEnd = End;

                    CHAR Segs[8][5];
                    BOOL IsRoot = FALSE;
                    INT N = ReadNameString(&Ptr, FldEnd, Segs, 8, &IsRoot);

                    if (N > 0) {
                        // Find an OpRegion by name
                        UINT32 RegionIdx = (UINT32)-1;
                        for (UINT32 R = 0; R < GAml.OpRegionCount; R++) {
                            if (NameSegEq(GAml.OpRegions[R].RegionName, Segs[N-1])) {
                                RegionIdx = R;
                                break;
                            }
                        }
                        if (RegionIdx == (UINT32)-1 && GAml.OpRegionCount > 0) {
                            RegionIdx = GAml.OpRegionCount - 1;
                        }

                        // Flags: 3 bytes
                        UINT32 Flags = 0;
                        if (Ptr + 3 <= FldEnd) {
                            Flags = Ptr[0] | (Ptr[1] << 8) | (Ptr[2] << 16);
                            Ptr += 3;
                        }

                        // Define the namespace parent for the fields
                        UINT32 FieldParent = Parent;
                        UINT32 ParentScope = IsRoot ? 0 : Parent;
                        UINT32 Cur = ParentScope;
                        for (INT K = 0; K < N - 1; K++) {
                            if (Segs[K][0] == '^') continue;
                            UINT32 Next = NsFindChild(Cur, Segs[K]);
                            if (Next == (UINT32)-1) {
                                Next = NsAdd(Cur, Segs[K], AML_ENTRY_SCOPE, NULLPTR, 0);
                            }
                            Cur = Next;
                        }
                        FieldParent = Cur;

                        ParseFieldList(FieldParent, RegionIdx, Flags, &Ptr, FldEnd);
                    }
                    Ptr = FldEnd;
                }
                else if (Ext == AML_EXT_INDEXFIELD_OP ||
                         Ext == AML_EXT_BANKFIELD_OP ||
                         Ext == AML_EXT_MUTEX_OP ||
                         Ext == AML_EXT_EVENT_OP) {
                    UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                    Ptr += PkgLen;
                    if (Ptr > End) Ptr = End;
                }
                else {
                    UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                    Ptr += PkgLen;
                    if (Ptr > End) Ptr = End;
                }
                break;
            }

            case AML_NAME_OP: {
                CHAR Segs[8][5];
                BOOL IsRoot = FALSE;
                INT N = ReadNameString(&Ptr, End, Segs, 8, &IsRoot);
                if (N <= 0) break;

                UINT32 ParentScope = IsRoot ? 0 : Parent;
                UINT32 Cur = ParentScope;
                for (INT K = 0; K < N - 1; K++) {
                    if (Segs[K][0] == '^') continue;
                    UINT32 Next = NsFindChild(Cur, Segs[K]);
                    if (Next == (UINT32)-1) {
                        Next = NsAdd(Cur, Segs[K], AML_ENTRY_SCOPE, NULLPTR, 0);
                    }
                    Cur = Next;
                }

                const UINT8 *ValueStart = Ptr;

                UINT32 EstLen = 0;
                if (Ptr < End) {
                    UINT8 V = *Ptr;
                    if (V == AML_ZERO_OP || V == AML_ONE_OP || V == AML_ONES_OP) {
                        EstLen = 1;
                    } else if (V == AML_BYTE_PREFIX) {
                        EstLen = 2;
                    } else if (V == AML_WORD_PREFIX) {
                        EstLen = 3;
                    } else if (V == AML_DWORD_PREFIX) {
                        EstLen = 5;
                    } else if (V == AML_QWORD_PREFIX) {
                        EstLen = 9;
                    } else if (V == AML_STRING_PREFIX) {
                        const UINT8 *S = Ptr + 1;
                        while (S < End && *S) S++;
                        EstLen = (UINT32)(S - Ptr) + 1;
                    } else if (V == AML_PACKAGE_OP || V == AML_VAR_PACKAGE_OP) {
                        const UINT8 *P2 = Ptr + 1;
                        UINT32 PkgLen = ReadPkgLength(&P2, End);
                        EstLen = (UINT32)(P2 - Ptr) + PkgLen;
                    } else if (V == AML_BUFFER_OP) {
                        const UINT8 *P2 = Ptr + 1;
                        UINT32 PkgLen = ReadPkgLength(&P2, End);
                        EstLen = (UINT32)(P2 - Ptr) + PkgLen;
                    } else {
                        EstLen = 1;
                    }
                }
                if (ValueStart + EstLen > End) EstLen = (UINT32)(End - ValueStart);

                NsAdd(Cur, Segs[N - 1], AML_ENTRY_NAME, ValueStart, EstLen);
                Ptr = ValueStart + EstLen;
                break;
            }

            case AML_METHOD_OP: {
                UINT32 PkgLen = ReadPkgLength(&Ptr, End);
                const UINT8 *MethStart = Ptr;
                const UINT8 *MethEnd = MethStart + PkgLen;
                if (MethEnd > End) MethEnd = End;

                CHAR Segs[8][5];
                BOOL IsRoot = FALSE;
                INT N = ReadNameString(&Ptr, MethEnd, Segs, 8, &IsRoot);
                if (N <= 0) { Ptr = MethEnd; break; }

                UINT32 ParentScope = IsRoot ? 0 : Parent;
                UINT32 Cur = ParentScope;
                for (INT K = 0; K < N - 1; K++) {
                    if (Segs[K][0] == '^') continue;
                    UINT32 Next = NsFindChild(Cur, Segs[K]);
                    if (Next == (UINT32)-1) {
                        Next = NsAdd(Cur, Segs[K], AML_ENTRY_SCOPE, NULLPTR, 0);
                    }
                    Cur = Next;
                }

                if (Ptr < MethEnd) Ptr++; // Flags

                const UINT8 *Body = Ptr;
                UINT32 BodyLen = (UINT32)(MethEnd - Body);

                NsAdd(Cur, Segs[N - 1], AML_ENTRY_METHOD, Body, BodyLen);
                Ptr = MethEnd;
                break;
            }

            case AML_ALIAS_OP: {
                CHAR Segs[8][5];
                BOOL IsRoot = FALSE;
                ReadNameString(&Ptr, End, Segs, 8, &IsRoot);
                ReadNameString(&Ptr, End, Segs, 8, &IsRoot);
                break;
            }

            case AML_EXTERNAL_OP: {
                CHAR Segs[8][5];
                BOOL IsRoot = FALSE;
                ReadNameString(&Ptr, End, Segs, 8, &IsRoot);
                if (Ptr < End) Ptr += 2;
                break;
            }

            default:
                break;
        }
    }
}

// ============================================================================
// Skip helpers
// ============================================================================

STATIC const UINT8 *SkipDataObject(const UINT8 *P, const UINT8 *End) {
    if (P >= End) return NULLPTR;
    UINT8 V = *P;
    switch (V) {
        case AML_ZERO_OP:
        case AML_ONE_OP:
        case AML_ONES_OP:
            return P + 1;
        case AML_BYTE_PREFIX:  return (P + 2 <= End) ? P + 2 : NULLPTR;
        case AML_WORD_PREFIX:  return (P + 3 <= End) ? P + 3 : NULLPTR;
        case AML_DWORD_PREFIX: return (P + 5 <= End) ? P + 5 : NULLPTR;
        case AML_QWORD_PREFIX: return (P + 9 <= End) ? P + 9 : NULLPTR;
        case AML_STRING_PREFIX: {
            const UINT8 *S = P + 1;
            while (S < End && *S) S++;
            if (S >= End) return NULLPTR;
            return S + 1;
        }
        case AML_BUFFER_OP:
        case AML_PACKAGE_OP:
        case AML_VAR_PACKAGE_OP: {
            const UINT8 *P2 = P + 1;
            UINT32 PkgLen = ReadPkgLength(&P2, End);
            const UINT8 *PkgEnd = P2 + PkgLen;
            if (PkgEnd > End) return NULLPTR;
            return PkgEnd;
        }
        default:
            return NULLPTR;
    }
}

STATIC const UINT8 *SkipTermArg(const UINT8 *P, const UINT8 *End) {
    if (P >= End) return NULLPTR;
    UINT8 Op = *P;
    if (Op == AML_ZERO_OP || Op == AML_ONE_OP || Op == AML_ONES_OP ||
        Op == AML_BYTE_PREFIX || Op == AML_WORD_PREFIX ||
        Op == AML_DWORD_PREFIX || Op == AML_QWORD_PREFIX ||
        Op == AML_STRING_PREFIX ||
        Op == AML_BUFFER_OP || Op == AML_PACKAGE_OP || Op == AML_VAR_PACKAGE_OP) {
        return SkipDataObject(P, End);
    }
    if (Op == '\\' || Op == '^' || Op == '_' ||
        (Op >= 'A' && Op <= 'Z') || Op == 0x2E || Op == 0x2F) {
        CHAR Segs[8][5];
        BOOL IsRoot = FALSE;
        const UINT8 *P2 = P;
        INT N = ReadNameString(&P2, End, Segs, 8, &IsRoot);
        if (N <= 0) return NULLPTR;
        return P2;
    }
    if (Op >= AML_LOCAL0_OP && Op <= AML_LOCAL7_OP) return P + 1;
    if (Op >= AML_ARG0_OP && Op <= AML_ARG6_OP) return P + 1;
    if (Op == AML_EXT_OP_PREFIX) {
        if (P + 1 >= End) return NULLPTR;
        const UINT8 *P2 = P + 2;
        UINT32 PkgLen = ReadPkgLength(&P2, End);
        const UINT8 *PkgEnd = P2 + PkgLen;
        if (PkgEnd > End) return NULLPTR;
        return PkgEnd;
    }
    // Fallback: we believe that this is a package with PkgLength
    const UINT8 *P2 = P + 1;
    UINT32 PkgLen = ReadPkgLength(&P2, End);
    if (PkgLen == 0) return P + 1;
    const UINT8 *PkgEnd = P2 + PkgLen;
    if (PkgEnd > End) return NULLPTR;
    return PkgEnd;
}

// ============================================================================
// Builtin virtual methods
// ============================================================================

STATIC BOOL AmlBuiltinCall(const CHAR *Name, AmlValue *Args, INT ArgCount,
                           AmlValue *Out) {
    if (StrCaseCmp(Name, "_OSI") == 0) {
        STATIC const CHAR *Known[] = {
            "Windows 2000", "Windows 2001", "Windows 2001 SP1",
            "Windows 2001 SP2", "Windows 2001.1", "Windows 2006",
            "Windows 2006 SP1", "Windows 2006.1", "Windows 2009",
            "Windows 2012", "Windows 2013", "Windows 2015",
            "Windows 2016", "Windows 2017", "Windows 2017.2",
            "Windows 2018", "Windows 2018.2", "Windows 2019",
            "Windows 2020", "Windows 2021", "Windows 2022",
            "Linux", "Darwin", "FreeBSD", NULLPTR
        };
        Out->Kind = AMLV_INTEGER;
        Out->Integer = 0;
        if (ArgCount >= 1 && Args[0].Kind == AMLV_STRING && Args[0].Data) {
            const CHAR *Os = (const CHAR *)Args[0].Data;
            for (INT I = 0; Known[I]; I++) {
                if (StrCaseCmp(Os, Known[I]) == 0) { Out->Integer = 1; break; }
            }
        }
        return TRUE;
    }
    if (StrCaseCmp(Name, "_OS") == 0) {
        STATIC const CHAR OsName[] = "Linux";
        Out->Kind = AMLV_STRING;
        Out->Data = (const UINT8 *)OsName;
        Out->DataLen = sizeof(OsName) - 1;
        Out->NumElements = 0;
        Out->Integer = (UINT64)(UINTPTR)OsName;
        return TRUE;
    }
    if (StrCaseCmp(Name, "_REV") == 0) {
        Out->Kind = AMLV_INTEGER;
        Out->Integer = 2;  // ACPI 2.0
        return TRUE;
    }
    return FALSE;
}

// ============================================================================
// Interpreter
// ============================================================================

STATIC AmlValue EvalNameRef(UINT32 Scope, CHAR Segs[][5], INT Count, BOOL IsRoot) {
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    UINT32 Idx = NsResolveFrom(Scope, Segs, Count, IsRoot);
    if (Idx == (UINT32)-1) return V;

    AmlName *E = &GAml.Entries[Idx];

    if (E->Kind == AML_ENTRY_NAME) {
        if (E->ValueCached) return E->Value;

        const UINT8 *P = E->Code;
        const UINT8 *E2 = P + E->CodeLen;
        AmlFrame Sub = {0};
        AmlValue Out = AMLV_MAKE(AMLV_INTEGER);
        if (ExecTermList(Idx, &P, E2, &Sub, &Out)) {
            E->Value = Out;
            E->ValueCached = TRUE;
        }
        return E->Value;
    }

    if (E->Kind == AML_ENTRY_METHOD) {
        V.Kind = AMLV_NAMEREF;
        V.NameRef = E;
        return V;
    }

    if (E->Kind == AML_ENTRY_FIELD) {
        AmlField *F = (AmlField *)E->Code;
        UINT64 Val = 0;
        if (FieldRead(F, &Val)) {
            V.Kind = AMLV_INTEGER;
            V.Integer = Val;
        }
        return V;
    }

    return V;
}

STATIC BOOL CallMethod(AmlName *Method, AmlValue *Args, INT ArgCount,
                       AmlValue *Out) {
    if (!Method || Method->Kind != AML_ENTRY_METHOD) return FALSE;
    if (ArgCount > AML_MAX_ARGS) ArgCount = AML_MAX_ARGS;

    UINT32 MethodIdx = (UINT32)(Method - GAml.Entries);

    AmlFrame Frame = {0};
    for (INT I = 0; I < ArgCount; I++) {
        Frame.Args[I] = Args[I];
    }
    for (INT I = 0; I < AML_MAX_LOCALS; I++) {
        Frame.Locals[I].Kind = AMLV_UNINITIALIZED;
    }

    const UINT8 *P = Method->Code;
    const UINT8 *E = P + Method->CodeLen;
    AmlValue V = AMLV_MAKE(AMLV_INTEGER);

    GInstructionCounter = 0;
    ExecTermList(MethodIdx, &P, E, &Frame, &V);

    if (Frame.Returned) {
        *Out = Frame.ReturnValue;
    } else {
        *Out = V;
    }
    return TRUE;
}

STATIC const UINT8 *WriteTarget(UINT32 Scope, const UINT8 *P, const UINT8 *End,
                                AmlFrame *Frame, AmlValue Val) {
    if (P >= End) return P;
    UINT8 D = *P;
    if (D >= AML_LOCAL0_OP && D <= AML_LOCAL7_OP && Frame) {
        Frame->Locals[D - AML_LOCAL0_OP] = Val;
        return P + 1;
    }
    if (D >= AML_ARG0_OP && D <= AML_ARG6_OP && Frame) {
        Frame->Args[D - AML_ARG0_OP] = Val;
        return P + 1;
    }
    if (D == 0x00) return P + 1;

    const UINT8 *P3 = P;
    CHAR Segs[8][5];
    BOOL IsRoot = FALSE;
    INT N = ReadNameString(&P3, End, Segs, 8, &IsRoot);
    if (N > 0) {
        UINT32 Idx = NsResolveFrom(Scope, Segs, N, IsRoot);
        if (Idx != (UINT32)-1) {
            AmlName *E = &GAml.Entries[Idx];
            if (E->Kind == AML_ENTRY_FIELD) {
                AmlField *F = (AmlField *)E->Code;
                FieldWrite(F, Val.Integer);
                return P3;
            }
            E->Value = Val;
            E->ValueCached = TRUE;
            return P3;
        }
    }
    return P;
}

STATIC BOOL ExecTermList(UINT32 Scope, const UINT8 **Ptr, const UINT8 *End,
                         AmlFrame *Frame, AmlValue *Out) {
    while (*Ptr < End) {
        if (Frame && Frame->Returned) return TRUE;
        if (Frame && Frame->Break)    return TRUE;
        if (Frame && Frame->Continue) return TRUE;

        if (++GInstructionCounter > AML_MAX_INSTRUCTIONS) {
            *Ptr = End;
            return FALSE;
        }

        const UINT8 *P = *Ptr;
        UINT8 Op = *P++;

        switch (Op) {
            case AML_ZERO_OP:
                Out->Kind = AMLV_INTEGER; Out->Integer = 0; break;
            case AML_ONE_OP:
                Out->Kind = AMLV_INTEGER; Out->Integer = 1; break;
            case AML_ONES_OP:
                Out->Kind = AMLV_INTEGER; Out->Integer = 0xFFFFFFFFFFFFFFFFULL; break;

            case AML_BYTE_PREFIX:
                if (P >= End) { *Ptr = End; return FALSE; }
                Out->Kind = AMLV_INTEGER; Out->Integer = *P++;
                break;
            case AML_WORD_PREFIX:
                if (P + 2 > End) { *Ptr = End; return FALSE; }
                Out->Kind = AMLV_INTEGER;
                Out->Integer = P[0] | (P[1] << 8);
                P += 2;
                break;
            case AML_DWORD_PREFIX:
                if (P + 4 > End) { *Ptr = End; return FALSE; }
                Out->Kind = AMLV_INTEGER;
                Out->Integer = P[0] | (P[1] << 8) | (P[2] << 16) | ((UINT64)P[3] << 24);
                P += 4;
                break;
            case AML_QWORD_PREFIX: {
                if (P + 8 > End) { *Ptr = End; return FALSE; }
                Out->Kind = AMLV_INTEGER;
                UINT64 Q = 0;
                for (INT I = 0; I < 8; I++) Q |= ((UINT64)P[I]) << (8 * I);
                Out->Integer = Q;
                P += 8;
                break;
            }

            case AML_STRING_PREFIX: {
                Out->Kind = AMLV_STRING;
                Out->Integer = (UINT64)(UINTPTR)P;
                Out->Data = P;
                const UINT8 *S = P;
                while (S < End && *S) S++;
                Out->DataLen = (UINT32)(S - P);
                Out->NumElements = 0;
                P = S;
                if (P < End) P++;
                break;
            }

            case AML_BUFFER_OP: {
                const UINT8 *P2 = P;
                UINT32 PkgLen = ReadPkgLength(&P2, End);
                const UINT8 *PkgEnd = P2 + PkgLen;
                if (PkgEnd > End) { *Ptr = End; return FALSE; }

                // TermArg BufferSize — skip
                const UINT8 *SizeEnd = SkipTermArg(P2, PkgEnd);
                if (!SizeEnd) SizeEnd = P2;

                Out->Kind = AMLV_BUFFER;
                Out->Data = SizeEnd;                    // the first byte of data
                Out->DataLen = (UINT32)(PkgEnd - SizeEnd);
                Out->NumElements = Out->DataLen;
                Out->Integer = 0;
                P = PkgEnd;
                break;
            }

            case AML_PACKAGE_OP:
            case AML_VAR_PACKAGE_OP: {
                const UINT8 *P2 = P;
                UINT32 PkgLen = ReadPkgLength(&P2, End);
                const UINT8 *PkgEnd = P2 + PkgLen;
                if (PkgEnd > End) { *Ptr = End; return FALSE; }

                UINT32 NumElem = 0;
                if (Op == AML_PACKAGE_OP) {
                    if (P2 < PkgEnd) {
                        NumElem = *P2++;
                    }
                } else {
                    // VarPackage: NumElements — TermArg
                    AmlValue Nv = AMLV_MAKE(AMLV_UNINITIALIZED);
                    if (ExecTermList(Scope, &P2, PkgEnd, Frame, &Nv)) {
                        NumElem = (UINT32)Nv.Integer;
                    }
                }

                Out->Kind = AMLV_PACKAGE;
                Out->Data = P2;
                Out->DataLen = (UINT32)(PkgEnd - P2);
                Out->NumElements = NumElem;
                Out->Integer = 0;
                P = PkgEnd;
                break;
            }

            case AML_LOCAL0_OP: case AML_LOCAL1_OP: case AML_LOCAL2_OP: case AML_LOCAL3_OP:
            case AML_LOCAL4_OP: case AML_LOCAL5_OP: case AML_LOCAL6_OP: case AML_LOCAL7_OP:
                if (Frame) {
                    INT Idx = Op - AML_LOCAL0_OP;
                    *Out = Frame->Locals[Idx];
                } else {
                    Out->Kind = AMLV_UNINITIALIZED;
                }
                break;

            case AML_ARG0_OP: case AML_ARG1_OP: case AML_ARG2_OP: case AML_ARG3_OP:
            case AML_ARG4_OP: case AML_ARG5_OP: case AML_ARG6_OP:
                if (Frame) {
                    INT Idx = Op - AML_ARG0_OP;
                    *Out = Frame->Args[Idx];
                } else {
                    Out->Kind = AMLV_UNINITIALIZED;
                }
                break;

            case AML_STORE_OP: {
                AmlValue Src = AMLV_MAKE(AMLV_UNINITIALIZED);
                const UINT8 *P2 = P;
                if (!ExecTermList(Scope, &P2, End, Frame, &Src)) {
                    *Ptr = End;
                    return FALSE;
                }
                const UINT8 *P3 = WriteTarget(Scope, P2, End, Frame, Src);
                if (P3 == P2 && P2 < End) P3++;    // anti-freeze protection
                P = P3;
                *Out = Src;
                break;
            }

            case AML_ADD_OP: case AML_SUBTRACT_OP:
            case AML_MULTIPLY_OP: case AML_AND_OP: case AML_OR_OP:
            case AML_XOR_OP: case AML_SHIFT_LEFT_OP: case AML_SHIFT_RIGHT_OP:
            case AML_MOD_OP:
            case AML_LEQUAL_OP: case AML_LGREATER_OP: case AML_LLESS_OP:
            case AML_LAND_OP: case AML_LOR_OP: {
                AmlValue A = {0}, B = {0};
                const UINT8 *P2 = P;
                if (!ExecTermList(Scope, &P2, End, Frame, &A)) { *Ptr = End; return FALSE; }
                if (!ExecTermList(Scope, &P2, End, Frame, &B)) { *Ptr = End; return FALSE; }

                UINT64 a = A.Integer, b = B.Integer;
                UINT64 r = 0;
                switch (Op) {
                    case AML_ADD_OP: r = a + b; break;
                    case AML_SUBTRACT_OP: r = a - b; break;
                    case AML_MULTIPLY_OP: r = a * b; break;
                    case AML_AND_OP: r = a & b; break;
                    case AML_OR_OP:  r = a | b; break;
                    case AML_XOR_OP: r = a ^ b; break;
                    case AML_SHIFT_LEFT_OP:  r = a << (b & 63); break;
                    case AML_SHIFT_RIGHT_OP: r = a >> (b & 63); break;
                    case AML_MOD_OP: r = (b == 0) ? 0 : (a % b); break;
                    case AML_LEQUAL_OP:   r = (a == b) ? 1 : 0; break;
                    case AML_LGREATER_OP: r = (a > b)  ? 1 : 0; break;
                    case AML_LLESS_OP:    r = (a < b)  ? 1 : 0; break;
                    case AML_LAND_OP:     r = (a && b) ? 1 : 0; break;
                    case AML_LOR_OP:      r = (a || b) ? 1 : 0; break;
                }
                Out->Kind = AMLV_INTEGER;
                Out->Integer = r;
                Out->Data = NULLPTR;
                Out->DataLen = 0;
                Out->NumElements = 0;

                const UINT8 *P3 = WriteTarget(Scope, P2, End, Frame, *Out);
                if (P3 == P2 && P2 < End) P3++;
                P = P3;
                break;
            }

            case AML_DIVIDE_OP: {
                AmlValue A = {0}, B = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &A);
                ExecTermList(Scope, &P2, End, Frame, &B);

                UINT64 q = (B.Integer == 0) ? 0 : (A.Integer / B.Integer);
                UINT64 r = (B.Integer == 0) ? 0 : (A.Integer % B.Integer);

                AmlValue Rem = AMLV_MAKE(AMLV_INTEGER); Rem.Integer = r;
                AmlValue Quo = AMLV_MAKE(AMLV_INTEGER); Quo.Integer = q;

                const UINT8 *P3 = WriteTarget(Scope, P2, End, Frame, Rem);
                if (P3 == P2 && P2 < End) P3++;
                const UINT8 *P4 = WriteTarget(Scope, P3, End, Frame, Quo);
                if (P4 == P3 && P3 < End) P4++;

                Out->Kind = AMLV_INTEGER;
                Out->Integer = q;
                P = P4;
                break;
            }

            case AML_NOT_OP: case AML_LNOT_OP: {
                AmlValue A = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &A);
                Out->Kind = AMLV_INTEGER;
                Out->Integer = (Op == AML_LNOT_OP) ? (A.Integer ? 0 : 1) : ~A.Integer;
                const UINT8 *P3 = WriteTarget(Scope, P2, End, Frame, *Out);
                if (P3 == P2 && P2 < End) P3++;
                P = P3;
                break;
            }

            case AML_INCREMENT_OP: case AML_DECREMENT_OP: {
                AmlValue T = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &T);
                if (Op == AML_INCREMENT_OP) T.Integer++;
                else T.Integer--;
                *Out = T;
                P = P2;
                break;
            }

            case AML_RETURN_OP: {
                AmlValue V = AMLV_MAKE(AMLV_INTEGER);
                const UINT8 *P2 = P;
                if (P2 < End) {
                    ExecTermList(Scope, &P2, End, Frame, &V);
                }
                if (Frame) {
                    Frame->Returned = TRUE;
                    Frame->ReturnValue = V;
                }
                *Out = V;
                *Ptr = P2;
                return TRUE;
            }

            case AML_IF_OP: {
                UINT32 PkgLen = ReadPkgLength(&P, End);
                const UINT8 *IfEnd = P + PkgLen;
                if (IfEnd > End) IfEnd = End;

                AmlValue Pred = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &Pred);
                if (Pred.Integer) {
                    const UINT8 *Body = P2;
                    ExecTermList(Scope, &Body, IfEnd, Frame, Out);
                }
                P = IfEnd;

                if (P < End && *P == AML_ELSE_OP) {
                    P++;
                    UINT32 E2Len = ReadPkgLength(&P, End);
                    const UINT8 *ElseEnd = P + E2Len;
                    if (ElseEnd > End) ElseEnd = End;

                    if (!Pred.Integer) {
                        const UINT8 *Body = P;
                        ExecTermList(Scope, &Body, ElseEnd, Frame, Out);
                    }
                    P = ElseEnd;
                }
                break;
            }

            case AML_WHILE_OP: {
                UINT32 PkgLen = ReadPkgLength(&P, End);
                const UINT8 *LoopEnd = P + PkgLen;
                if (LoopEnd > End) LoopEnd = End;

                const UINT8 *PredStart = P;
                INT Safety = 0;
                while (++Safety < 100000) {
                    const UINT8 *P2 = PredStart;
                    AmlValue Pred = {0};
                    ExecTermList(Scope, &P2, LoopEnd, Frame, &Pred);
                    if (!Pred.Integer) break;

                    const UINT8 *Body = P2;
                    ExecTermList(Scope, &Body, LoopEnd, Frame, Out);

                    if (Frame && Frame->Returned) break;
                    if (Frame && Frame->Break) { Frame->Break = FALSE; break; }
                    if (Frame && Frame->Continue) { Frame->Continue = FALSE; continue; }
                }
                P = LoopEnd;
                break;
            }

            case AML_BREAK_OP:
                if (Frame) Frame->Break = TRUE;
                break;

            case AML_CONTINUE_OP:
                if (Frame) Frame->Continue = TRUE;
                break;

            case AML_NOOP_OP:
            case AML_BREAK_POINT_OP:
                break;

            case AML_TO_INTEGER_OP: {
                AmlValue A = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &A);
                Out->Kind = AMLV_INTEGER;
                Out->Integer = A.Integer;
                const UINT8 *P3 = WriteTarget(Scope, P2, End, Frame, *Out);
                if (P3 == P2 && P2 < End) P3++;
                P = P3;
                break;
            }

            case AML_SIZE_OF_OP: {
                AmlValue A = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &A);
                Out->Kind = AMLV_INTEGER;
                if (A.Kind == AMLV_STRING) Out->Integer = A.DataLen;
                else if (A.Kind == AMLV_BUFFER) Out->Integer = A.NumElements;
                else if (A.Kind == AMLV_PACKAGE) Out->Integer = A.NumElements;
                else Out->Integer = 0;
                P = P2;
                break;
            }

            case AML_OBJECT_TYPE_OP: {
                AmlValue A = {0};
                const UINT8 *P2 = P;
                ExecTermList(Scope, &P2, End, Frame, &A);
                Out->Kind = AMLV_INTEGER;
                // Types according to ACPI: 0=Uninit, 1=Int, 2=Str, 3=Buf, 4=Pkg, 5=Field
                switch (A.Kind) {
                    case AMLV_INTEGER: Out->Integer = 1; break;
                    case AMLV_STRING:  Out->Integer = 2; break;
                    case AMLV_BUFFER:  Out->Integer = 3; break;
                    case AMLV_PACKAGE: Out->Integer = 4; break;
                    default:           Out->Integer = 0; break;
                }
                P = P2;
                break;
            }

            case AML_EXT_OP_PREFIX: {
                if (P >= End) { *Ptr = End; return FALSE; }
                UINT8 Ext = *P++;
                switch (Ext) {
                    case AML_EXT_REVISION_OP:
                        Out->Kind = AMLV_INTEGER;
                        Out->Integer = 2;
                        break;
                    case AML_EXT_DEBUG_OP:
                        Out->Kind = AMLV_INTEGER;
                        Out->Integer = 0;
                        break;
                    case AML_EXT_SLEEP_OP:
                    case AML_EXT_STALL_OP: {
                        // Skip the TermArg
                        AmlValue tmp = {0};
                        ExecTermList(Scope, &P, End, Frame, &tmp);
                        Out->Kind = AMLV_UNINITIALIZED;
                        break;
                    }
                    default: {
                        UINT32 PkgLen = ReadPkgLength(&P, End);
                        P += PkgLen;
                        if (P > End) P = End;
                        break;
                    }
                }
                break;
            }

            default: {
                if (Op == '\\' || Op == '^' ||
                    Op == '_' || (Op >= 'A' && Op <= 'Z') ||
                    Op == 0x2E || Op == 0x2F) {
                    P--;
                    CHAR Segs[8][5];
                    BOOL IsRoot = FALSE;
                    INT N = ReadNameString(&P, End, Segs, 8, &IsRoot);
                    if (N > 0) {
                        // Builtin?
                        if (N == 1) {
                            if (StrCaseCmp(Segs[0], "_OSI") == 0) {
                                AmlValue Arg = AMLV_MAKE(AMLV_UNINITIALIZED);
                                if (ExecTermList(Scope, &P, End, Frame, &Arg)) {
                                    AmlValue Args[1] = { Arg };
                                    AmlBuiltinCall("_OSI", Args, 1, Out);
                                } else {
                                    Out->Kind = AMLV_INTEGER;
                                    Out->Integer = 0;
                                }
                                break;
                            }
                            if (StrCaseCmp(Segs[0], "_OS") == 0 ||
                                StrCaseCmp(Segs[0], "_REV") == 0) {
                                AmlBuiltinCall(Segs[0], NULLPTR, 0, Out);
                                break;
                            }
                        }

                        AmlValue V = EvalNameRef(Scope, Segs, N, IsRoot);
                        if (V.Kind == AMLV_NAMEREF && V.NameRef) {
                            AmlValue Res = AMLV_MAKE(AMLV_INTEGER);
                            CallMethod(V.NameRef, NULLPTR, 0, &Res);
                            *Out = Res;
                        } else if (V.Kind != AMLV_UNINITIALIZED) {
                            *Out = V;
                        } else {
                            // Maybe a field?
                            UINT32 Idx = NsResolveFrom(Scope, Segs, N, IsRoot);
                            if (Idx != (UINT32)-1 &&
                                GAml.Entries[Idx].Kind == AML_ENTRY_FIELD) {
                                AmlField *F = (AmlField *)GAml.Entries[Idx].Code;
                                UINT64 Val = 0;
                                if (FieldRead(F, &Val)) {
                                    Out->Kind = AMLV_INTEGER;
                                    Out->Integer = Val;
                                    break;
                                }
                            }
                            Out->Kind = AMLV_INTEGER;
                            Out->Integer = 0;
                        }
                        break;
                    }
                }
                *Ptr = End;
                return FALSE;
            }
        }

        *Ptr = P;
    }

    return TRUE;
}

// ============================================================================
// Package access (public)
// ============================================================================

STATIC BOOL ResolvePath(const CHAR *Path, AmlValue *Out) {
    if (!Path || !Out) return FALSE;

    CHAR Segs[16][5];
    INT N = 0;
    BOOL IsRoot = FALSE;

    const CHAR *P = Path;
    if (*P == '\\') { IsRoot = TRUE; P++; }

    while (*P && N < 16) {
        if (*P == '^') {
            StrCpy(Segs[N], "^");
            N++;
            P++;
            continue;
        }
        CHAR Seg[5] = {0};
        INT K = 0;
        while (*P && *P != '.' && K < 4) {
            Seg[K++] = *P++;
        }
        if (*P == '.') P++;
        if (K == 0) break;
        StrCpy(Segs[N], Seg);
        N++;
    }

    if (N == 0) return FALSE;

    UINT32 Idx = NsResolveFrom(0, Segs, N, TRUE);
    if (Idx == (UINT32)-1) return FALSE;

    AmlName *E = &GAml.Entries[Idx];

    if (E->Kind == AML_ENTRY_METHOD) {
        AmlValue Res = AMLV_MAKE(AMLV_INTEGER);
        CallMethod(E, NULLPTR, 0, &Res);
        *Out = Res;
        return TRUE;
    }
    if (E->Kind == AML_ENTRY_NAME) {
        AmlValue V = EvalNameRef(0, Segs, N, TRUE);
        *Out = V;
        return V.Kind != AMLV_UNINITIALIZED;
    }
    if (E->Kind == AML_ENTRY_FIELD) {
        AmlField *F = (AmlField *)E->Code;
        UINT64 Val = 0;
        if (FieldRead(F, &Val)) {
            Out->Kind = AMLV_INTEGER;
            Out->Integer = Val;
            return TRUE;
        }
    }
    return FALSE;
}

// ============================================================================
// Public API
// ============================================================================

VOID AmlInit(SDTHeader *Dsdt, SDTHeader **Ssdts, INT SsdtCount) {
    MemSet(&GAml, 0, sizeof(GAml));
    GInstructionCounter = 0;

    GAml.Count = 1;
    GAml.Entries[0].Segment[0] = '\0';
    GAml.Entries[0].Parent = (UINT32)-1;
    GAml.Entries[0].Kind = AML_ENTRY_SCOPE;

    if (Dsdt) {
        const UINT8 *Base = (const UINT8*)Dsdt;
        if (Dsdt->Length >= 36) {
            ParseTermList(0, Base + 36, Base + Dsdt->Length);
        }
    }

    for (INT I = 0; I < SsdtCount; I++) {
        SDTHeader *S = Ssdts[I];
        if (!S || S->Length < 36) continue;
        const UINT8 *Base = (const UINT8*)S;
        ParseTermList(0, Base + 36, Base + S->Length);
    }

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "aml: parsed %u entries, %u opregions, %u fields\n",
                  GAml.Count, GAml.OpRegionCount, GAml.FieldCount);
}

BOOL AmlExecByPath(const CHAR *Path, UINT64 *Out) {
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    if (!ResolvePath(Path, &V)) return FALSE;
    if (Out) *Out = V.Integer;
    return TRUE;
}

BOOL AmlGetInteger(const CHAR *Path, UINT64 *Out) {
    return AmlExecByPath(Path, Out);
}

BOOL AmlGetString(const CHAR *Path, CHAR *Out, INT MaxLen) {
    if (!Out || MaxLen <= 0) return FALSE;
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    if (!ResolvePath(Path, &V)) return FALSE;
    if (V.Kind != AMLV_STRING || !V.Data) return FALSE;

    INT N = (INT)V.DataLen;
    if (N >= MaxLen) N = MaxLen - 1;
    for (INT I = 0; I < N; I++) Out[I] = (CHAR)V.Data[I];
    Out[N] = '\0';
    return TRUE;
}

BOOL AmlFindPath(const CHAR *Path) {
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    return ResolvePath(Path, &V);
}

const CHAR *AmlGetKind(const CHAR *Path) {
    if (!Path) return "Unknown";

    CHAR Segs[16][5];
    INT N = 0;
    BOOL IsRoot = FALSE;

    const CHAR *P = Path;
    if (*P == '\\') { IsRoot = TRUE; P++; }

    while (*P && N < 16) {
        if (*P == '^') { StrCpy(Segs[N], "^"); N++; P++; continue; }
        CHAR Seg[5] = {0};
        INT K = 0;
        while (*P && *P != '.' && K < 4) Seg[K++] = *P++;
        if (*P == '.') P++;
        if (K == 0) break;
        StrCpy(Segs[N], Seg);
        N++;
    }

    if (N == 0) return "Unknown";

    UINT32 Idx = NsResolveFrom(0, Segs, N, TRUE);
    if (Idx == (UINT32)-1) return "Unknown";

    switch (GAml.Entries[Idx].Kind) {
        case AML_ENTRY_SCOPE:       return "Scope";
        case AML_ENTRY_NAME:        return "Name";
        case AML_ENTRY_METHOD:      return "Method";
        case AML_ENTRY_DEVICE:      return "Device";
        case AML_ENTRY_ALIAS:       return "Alias";
        case AML_ENTRY_OPREGION:    return "OpRegion";
        case AML_ENTRY_FIELD:       return "Field";
        case AML_ENTRY_INDEXFIELD:  return "IndexField";
        case AML_ENTRY_BANKFIELD:   return "BankField";
        case AML_ENTRY_PROCESSOR:   return "Processor";
        case AML_ENTRY_POWERRES:    return "PowerRes";
        case AML_ENTRY_THERMALZONE: return "ThermalZone";
    }
    return "Unknown";
}

INT AmlGetPackageCount(const CHAR *Path) {
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    if (!ResolvePath(Path, &V)) return -1;
    if (V.Kind != AMLV_PACKAGE) return -1;
    return (INT)V.NumElements;
}

BOOL AmlGetPackageInt(const CHAR *Path, INT Index, UINT64 *Out) {
    if (Index < 0 || !Out) return FALSE;
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    if (!ResolvePath(Path, &V)) return FALSE;
    if (V.Kind != AMLV_PACKAGE || !V.Data) return FALSE;
    if (Index >= (INT)V.NumElements) return FALSE;

    const UINT8 *P = V.Data;
    const UINT8 *End = P + V.DataLen;

    AmlFrame Sub = {0};
    AmlValue Val = AMLV_MAKE(AMLV_UNINITIALIZED);

    for (INT I = 0; I <= Index; I++) {
        if (P >= End) return FALSE;
        const UINT8 *Save = P;
        if (!ExecTermList(0, &P, End, &Sub, &Val)) {
            P = SkipTermArg(Save, End);
            if (!P) return FALSE;
        }
    }
    *Out = Val.Integer;
    return TRUE;
}

BOOL AmlGetPackageString(const CHAR *Path, INT Index, CHAR *Out, INT MaxLen) {
    if (Index < 0 || !Out || MaxLen <= 0) return FALSE;
    AmlValue V = AMLV_MAKE(AMLV_UNINITIALIZED);
    if (!ResolvePath(Path, &V)) return FALSE;
    if (V.Kind != AMLV_PACKAGE || !V.Data) return FALSE;
    if (Index >= (INT)V.NumElements) return FALSE;

    const UINT8 *P = V.Data;
    const UINT8 *End = P + V.DataLen;

    AmlFrame Sub = {0};
    AmlValue Val = AMLV_MAKE(AMLV_UNINITIALIZED);

    for (INT I = 0; I <= Index; I++) {
        if (P >= End) return FALSE;
        const UINT8 *Save = P;
        if (!ExecTermList(0, &P, End, &Sub, &Val)) {
            P = SkipTermArg(Save, End);
            if (!P) return FALSE;
        }
    }
    if (Val.Kind != AMLV_STRING || !Val.Data) return FALSE;

    INT N = (INT)Val.DataLen;
    if (N >= MaxLen) N = MaxLen - 1;
    for (INT I = 0; I < N; I++) Out[I] = (CHAR)Val.Data[I];
    Out[N] = '\0';
    return TRUE;
}

// ============================================================================
// Method call with arguments (public)
// ============================================================================

BOOL AmlCallMethod(const CHAR *Path, INT ArgCount, AmlValue *Args, AmlValue *Out) {
    if (!Path || !Out) return FALSE;

    CHAR Segs[16][5];
    INT N = 0;
    BOOL IsRoot = FALSE;

    const CHAR *P = Path;
    if (*P == '\\') { IsRoot = TRUE; P++; }
    while (*P && N < 16) {
        if (*P == '^') { StrCpy(Segs[N], "^"); N++; P++; continue; }
        CHAR Seg[5] = {0};
        INT K = 0;
        while (*P && *P != '.' && K < 4) Seg[K++] = *P++;
        if (*P == '.') P++;
        if (K == 0) break;
        StrCpy(Segs[N], Seg);
        N++;
    }
    if (N == 0) return FALSE;

    UINT32 Idx = NsResolveFrom(0, Segs, N, TRUE);
    if (Idx == (UINT32)-1) return FALSE;

    AmlName *E = &GAml.Entries[Idx];
    if (E->Kind != AML_ENTRY_METHOD) return FALSE;

    return CallMethod(E, Args, ArgCount, Out);
}

AmlValue AmlMakeInteger(UINT64 Val) {
    AmlValue V = AMLV_MAKE(AMLV_INTEGER);
    V.Integer = Val;
    return V;
}

AmlValue AmlMakeString(const CHAR *Str) {
    AmlValue V = AMLV_MAKE(AMLV_STRING);
    V.Data = (const UINT8 *)Str;
    V.DataLen = StrLen(Str);
    V.NumElements = 0;
    V.Integer = (UINT64)(UINTPTR)Str;
    return V;
}

AmlValue AmlMakeBuffer(const VOID *Data, UINT32 Len) {
    AmlValue V = AMLV_MAKE(AMLV_BUFFER);
    V.Data = (const UINT8 *)Data;
    V.DataLen = Len;
    V.NumElements = Len;
    return V;
}

// ============================================================================
// Sleep states
// ============================================================================

STATIC BOOL AmlGetNameSegByString(const CHAR *Name, CHAR *OutSeg) {
    INT Len = StrLen(Name);
    for (INT I = 0; I < 4; I++) {
        OutSeg[I] = (I < Len) ? Name[I] : '_';
    }
    OutSeg[4] = '\0';
    return TRUE;
}

BOOL AmlGetSleepSlpTyp(const CHAR *Name, UINT8 *OutSlpTyp) {
    if (!Name || !OutSlpTyp) return FALSE;

    CHAR Seg[5];
    AmlGetNameSegByString(Name, Seg);

    CHAR Segs[1][5];
    StrCpy(Segs[0], Seg);

    UINT32 Idx = NsResolveFrom(0, Segs, 1, TRUE);
    if (Idx == (UINT32)-1) {
        Idx = NsResolveFrom(0, Segs, 1, FALSE);
        if (Idx == (UINT32)-1) return FALSE;
    }

    AmlName *E = &GAml.Entries[Idx];

    if (E->Kind == AML_ENTRY_METHOD) {
        AmlValue Res = AMLV_MAKE(AMLV_INTEGER);
        CallMethod(E, NULLPTR, 0, &Res);
        *OutSlpTyp = (UINT8)(Res.Integer & 0x07);
        return TRUE;
    }

    if (E->Kind == AML_ENTRY_NAME) {
        const UINT8 *P = E->Code;
        const UINT8 *End = P + E->CodeLen;
        if (P >= End) return FALSE;

        UINT8 V = *P;
        if (V == AML_ZERO_OP) { *OutSlpTyp = 0; return TRUE; }
        if (V == AML_ONE_OP)  { *OutSlpTyp = 1; return TRUE; }

        if (V == AML_PACKAGE_OP || V == AML_VAR_PACKAGE_OP) {
            AmlValue Pkg = AMLV_MAKE(AMLV_UNINITIALIZED);
            AmlFrame Sub = {0};
            const UINT8 *Ptr = P;
            if (ExecTermList(Idx, &Ptr, End, &Sub, &Pkg) &&
                Pkg.Kind == AMLV_PACKAGE) {
                // The first element of the package
                const UINT8 *Q = Pkg.Data;
                const UINT8 *QEnd = Q + Pkg.DataLen;
                AmlValue First = AMLV_MAKE(AMLV_UNINITIALIZED);
                if (Q < QEnd && ExecTermList(0, &Q, QEnd, &Sub, &First)) {
                    *OutSlpTyp = (UINT8)(First.Integer & 0x07);
                    return TRUE;
                }
            }
            *OutSlpTyp = 0;
            return TRUE;
        }

        AmlFrame Sub = {0};
        AmlValue Val = AMLV_MAKE(AMLV_UNINITIALIZED);
        const UINT8 *Ptr = P;
        if (ExecTermList(Idx, &Ptr, End, &Sub, &Val)) {
            *OutSlpTyp = (UINT8)(Val.Integer & 0x07);
            return TRUE;
        }
    }

    return FALSE;
}

BOOL AmlGetS5SlpTyp(UINT8 *OutSlpTyp) {
    return AmlGetSleepSlpTyp("_S5", OutSlpTyp);
}

BOOL AmlGetS4SlpTyp(UINT8 *OutSlpTyp) {
    return AmlGetSleepSlpTyp("_S4", OutSlpTyp);
}

BOOL AmlGetS3SlpTyp(UINT8 *OutSlpTyp) {
    return AmlGetSleepSlpTyp("_S3", OutSlpTyp);
}