.section .text.__x86_indirect_thunk, "ax", @progbits

/* Macro for generating a tank for a specific register*/
.macro GEN_INDIRECT_THUNK reg
.global __x86_indirect_thunk_\reg
.type __x86_indirect_thunk_\reg, @function
__x86_indirect_thunk_\reg:
    call 2f
1:
    pause
    lfence
    jmp 1b
2:
    mov %\reg, (%rsp)
    ret
.endm

/* We generate tanks for all the necessary registers*/
GEN_INDIRECT_THUNK rax
GEN_INDIRECT_THUNK rbx
GEN_INDIRECT_THUNK rcx
GEN_INDIRECT_THUNK rdx
GEN_INDIRECT_THUNK rsi
GEN_INDIRECT_THUNK rdi
GEN_INDIRECT_THUNK rbp
GEN_INDIRECT_THUNK r8
GEN_INDIRECT_THUNK r9
GEN_INDIRECT_THUNK r10
GEN_INDIRECT_THUNK r11
GEN_INDIRECT_THUNK r12
GEN_INDIRECT_THUNK r13
GEN_INDIRECT_THUNK r14
GEN_INDIRECT_THUNK r15

.section .note.GNU-stack, "", @progbits
