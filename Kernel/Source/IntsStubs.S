.code64
.section .text

.macro IsrStub num
.global IsrStub\num
IsrStub\num:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax

    iretq
.endm

// Generating 32 handlers
IsrStub 0
IsrStub 1
IsrStub 2
IsrStub 3
IsrStub 4
IsrStub 5
IsrStub 6
IsrStub 7
IsrStub 8
IsrStub 9
IsrStub 10
IsrStub 11
IsrStub 12
IsrStub 13
IsrStub 14
IsrStub 15
IsrStub 16
IsrStub 17
IsrStub 18
IsrStub 19
IsrStub 20
IsrStub 21
IsrStub 22
IsrStub 23
IsrStub 24
IsrStub 25
IsrStub 26
IsrStub 27
IsrStub 28
IsrStub 29
IsrStub 30
IsrStub 31

.section .note.GNU-stack