.section ".text.boot"
.global _start

#define PT_REGS_SLOTS 35
#define PT_OFF(n) (8 * (n))

.macro SAVE_GPR reg, slot
    sd \reg, PT_OFF(\slot)(sp)
.endm

.macro RESTORE_GPR reg, slot
    ld \reg, PT_OFF(\slot)(sp)
.endm

.macro SAVE_GPR_BLOCK
    SAVE_GPR ra,  0
    SAVE_GPR gp,  2
    SAVE_GPR tp,  3
    SAVE_GPR t0,  4
    SAVE_GPR t1,  5
    SAVE_GPR t2,  6
    SAVE_GPR s0,  7
    SAVE_GPR s1,  8
    SAVE_GPR a0,  9
    SAVE_GPR a1, 10
    SAVE_GPR a2, 11
    SAVE_GPR a3, 12
    SAVE_GPR a4, 13
    SAVE_GPR a5, 14
    SAVE_GPR a6, 15
    SAVE_GPR a7, 16
    SAVE_GPR s2, 17
    SAVE_GPR s3, 18
    SAVE_GPR s4, 19
    SAVE_GPR s5, 20
    SAVE_GPR s6, 21
    SAVE_GPR s7, 22
    SAVE_GPR s8, 23
    SAVE_GPR s9, 24
    SAVE_GPR s10, 25
    SAVE_GPR s11, 26
    SAVE_GPR t3, 27
    SAVE_GPR t4, 28
    SAVE_GPR t5, 29
    SAVE_GPR t6, 30
.endm

.macro RESTORE_GPR_BLOCK
    RESTORE_GPR ra,  0
    RESTORE_GPR gp,  2
    RESTORE_GPR tp,  3
    RESTORE_GPR t0,  4
    RESTORE_GPR t1,  5
    RESTORE_GPR t2,  6
    RESTORE_GPR s0,  7
    RESTORE_GPR s1,  8
    RESTORE_GPR a0,  9
    RESTORE_GPR a1, 10
    RESTORE_GPR a2, 11
    RESTORE_GPR a3, 12
    RESTORE_GPR a4, 13
    RESTORE_GPR a5, 14
    RESTORE_GPR a6, 15
    RESTORE_GPR a7, 16
    RESTORE_GPR s2, 17
    RESTORE_GPR s3, 18
    RESTORE_GPR s4, 19
    RESTORE_GPR s5, 20
    RESTORE_GPR s6, 21
    RESTORE_GPR s7, 22
    RESTORE_GPR s8, 23
    RESTORE_GPR s9, 24
    RESTORE_GPR s10, 25
    RESTORE_GPR s11, 26
    RESTORE_GPR t3, 27
    RESTORE_GPR t4, 28
    RESTORE_GPR t5, 29
    RESTORE_GPR t6, 30
.endm

_start:
    mv s0, a0
    mv s1, a1
    la sp, _end
    call setup_vm
    la a2, 1f
    li t0, -1
    slli t0, t0, 38
    add a2, a2, t0
    jr a2

1:
    la sp, _end
    la a3, __bss_start
    la a4, __bss_stop
    ble a4, a3, clear_bss_done
clear_bss:
    sd zero, (a3)
    add a3, a3, 8
    blt a3, a4, clear_bss
clear_bss_done:
    call drop_identity_map
    la a2, handle_exception
    csrw stvec, a2
    mv a0, s0
    li t0, -1
    slli t0, t0, 38
    add a1, s1, t0
    tail start_kernel

.global handle_exception
.align 2
handle_exception:
    csrrw sp, sscratch, sp
    bnez sp, save_context
    csrr sp, sscratch
save_context:
    addi sp, sp, -PT_OFF(PT_REGS_SLOTS)
    SAVE_GPR_BLOCK
    csrr s0, sscratch
    csrr s1, sepc
    csrr s2, sstatus
    csrr s3, scause
    csrr s4, stval
    sd s0, PT_OFF(1)(sp)
    sd s1, PT_OFF(31)(sp)
    sd s2, PT_OFF(32)(sp)
    sd s3, PT_OFF(33)(sp)
    sd s4, PT_OFF(34)(sp)

    csrw sscratch, zero
    addi sp, sp, -8
    addi a0, sp, 8
    call do_trap
    addi sp, sp, 8

.global ret_from_exception
ret_from_exception:
    ld s0, PT_OFF(32)(sp)
    andi s0, s0, (1 << 8)
    bnez s0, 1f
    addi s0, sp, PT_OFF(PT_REGS_SLOTS)
    csrw sscratch, s0
1:
    ld s1, PT_OFF(31)(sp)
    ld s2, PT_OFF(32)(sp)
    csrw sepc, s1
    csrw sstatus, s2
    RESTORE_GPR_BLOCK
    ld sp, PT_OFF(1)(sp)
    sret

.global switch_to
switch_to:
    sd ra, 0(a0)
    sd sp, 8(a0)
    sd s0, 16(a0)
    sd s1, 24(a0)
    sd s2, 32(a0)
    sd s3, 40(a0)
    sd s4, 48(a0)
    sd s5, 56(a0)
    sd s6, 64(a0)
    sd s7, 72(a0)
    sd s8, 80(a0)
    sd s9, 88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    ld ra, 0(a1)
    ld sp, 8(a1)
    ld s0, 16(a1)
    ld s1, 24(a1)
    ld s2, 32(a1)
    ld s3, 40(a1)
    ld s4, 48(a1)
    ld s5, 56(a1)
    ld s6, 64(a1)
    ld s7, 72(a1)
    ld s8, 80(a1)
    ld s9, 88(a1)
    ld s10, 96(a1)
    ld s11, 104(a1)
    ret
