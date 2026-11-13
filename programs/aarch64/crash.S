.section .text
.ascii "CRONKA64"
.global crash_entry
crash_entry:
    msr daifset, #2 // EL0 cannot mask kernel interrupts.
    ret
