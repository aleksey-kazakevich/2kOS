#include <Mem/PhysAlloc.h>
#include <Mem/Allocator.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Return.h>
#include <Elf.h>

// ============================================================================
// VALIDATION
// ============================================================================

BOOL ElfIsValid(const VOID *Image, USIZE ImageSize) {
    if (!Image || ImageSize < sizeof(Elf64Ehdr)) return FALSE;

    const Elf64Ehdr *Ehdr = (const Elf64Ehdr *)Image;

    if (*(const UINT32 *)Ehdr->EIdent != ELF_MAGIC) return FALSE;
    if (Ehdr->EIdent[4] != ELFCLASS64) return FALSE;
    if (Ehdr->EIdent[5] != ELFDATA2LSB) return FALSE;
    if (Ehdr->EIdent[6] != ELF_VERSION_CUR) return FALSE;
    if (Ehdr->EMachine != EM_X86_64) return FALSE;
    if (Ehdr->EType != ET_EXEC && Ehdr->EType != ET_DYN) return FALSE;
    if (Ehdr->EEhSize != sizeof(Elf64Ehdr)) return FALSE;
    if (Ehdr->EPhEntSize != sizeof(Elf64Phdr)) return FALSE;
    if (Ehdr->EPhNum == 0 || Ehdr->EPhNum > ELF_MAX_PHN) return FALSE;
    if (Ehdr->EPhOff + (UINT64)Ehdr->EPhNum * sizeof(Elf64Phdr) > ImageSize)
        return FALSE;

    return TRUE;
}

STATIC const Elf64Phdr *ElfGetPhdr(const Elf64Ehdr *Ehdr, INT Index) {
    return (const Elf64Phdr *)((const UINT8 *)Ehdr
           + Ehdr->EPhOff
           + (UINT64)Index * sizeof(Elf64Phdr));
}

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

STATIC UINT64 ElfAlignDown(UINT64 Val, UINT64 Align) {
    return Val & ~(Align - 1);
}

STATIC UINT64 ElfAlignUp(UINT64 Val, UINT64 Align) {
    return (Val + Align - 1) & ~(Align - 1);
}

// Select and color one page in Pml4
STATIC VOID *ElfAllocPage(UINT64 *Pml4, UINT64 Vaddr, UINT64 Flags) {
    VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys) return NULLPTR;

    UINT8 *Virt = (UINT8 *)PhysToVirt((UINT64)Phys);
    MemSet(Virt, 0, PAGE_SIZE);

    INT Rc = PagingMapPage(Pml4, Vaddr, (UINT64)Phys, Flags);
    if (Rc != SUCCESS) {
        PhysAllocFreePage(PhysAllocGet(), Phys);
        return NULLPTR;
    }
    return Virt;
}

// ============================================================================
// MAP ONE PT_LOAD SEGMENT
// ============================================================================

STATIC INT ElfMapSegment(UINT64 *Pml4,
                         const Elf64Phdr *Ph,
                         const UINT8 *Image,
                         USIZE ImageSize) {
    if (Ph->PType != PT_LOAD) return SUCCESS;
    if (Ph->PMemsz == 0) return SUCCESS;

    // Checks
    if (Ph->PFilesz > Ph->PMemsz) return INCORRECT_VALUE;
    if (Ph->POffset + Ph->PFilesz > ImageSize) return INCORRECT_VALUE;
    if (Ph->PVaddr < USER_CODE_VADDR) return INCORRECT_VALUE;
    if (Ph->PVaddr + Ph->PMemsz >= USER_STACK_TOP) return INCORRECT_VALUE;
    if (Ph->PVaddr + Ph->PMemsz < Ph->PVaddr) return INCORRECT_VALUE; // overflow

    UINT64 SegStart = ElfAlignDown(Ph->PVaddr, PAGE_SIZE);
    UINT64 SegEnd   = ElfAlignUp(Ph->PVaddr + Ph->PMemsz, PAGE_SIZE);
    if (SegEnd <= SegStart) return INCORRECT_VALUE;

    // Page Flags
    UINT64 Flags = PTE_USER | PTE_PRESENT;
    if (Ph->PFlags & PF_W) Flags |= PTE_WRITABLE;
    if (!(Ph->PFlags & PF_X)) Flags |= PTE_NO_EXEC;

    // Load
    const UINT8 *SrcBase = Image + Ph->POffset;

    for (UINT64 Va = SegStart; Va < SegEnd; Va += PAGE_SIZE) {
        UINT8 *Dst = (UINT8 *)ElfAllocPage(Pml4, Va, Flags);
        if (!Dst) return NO_MEMORY;

        // We determine how many bytes of the file end up on this page.
        UINT64 PageFileStart = Va;
        UINT64 PageFileEnd   = Va + PAGE_SIZE;

        // Cutting off the segment boundaries in the file
        UINT64 SegFileStart = Ph->PVaddr;
        UINT64 SegFileEnd   = Ph->PVaddr + Ph->PFilesz;

        UINT64 CopyStart = PageFileStart > SegFileStart ? PageFileStart : SegFileStart;
        UINT64 CopyEnd   = PageFileEnd   < SegFileEnd   ? PageFileEnd   : SegFileEnd;

        if (CopyStart < CopyEnd) {
            UINT64 DstOff = CopyStart - Va;
            UINT64 SrcOff = CopyStart - Ph->PVaddr;
            UINT64 CopyLen = CopyEnd - CopyStart;

            MemCpy(Dst + DstOff, SrcBase + SrcOff, CopyLen);
        }
    }

    return SUCCESS;
}

