.code64
.section .text

.extern SegmentNotPresentHandler

.global SegmentNotPresentStub
SegmentNotPresentStub:
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

    mov %rsp, %rdi              # RDI = InterruptFrame pointer
    mov 120(%rsp), %rsi         # RSI = error code

    call SegmentNotPresentHandler

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

    add $8, %rsp      /* Removing error code*/
    iretq

.section .note.GNU-stack
