.section .text
.balign 2048
.global arm_vectors
arm_vectors:
.macro vector target
    b \target
    .balign 128
.endm
vector arm_sync_entry
vector arm_irq_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_irq_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_irq_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_sync_entry
vector arm_irq_entry
vector arm_sync_entry
vector arm_sync_entry

.macro entry name, handler
.global \name
\name:
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x9, sp_el0
    mrs x10, elr_el1
    mrs x11, spsr_el1
    str x9, [sp, #248]
    stp x10, x11, [sp, #256]
    mov x0, sp
    bl \handler
    mov sp, x0
    ldr x9, [sp, #248]
    ldp x10, x11, [sp, #256]
    msr sp_el0, x9
    msr elr_el1, x10
    msr spsr_el1, x11
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #272
    eret
.endm
entry arm_sync_entry, arm_sync
entry arm_irq_entry, arm_irq
.section .note.GNU-stack,"",%progbits
