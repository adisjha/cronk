.text
.macro read_cache name, reg
.global \name
.type \name, %function
\name:
    mrs x0, \reg
    ret
.endm
read_cache arm_cache_clidr, clidr_el1
read_cache arm_cache_ccsidr, ccsidr_el1
read_cache arm_cache_csselr, csselr_el1
read_cache arm_cache_mmfr2, id_aa64mmfr2_el1
.global arm_cache_select
.type arm_cache_select, %function
arm_cache_select:
    mov x9, x0
    mov x0, #1
.global arm_cache_select_instruction
arm_cache_select_instruction:
    msr csselr_el1, x9
    isb
    ret
.section .note.GNU-stack,"",%progbits
