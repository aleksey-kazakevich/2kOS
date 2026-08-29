OUTPUT_FORMAT(elf64-x86-64)
ENTRY(LongModeEntry)

PHDRS
{
    text        PT_LOAD FLAGS(5);  /* PF_R | PF_X */
    rodata      PT_LOAD FLAGS(4);  /* PF_R        */
    data        PT_LOAD FLAGS(6);  /* PF_R | PF_W */
}

SECTIONS
{
    /* Limine требует базовый виртуальный адрес 0xFFFFFFFF80200000 */
    KernelVirtStart = 0xFFFFFFFF80200000;
    KernelPhysStart = 0x200000; /* Физический старт на 2 МиБ */
    
    . = KernelVirtStart;

    KernelTextStart = .;
    .text : AT(KernelPhysStart + (KernelTextStart - KernelVirtStart)) {
        . = ALIGN(8);
        KEEP(*(.limine_reqs))
        KEEP(*(.limine_reqs.*))
        . = ALIGN(8);

        *(.text)
        *(.text*)
        *(.apboot)
    } :text
    KernelTextEnd = .;
    
    . = ALIGN(4096);

    KernelRodataStart = .;
    .rodata : AT(KernelPhysStart + (KernelRodataStart - KernelVirtStart)) {
        *(.rodata)
        *(.rodata*)
        
        /* Чексумма перенесена сюда, в неизменяемые данные (rodata) */
        __checksum_start = .;
        KEEP(*(.checksum))
        __checksum_end = .;
    } :rodata
    KernelRodataEnd = .;
    
    . = ALIGN(4096);

    KernelDataStart = .;
    .data : AT(KernelPhysStart + (KernelDataStart - KernelVirtStart)) {
        *(.data)
        *(.data*)
    } :data
    KernelDataEnd = .;

    . = ALIGN(4096);

    KernelBssStart = .;
    .bss : AT(KernelPhysStart + (KernelBssStart - KernelVirtStart)) {
        *(.bss)
        *(.bss*)
        *(COMMON)
        
        . = ALIGN(16);
        StackStart = .;
        . += 262144; /* 256 KiB stack */
        StackEnd = .;

        . = ALIGN(16);
        TssBuffer = .;
        . += 104;
    } :data
    KernelBssEnd = .;

    /* 
     * ВАЖНО: Секцию .heap (NOLOAD) мы удалили. 
     * Выделяйте память под кучу динамически в kmain через карту памяти Limine!
     */

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.debug*)
        *(.got)
        *(.got.plt)
        *(.plt)
        *(.note.GNU-stack)
    }
    
    /* Математика для ваших внутренних переменных размера ядра */
    KernelVirtEnd = .;
    KernelPhysEnd = KernelPhysStart + (KernelVirtEnd - KernelVirtStart);
    KernelSize = KernelVirtEnd - KernelVirtStart;
}