// ============================================================================
// USER STACK SETUP
// ============================================================================

STATIC UINT64 ElfSetupStack(UINT64 *Pml4,
                            INT ArgC, const CHAR *ArgV[]) {
    UINT64 StackSize = USER_STACK_DEFAULT;
    UINT64 StackBase = USER_STACK_TOP - StackSize;

    INT Rc = PagingMapAnonUser(Pml4, StackBase, StackSize,
                               PTE_USER | PTE_WRITABLE | PTE_NO_EXEC);
    if (Rc != SUCCESS) return 0;

    UINT64 Sp = USER_STACK_TOP;

    // Limitation
    if (ArgC < 0) ArgC = 0;
    if (ArgC > 32) ArgC = 32;

    // 1. Copying the argv strings (from top to bottom)
    UINT64 ArgvPtrs[32];
    for (INT I = ArgC - 1; I >= 0; I--) {
        USIZE Len = StrLen(ArgV[I]) + 1;
        Sp -= Len;
        Sp &= ~0xFULL;
        if (PagingCopyToUser(Pml4, Sp, ArgV[I], Len) != SUCCESS) return 0;
        ArgvPtrs[I] = Sp;
    }

    // 2. NULL-argv terminator[]
    Sp -= 8;
    UINT64 Zero = 0;
    if (PagingCopyToUser(Pml4, Sp, &Zero, 8) != SUCCESS) return 0;

    // 3. argv pointers[]
    for (INT I = ArgC - 1; I >= 0; I--) {
        Sp -= 8;
        if (PagingCopyToUser(Pml4, Sp, &ArgvPtrs[I], 8) != SUCCESS) return 0;
    }

    // 4. argc
    Sp -= 8;
    UINT64 ArgcVal = (UINT64)ArgC;
    if (PagingCopyToUser(Pml4, Sp, &ArgcVal, 8) != SUCCESS) return 0;

    // Alignment
    Sp &= ~0xFULL;

    return Sp;
}

// ============================================================================
// PUBLIC: ElfLoad (from memory)
// ============================================================================

