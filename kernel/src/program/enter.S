.text
.global program_enter
.type program_enter, @function
program_enter:
    # (entry, stack_top, write, context) -> entry(write, context)
    push %rbp
    mov %rsp, %rbp
    mov %rdi, %rax
    mov %rsi, %rsp
    mov %rdx, %rdi
    mov %rcx, %rsi
    call *%rax
    mov %rbp, %rsp
    pop %rbp
    ret
.section .note.GNU-stack,"",@progbits
