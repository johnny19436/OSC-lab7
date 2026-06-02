#include "kernel/common.h"

void start_kernel(unsigned long hartid, const void *fdt) {
    unsigned long base = probe_uart_base_from_fdt(fdt);
    if (base) uart_set_base(base);

    mm_init(fdt);
    if (!mm_ready()) DTB_FATAL("mm init failed");
    thread_system_init();

    asm volatile("csrw sscratch, zero");
    struct sbi_ret probe = sbi_call(SBI_EXT_BASE, SBI_FID_PROBE_EXT, SBI_EXT_TIME, 0, 0, 0, 0, 0);
    sched_set_boot_hartid(hartid);
    sched_set_time_ext((probe.error || probe.value == 0) ? 0 : 1);

    plic_init();
    uart_irq_init();
    timer_set_next();
    asm volatile("li t0, (1 << 5); csrs sie, t0;");
    asm volatile("li t0, (1 << 9); csrs sie, t0;");
    asm volatile("csrsi sstatus, (1 << 1)");

    shell();
}
