.text
.macro interrupt_entry symbol, handler
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
    call \handler
    mov %rax, %rsp
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
    iretq
.endm
interrupt_entry scheduler_irq, scheduler_tick
interrupt_entry keyboard_irq, scheduler_keyboard
interrupt_entry yield_irq, scheduler_dispatch
.section .note.GNU-stack,"",@progbits
