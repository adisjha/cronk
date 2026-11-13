.text
.global arm_boot
arm_boot:
    msr daifset, #15
    msr spsel, #1
    ldr x0, =kernel_boot_stack_top
    mov sp, x0
    b kmain
.bss
.balign 16
kernel_boot_stack:
    .skip 65536
kernel_boot_stack_top:
.text
.global arm_user_enter
arm_user_enter:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x9, sp
    str x9, [x0]
    mov x9, #0x400000
    add x9, x9, #6, lsl #12
    msr sp_el0, x9
    mov x9, #0x400000
    add x9, x9, #8
    msr elr_el1, x9
    msr spsr_el1, xzr
    mov x0, #0x400000
    add x0, x0, #4064
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x6, #0
    mov x7, #0
    mov x8, #0
    mov x9, #0
    mov x10, #0
    mov x11, #0
    mov x12, #0
    mov x13, #0
    mov x14, #0
    mov x15, #0
    mov x16, #0
    mov x17, #0
    mov x18, #0
    mov x19, #0
    mov x20, #0
    mov x21, #0
    mov x22, #0
    mov x23, #0
    mov x24, #0
    mov x25, #0
    mov x26, #0
    mov x27, #0
    mov x28, #0
    mov x29, #0
    mov x30, #0x400000
    add x30, x30, #4080
    eret
.global arm_user_leave
arm_user_leave:
    msr daifset, #2
    mov sp, x0
    mov w0, w1
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #96
    ret
.section .note.GNU-stack,"",%progbits

.text
// Limine supplies x0 = mp_info. Switch to our permanent per-CPU stack before C.
.global arm_secondary_entry
arm_secondary_entry:
    msr daifset, #15
    msr spsel, #1
    ldr x1, [x0, #32] // limine_mp_info.extra_argument
    mov sp, x1
    b arm_secondary_main
