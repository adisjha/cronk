.section .text
.ascii "CRONKA64"
.global counter_entry
counter_entry:
    stp x19, x20, [sp, #-48]!
    stp x21, x30, [sp, #16]
    mov x19, x0
    mov x20, x1
    mov x21, #0
1:
    mov x8, #2
    svc #0
    add x21, x21, #1
    add x1, sp, #47
    strb wzr, [x1]
    mov x9, x21
    mov x10, #10
2:
    udiv x11, x9, x10
    msub x12, x11, x10, x9
    add w12, w12, #48
    strb w12, [x1, #-1]!
    mov x9, x11
    cbnz x9, 2b
    mov x0, x20
    blr x19
    cmp x21, #1000
    b.ne 1b
    ldp x21, x30, [sp, #16]
    ldp x19, x20, [sp], #48
    ret