INT ElfLoad(const VOID *Image, USIZE ImageSize,
            UINT64 **OutPageTable,
            UINT64 *OutEntry,
            UINT64 *OutStackTop,
	    UINT64 *OutHeapStart,
            INT ArgC, const CHAR *ArgV[]) {

    if (!Image || !OutPageTable || !OutEntry || !OutStackTop)
        RETURN(INCORRECT_VALUE);

    if (!ElfIsValid(Image, ImageSize))
        RETURN(INCORRECT_VALUE);

    const Elf64Ehdr *Ehdr = (const Elf64Ehdr *)Image;

    // Checking the entry point
    UINT64 Entry = Ehdr->EEntry;
    if (Entry < USER_CODE_VADDR || Entry >= USER_STACK_TOP)
        RETURN(INCORRECT_VALUE);

    // Counting the address range
    UINT64 MinVaddr = UINT64MAX;
    UINT64 MaxVaddr = 0;
    BOOL HasLoad = FALSE;

    for (INT I = 0; I < Ehdr->EPhNum; I++) {
        const Elf64Phdr *Ph = ElfGetPhdr(Ehdr, I);
        if (Ph->PType != PT_LOAD) continue;
        if (Ph->PMemsz == 0) continue;
        HasLoad = TRUE;

        UINT64 Start = ElfAlignDown(Ph->PVaddr, PAGE_SIZE);
        UINT64 End   = ElfAlignUp(Ph->PVaddr + Ph->PMemsz, PAGE_SIZE);

        if (Start < MinVaddr) MinVaddr = Start;
        if (End > MaxVaddr)   MaxVaddr = End;
    }

    if (!HasLoad) RETURN(INCORRECT_VALUE);
    if (MinVaddr < USER_CODE_VADDR) RETURN(INCORRECT_VALUE);
    if (MaxVaddr >= USER_STACK_TOP - USER_STACK_DEFAULT) RETURN(INCORRECT_VALUE);

    // Creating an address space
    UINT64 *Pml4 = PagingCreateUserAddressSpace();
    if (!Pml4) RETURN(NO_MEMORY);

    // Map all segments
    for (INT I = 0; I < Ehdr->EPhNum; I++) {
        const Elf64Phdr *Ph = ElfGetPhdr(Ehdr, I);
        INT Rc = ElfMapSegment(Pml4, Ph, (const UINT8 *)Image, ImageSize);
        if (Rc != SUCCESS) {
            PagingDestroyUserAddressSpace(Pml4);
            return Rc;
        }
    }

    // Stack
    UINT64 Sp = ElfSetupStack(Pml4, ArgC, ArgV);
    if (!Sp) {
        PagingDestroyUserAddressSpace(Pml4);
        RETURN(NO_MEMORY);
    }

    *OutPageTable = Pml4;
    *OutEntry = Entry;
    *OutStackTop = Sp;

    if (OutHeapStart) {
        *OutHeapStart = MaxVaddr;
    }

    RETURN(SUCCESS);
}

// ============================================================================
// PUBLIC: ElfLoadFromFile
// ============================================================================

