.section .text
.ascii "CRONK001"
.global crash_entry
crash_entry:
    cli                         # Ring 3 cannot disable interrupts (#GP).
    ret
