[BITS 64]

global Ps2KeyboardIrq
extern Ps2KeyboardHandleIRQ

Ps2KeyboardIrq:
    push rbp
    mov rbp, rsp          ; save a pointer to RIP, CS, RFLAGS
    ; RBP now points to RIP, which is on the stack
    
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r12, rsp          ; save current RSP
    
    and rsp, -16          ; leveling
    
    call Ps2KeyboardHandleIRQ

    mov rsp, r12          ; restore RSP to all pops
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    pop rbp
    
    iretq
