# Publish the decimal count through the program write callback once per turn.
.section .text
.ascii "CRONK001"
.global counter_entry
counter_entry:
    push %r12
    push %r13
    push %r14
    sub $16, %rsp
    mov %rdi, %r12
    mov %rsi, %r13
    xor %r14d, %r14d
1:
    hlt
    inc %r14
    lea 15(%rsp), %rsi
    movb $0, (%rsi)
    mov %r14, %rax
    mov $10, %ecx
2:
    xor %edx, %edx
    div %rcx
    add $48, %dl
    dec %rsi
    mov %dl, (%rsi)
    test %rax, %rax
    jnz 2b
    mov %r13, %rdi
    call *%r12
    cmp $1000, %r14
    jne 1b
    add $16, %rsp
    pop %r14
    pop %r13
    pop %r12
    ret
