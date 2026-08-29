# Gdt.S - Global Descriptor Table
.section .data
.align 8

.globl Gdt
Gdt:
    .quad 0x0000000000000000     # 0x00: Null
    .quad 0x00209A0000000000     # 0x08: Kernel Code (64-bit, Ring 0)
    .quad 0x0000920000000000     # 0x10: Kernel Data (Ring 0)
    .quad 0x0020FA0000000000     # 0x18: User Code (64-bit, Ring 3) — селектор с RPL3 = 0x1B
    .quad 0x0000F20000000000     # 0x20: User Data (Ring 3) — selector with RPL3 = 0x23
    # 64-bit TSS (104 bytes)
    .quad 0x0000006800008968     # 0x28: TSS low (type=0x9, 64-bit TSS available)
    .quad 0x0000000000000000     # 0x30: TSS high (base[63:32])
GdtEnd:

.section .rodata
.align 8

.globl GdtDesc
GdtDesc:
    .word (GdtEnd - Gdt - 1)
    .quad Gdt

.section .note.GNU-stack
