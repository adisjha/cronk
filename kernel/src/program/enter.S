.text
.global user_enter
user_enter:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp,(%rdi)
    mov %rsp,(%rsi)
    pushq $0x8b
    pushq $0x405ff8
    pushq $0x202
    pushq $0x83
    pushq $0x400008
    mov $0x400ff0,%edi
    xor %esi,%esi
    xor %eax,%eax
    xor %ebx,%ebx
    xor %ecx,%ecx
    xor %edx,%edx
    xor %ebp,%ebp
    xor %r8d,%r8d
    xor %r9d,%r9d
    xor %r10d,%r10d
    xor %r11d,%r11d
    xor %r12d,%r12d
    xor %r13d,%r13d
    xor %r14d,%r14d
    xor %r15d,%r15d
    iretq
.global user_leave
user_leave:
    cli
    mov %rdi,%rsp
    mov %esi,%eax
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
.section .note.GNU-stack,"",@progbits
