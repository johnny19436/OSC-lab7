#include "kernel/common.h"

void do_trap(struct pt_regs *regs) {
    unsigned long cause = regs->scause;
    unsigned long code = cause & ~SCAUSE_INT;
    if ((cause & ~SCAUSE_INT) == SCAUSE_ECALL_U) {
        unsigned long old_sepc = regs->sepc;
        regs->a0 = (unsigned long)syscall_dispatch(regs);
        if (regs->sepc == old_sepc) regs->sepc += 4;
        signal_try_deliver(regs);
        schedule();
        return;
    }
    if (cause & SCAUSE_INT) {
        unsigned long irq = cause & ~SCAUSE_INT;
        if (irq == SCAUSE_TIMER) {
            sched_on_timer_interrupt();
            signal_try_deliver(regs);
            schedule();
            return;
        }
        if (irq == SCAUSE_EXT) {
            sched_on_external_interrupt();
            signal_try_deliver(regs);
            return;
        }
    }
    if (code == 12UL || code == 13UL || code == 15UL) {
        struct task_struct *cur = get_current();
        if (cur && cur->is_user && cur->pid != 0) {
            if (handle_user_page_fault(code, regs->stval) == 0) return;
            uart_puts("[Segmentation fault]: Kill Process\n");
            task_mark_exit(cur, -1);
            schedule();
            return;
        }
    }
    uart_puts("=== S-Mode trap ===\nscause: ");
    uart_hex(regs->scause);
    uart_puts("\nsepc: ");
    uart_hex(regs->sepc);
    uart_puts("\nstval: ");
    uart_hex(regs->stval);
    uart_puts("\n");
}
