# Cronk flat program ABI (x86-64 SysV): rdi = write callback, rsi = context.
# Code is position independent. Return normally to exit.
.section .text
.ascii "CRONK001"
.global hello_entry
hello_entry:
    push %rbp
    mov %rsp, %rbp
    mov %rdi, %rax
    mov %rsi, %rdi
    lea message(%rip), %rsi
    call *%rax
    pop %rbp
    ret
message:
    .asciz "Hello from a Cronk program!\n"
