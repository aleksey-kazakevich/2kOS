# Boot64.S - 64-bit entry point
.code64
.section .text

.extern StackEnd
.extern KMain
.extern GdtDesc
.globl LongModeEntry

LongModeEntry:
    # 1. We perform an absolute jump to high virtual addresses.
    movabs $HigherHalfTrampoline, %rax
    jmp *%rax

HigherHalfTrampoline:
    # 2. We reload the GDT, now using the actual virtual address.
    movabs $GdtDesc, %rax
    lgdt (%rax)

    # Perform a far jump to the new CS (0x08).
    pushq $0x08                # New CS = Kernel Code (0x08)
    movabs $AfterGdtReload, %rax
    pushq %rax
    lretq                      # Far jump = jump to new CS

AfterGdtReload:
    # Segment registers
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    
    # Stack
    movabs $StackEnd, %rsp
    and $-16, %rsp

    call EnableSSE

    call KMain

Hang64:
    hlt
    jmp Hang64

EnableSSE:
    # 1. Enable FPU/SSE in CR0
    mov %cr0, %rax
    btr $2, %rax        # CR0.EM = 0 (No FPU emulation)
    bts $1, %rax        # CR0.MP = 1 (FPU monitoring)
    btr $3, %rax        # CR0.TS = 0 (Do not save state)
    mov %rax, %cr0
    
    # 2. Enable SSE in CR4
    mov %cr4, %rax
    bts $9, %rax        # CR4.OSFXSR = 1 (Allow SSE instruction)
    bts $10, %rax       # CR4.OSXMMEXCPT = 1 (Allow SSE exceptions)
    mov %rax, %cr4
    
    # 3. Initialize FPU (clear state)
    fninit
    
    ret

.section .note.GNU-stack
