.text
.macro interrupt_entry symbol, handler, fault=0
.global \symbol
\symbol:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rbp
    push %rdi
    push %rsi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov %rsp, %rdi
    and $-16, %rsp
    .if \fault
    mov %rdi,%rbx
    lea 120(%rdi),%rdi
    call \handler
    mov %rbx,%rsp
    .else
    call \handler
    mov %rax,%rsp
    .endif
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rsi
    pop %rdi
    pop %rbp
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    .if \fault
    add $16,%rsp
    .endif
    iretq
.endm
interrupt_entry scheduler_irq, scheduler_tick
interrupt_entry keyboard_irq, scheduler_keyboard
interrupt_entry yield_irq, scheduler_dispatch
interrupt_entry syscall_irq, user_service
interrupt_entry exception_common, user_exception, 1
.macro fault_stub n
fault_\n:
    .if !(\n == 8 || \n == 10 || \n == 11 || \n == 12 || \n == 13 || \n == 14 || \n == 17 || \n == 21 || \n == 29 || \n == 30)
    pushq $0
    .endif
    pushq $\n
    jmp exception_common
.endm
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
fault_stub \n
.endr
.section .rodata
.global exception_vectors
exception_vectors:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
.quad fault_\n
.endr
.section .note.GNU-stack,"",@progbits
