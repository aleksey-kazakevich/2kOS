#pragma once

#include <Fs/Vfs.h>
#include <Types.h>

// ============================================================================
// ELF64 CONSTANTS
// ============================================================================

#define ELF_MAGIC       0x464C457FU   // "\x7FELF"
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define ELF_VERSION_CUR 1

#define EM_X86_64       62

#define ET_REL          1
#define ET_EXEC         2
#define ET_DYN          3

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_PHDR         6
#define PT_TLS          7
#define PT_GNU_STACK    0x6474E551
#define PT_GNU_RELRO    0x6474E552

#define PF_X            (1 << 0)
#define PF_W            (1 << 1)
#define PF_R            (1 << 2)

#define ELF_MAX_PHN     32

// ============================================================================
// STRUCTURES
// ============================================================================

typedef struct {
    UINT8  EIdent[16];
    UINT16 EType;
    UINT16 EMachine;
    UINT32 EVersion;
    UINT64 EEntry;
    UINT64 EPhOff;
    UINT64 EShOff;
    UINT32 EFlags;
    UINT16 EEhSize;
    UINT16 EPhEntSize;
    UINT16 EPhNum;
    UINT16 EShEntSize;
    UINT16 EShNum;
    UINT16 EShStrNdx;
} ATTRIBUTE(packed) Elf64Ehdr;

typedef struct {
    UINT32 PType;
    UINT32 PFlags;
    UINT64 POffset;
    UINT64 PVaddr;
    UINT64 PPaddr;
    UINT64 PFilesz;
    UINT64 PMemsz;
    UINT64 PAlign;
} ATTRIBUTE(packed) Elf64Phdr;

// ============================================================================
// API
// ============================================================================

// Загрузить ELF из памяти. Возвращает SUCCESS или код ошибки.
// - OutPageTable: новое адресное пространство (PML4)
// - OutEntry:     точка входа
// - OutStackTop:  верхушка user-стека (RSP)
INT ElfLoad(const VOID *Image, USIZE ImageSize,
            UINT64 **OutPageTable,
            UINT64 *OutEntry,
            UINT64 *OutStackTop,
            INT ArgC, const CHAR *ArgV[]);

// Загрузить ELF из VFS-файла
INT ElfLoadFromFile(VfsFile *File,
                    UINT64 **OutPageTable,
                    UINT64 *OutEntry,
                    UINT64 *OutStackTop,
                    INT ArgC, const CHAR *ArgV[]);

// Создать user-задачу из ELF-образа
INT UserTaskCreateFromElf(const VOID *Image, USIZE ImageSize,
                          const CHAR *Name,
                          INT ArgC, const CHAR *ArgV[]);

// Создать user-задачу из ELF-файла
INT UserTaskCreateFromElfFile(const CHAR *Path,
                              const CHAR *Name,
                              INT ArgC, const CHAR *ArgV[]);

// Проверка — является ли буфер валидным ELF64
BOOL ElfIsValid(const VOID *Image, USIZE ImageSize);