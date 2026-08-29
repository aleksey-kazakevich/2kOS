; SYSCALL input: user stack, IF cleared (SFMASK).
; We switch to TSS.RSP0, process the call and return via IRETQ.
; Not using SYSRET: GDT incompatible with STAR[63:48]+16.

[BITS 64]
default rel

extern SyscallHandler
extern SyscallStackTop
extern SyscallUserRsp

USER_CS equ 0x1B
USER_SS equ 0x23

%macro POP_GPRS 0
    pop rax
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
%endmacro

global SyscallEntry
SyscallEntry:
    mov [SyscallUserRsp], rsp
    mov rsp, [SyscallStackTop]
    and rsp, ~0xF

    ; IRET frame: RIP, CS, RFLAGS, RSP, SS
    push USER_SS
    push qword [SyscallUserRsp]
    push r11
    push USER_CS
    push rcx

    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    cld
    sti

    ; User ABI: rax=nr, rdi, rsi, rdx, r10, r8, r9
    ; SysV:     rdi, rsi, rdx, rcx, r8, r9, stack
    push r9
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    call SyscallHandler
    add rsp, 8

    mov [rsp], rax

    cli
    POP_GPRS
    iretq

global SyscallInt80Handler
SyscallInt80Handler:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx
    push rdx
    push rcx
    push rax

    push r9
    mov r9, r8
    mov r8, r10
    mov rcx, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, rax

    call SyscallHandler
    add rsp, 8

    mov [rsp], rax
    POP_GPRS
    iretq

global SyscallSetup
SyscallSetup:
    ret

section .note.GNU-stack