INT ElfLoadFromFile(VfsFile *File,
                    UINT64 **OutPageTable,
                    UINT64 *OutEntry,
                    UINT64 *OutStackTop,
		    UINT64 *OutHeapStart,
                    INT ArgC, const CHAR *ArgV[]) {

    if (!File || !OutPageTable || !OutEntry || !OutStackTop)
        RETURN(INCORRECT_VALUE);

    // Reading the ELF header
    Elf64Ehdr Ehdr;
    VfsSeek(File, 0, SEEK_SET);
    UINT32 Rd = 0;
    INT Rc = VfsRead(File, &Ehdr, sizeof(Ehdr), &Rd);
    if (Rc != SUCCESS || Rd != sizeof(Ehdr)) RETURN(IO_ERROR);

    UINT64 FileSize = File->FInode ? File->FInode->ISize : 0;
    if (FileSize == 0) RETURN(IO_ERROR);
    if (!ElfIsValid(&Ehdr, FileSize)) RETURN(INCORRECT_VALUE);

    // Reading program headers
    Elf64Phdr Phdrs[ELF_MAX_PHN];
    UINT64 PhSize = (UINT64)Ehdr.EPhNum * sizeof(Elf64Phdr);
    VfsSeek(File, Ehdr.EPhOff, SEEK_SET);
    Rc = VfsRead(File, Phdrs, (UINT32)PhSize, &Rd);
    if (Rc != SUCCESS || Rd != PhSize) RETURN(IO_ERROR);

    // Checking the entry
    UINT64 Entry = Ehdr.EEntry;
    if (Entry < USER_CODE_VADDR || Entry >= USER_STACK_TOP)
        RETURN(INCORRECT_VALUE);

    // Creating an address space
    UINT64 *Pml4 = PagingCreateUserAddressSpace();
    if (!Pml4) RETURN(NO_MEMORY);

    UINT64 MaxVaddr = 0;

    // Shipping segments
    for (INT I = 0; I < Ehdr.EPhNum; I++) {
        const Elf64Phdr *Ph = &Phdrs[I];
        if (Ph->PType != PT_LOAD) continue;
        if (Ph->PMemsz == 0) continue;

        if (Ph->PFilesz > Ph->PMemsz) {
            PagingDestroyUserAddressSpace(Pml4);
            RETURN(INCORRECT_VALUE);
        }
        if (Ph->PVaddr < USER_CODE_VADDR ||
            Ph->PVaddr + Ph->PMemsz >= USER_STACK_TOP) {
            PagingDestroyUserAddressSpace(Pml4);
            RETURN(INCORRECT_VALUE);
        }

        UINT64 SegStart = ElfAlignDown(Ph->PVaddr, PAGE_SIZE);
        UINT64 SegEnd   = ElfAlignUp(Ph->PVaddr + Ph->PMemsz, PAGE_SIZE);

        if (SegEnd > MaxVaddr) MaxVaddr = SegEnd;

        UINT64 Flags = PTE_USER | PTE_PRESENT;
        if (Ph->PFlags & PF_W) Flags |= PTE_WRITABLE;
        if (!(Ph->PFlags & PF_X)) Flags |= PTE_NO_EXEC;

        // Offset in the file
        UINT64 FileOff = Ph->POffset;

        for (UINT64 Va = SegStart; Va < SegEnd; Va += PAGE_SIZE) {
            UINT8 *Dst = (UINT8 *)ElfAllocPage(Pml4, Va, Flags);
            if (!Dst) {
                PagingDestroyUserAddressSpace(Pml4);
                RETURN(NO_MEMORY);
            }

            // How many bytes should I copy to this page?
            UINT64 PageStart = Va;
            UINT64 PageEnd   = Va + PAGE_SIZE;
            UINT64 FileStart = Ph->PVaddr;
            UINT64 FileEnd   = Ph->PVaddr + Ph->PFilesz;

            UINT64 CopyStart = PageStart > FileStart ? PageStart : FileStart;
            UINT64 CopyEnd   = PageEnd   < FileEnd   ? PageEnd   : FileEnd;

            if (CopyStart < CopyEnd) {
                UINT64 DstOff = CopyStart - Va;
                UINT64 SrcOff = CopyStart - Ph->PVaddr;
                UINT64 CopyLen = CopyEnd - CopyStart;

                // We read in 4 KB portions
                VfsSeek(File, FileOff + SrcOff, SEEK_SET);
                UINT32 Got = 0;
                Rc = VfsRead(File, Dst + DstOff, (UINT32)CopyLen, &Got);
                if (Rc != SUCCESS || Got != CopyLen) {
                    PagingDestroyUserAddressSpace(Pml4);
                    RETURN(IO_ERROR);
                }
            }
        }
    }

    // Stack
    UINT64 Sp = ElfSetupStack(Pml4, ArgC, ArgV);
    if (!Sp) {
        PagingDestroyUserAddressSpace(Pml4);
        RETURN(NO_MEMORY);
    }

    *OutPageTable = Pml4;
    *OutEntry = Entry;
    *OutStackTop = Sp;

    if (OutHeapStart) {
        *OutHeapStart = MaxVaddr;
    }

    RETURN(SUCCESS);
}

// ============================================================================
// PUBLIC: Task creation
// ============================================================================

INT UserTaskCreateFromElf(const VOID *Image, USIZE ImageSize,
                          const CHAR *Name,
                          INT ArgC, const CHAR *ArgV[]) {

    UINT64 *Pml4 = NULLPTR;
    UINT64 Entry = 0;
    UINT64 Sp = 0;

    UINT64 HeapStart = 0;
    INT Rc = ElfLoad(Image, ImageSize, &Pml4, &Entry, &Sp, &HeapStart, ArgC, ArgV);
    if (Rc != SUCCESS) return Rc;

    return UserTaskCreateFromPageTable(Pml4, Entry, Sp, HeapStart, Name);
}

INT UserTaskCreateFromElfFile(const CHAR *Path,
                              const CHAR *Name,
                              INT ArgC, const CHAR *ArgV[]) {

    VfsFile *File = NULLPTR;
    INT Rc = VfsOpen(VfsCurrentDir, Path, O_READ, &File);
    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "elf: cannot open '%s'\n", Path);
        return Rc;
    }

    UINT64 *Pml4 = NULLPTR;
    UINT64 Entry = 0;
    UINT64 Sp = 0;
    UINT64 HeapStart = 0;

    Rc = ElfLoadFromFile(File, &Pml4, &Entry, &Sp, &HeapStart, ArgC, ArgV);
    VfsClose(File);

    if (Rc != SUCCESS) {
        BaseconPrintf(BASECON_TYPE_ERROR, "elf: load failed (%d)\n", Rc);
        return Rc;
    }

    return UserTaskCreateFromPageTable(Pml4, Entry, Sp, HeapStart, Name);
}