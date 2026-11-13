.section .text
.ascii "CRONKA64"
.global hello_entry
hello_entry:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x0, x1
    adr x1, message
    blr x9
    ldp x29, x30, [sp], #16
    ret
message:
    .asciz "Hello from an ARM64 Cronk program!\n"
