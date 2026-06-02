#include "kernel/common.h"

static unsigned long g_boot_hartid;
static unsigned long g_boot_ticks;
static int g_has_time_ext = 1;
static struct timer_event g_timer_pool[MAX_TIMER_EVENTS];
static struct timer_event *g_timer_head;
static struct task_event g_task_pool[MAX_TASK_EVENTS];
static struct task_event *g_task_head;
static unsigned long g_task_seq;
static struct timeout_message g_timeout_pool[MAX_TIMER_EVENTS];
static int g_in_task_runner;
static int g_task_runner_depth;
static int g_current_task_priority = -2147483647;
static int g_test_priorities[4];

static void local_irq_disable(void) { asm volatile("csrci sstatus, (1 << 1)"); }
static void local_irq_enable(void) { asm volatile("csrsi sstatus, (1 << 1)"); }

void plic_init(void) {
    *(volatile unsigned int *)PLIC_PRIORITY(UART_IRQ) = 1;
    *(volatile unsigned int *)PLIC_ENABLE(g_boot_hartid) |= (1u << UART_IRQ);
    *(volatile unsigned int *)PLIC_THRESHOLD(g_boot_hartid) = 0;
}

static int plic_claim(void) { return *(volatile unsigned int *)PLIC_CLAIM(g_boot_hartid); }
static void plic_complete(int irq) { *(volatile unsigned int *)PLIC_CLAIM(g_boot_hartid) = (unsigned int)irq; }

static void task_queue_push(struct task_event *ev) {
    if (!g_task_head) {
        g_task_head = ev;
        ev->next = NULL;
        return;
    }
    struct task_event *it = g_task_head, *prev = NULL;
    while (it) {
        if (ev->priority > it->priority || (ev->priority == it->priority && ev->seq < it->seq)) break;
        prev = it;
        it = it->next;
    }
    if (!prev) {
        ev->next = g_task_head;
        g_task_head = ev;
    } else {
        ev->next = it;
        prev->next = ev;
    }
}

static struct task_event *task_queue_pop(void) {
    struct task_event *ev = g_task_head;
    if (ev) {
        g_task_head = ev->next;
        ev->next = NULL;
    }
    return ev;
}

void add_task(task_callback_t cb, void *arg, int priority) {
    if (!cb) return;
    local_irq_disable();
    for (int i = 0; i < MAX_TASK_EVENTS; i++) {
        if (!g_task_pool[i].used) {
            g_task_pool[i].used = 1;
            g_task_pool[i].running = 0;
            g_task_pool[i].priority = priority;
            g_task_pool[i].seq = g_task_seq++;
            g_task_pool[i].cb = cb;
            g_task_pool[i].arg = arg;
            g_task_pool[i].next = NULL;
            task_queue_push(&g_task_pool[i]);
            break;
        }
    }
    local_irq_enable();
}

void run_task_queue(void) {
    local_irq_disable();
    int entry_priority = g_current_task_priority;
    if (g_in_task_runner) {
        if (!g_task_head || g_task_head->priority <= entry_priority) {
            local_irq_enable();
            return;
        }
    } else {
        g_in_task_runner = 1;
    }
    g_task_runner_depth++;
    local_irq_enable();
    while (1) {
        local_irq_disable();
        struct task_event *ev = task_queue_pop();
        g_current_task_priority = ev ? ev->priority : entry_priority;
        local_irq_enable();
        if (!ev) break;
        ev->running = 1;
        ev->cb(ev->arg);
        local_irq_disable();
        ev->running = 0;
        ev->used = 0;
        local_irq_enable();
    }
    local_irq_disable();
    g_current_task_priority = entry_priority;
    g_task_runner_depth--;
    if (g_task_runner_depth == 0) g_in_task_runner = 0;
    local_irq_enable();
}

static struct timer_event *alloc_timer_event(void) {
    for (int i = 0; i < MAX_TIMER_EVENTS; i++) {
        if (!g_timer_pool[i].used) {
            g_timer_pool[i].used = 1;
            g_timer_pool[i].next = NULL;
            return &g_timer_pool[i];
        }
    }
    return NULL;
}

static void timer_enqueue_abs(unsigned long expire_tick, timer_callback_t cb, void *arg) {
    struct timer_event *ev = alloc_timer_event();
    if (!ev) return;
    ev->expire_tick = expire_tick;
    ev->cb = cb;
    ev->arg = arg;
    if (!g_timer_head || expire_tick < g_timer_head->expire_tick) {
        ev->next = g_timer_head;
        g_timer_head = ev;
        return;
    }
    struct timer_event *it = g_timer_head;
    while (it->next && it->next->expire_tick <= expire_tick) it = it->next;
    ev->next = it->next;
    it->next = ev;
}

void add_timer(void (*callback)(void *), void *arg, int sec) {
    if (!callback || sec < 0) return;
    unsigned long now = csr_time();
    unsigned long delta = (unsigned long)sec * TIME_FREQ;
    local_irq_disable();
    timer_enqueue_abs(now + delta, callback, arg);
    local_irq_enable();
}

static struct timer_event *timer_pop_expired(unsigned long now) {
    if (!g_timer_head || g_timer_head->expire_tick > now) return NULL;
    struct timer_event *ev = g_timer_head;
    g_timer_head = ev->next;
    ev->next = NULL;
    return ev;
}

int timeout_alloc_slot(void) {
    for (int i = 0; i < MAX_TIMER_EVENTS; i++) {
        if (!g_timeout_pool[i].used) {
            g_timeout_pool[i].used = 1;
            return i;
        }
    }
    return -1;
}

struct timeout_message *timeout_get_pool(void) {
    return g_timeout_pool;
}

unsigned long boot_ticks(void) {
    return g_boot_ticks;
}

static void timeout_print_task(void *arg) {
    struct timeout_message *m = (struct timeout_message *)arg;
    if (!m || !m->used) return;
    uart_puts("[settimeout] at ");
    uart_put_dec(g_boot_ticks);
    uart_puts(" (scheduled ");
    uart_put_dec(m->created_sec);
    uart_puts(" -> ");
    uart_put_dec(m->expire_sec);
    uart_puts("): ");
    uart_puts(m->text);
    uart_puts("\n");
    m->used = 0;
}

static void timeout_timer_cb(void *arg) { add_task(timeout_print_task, arg, 1); }

static void testtask_p1(void *arg) { (void)arg; uart_puts("P1 start\n"); uart_puts("P1 end\n"); }
static void testtask_p3(void *arg) { (void)arg; uart_puts("P3 start\n"); add_task(testtask_p1, NULL, g_test_priorities[0]); uart_puts("P3 end\n"); }
static void testtask_p2(void *arg) { (void)arg; uart_puts("P2 start\n"); add_task(testtask_p3, NULL, g_test_priorities[2]); uart_puts("P2 end\n"); }
static void testtask_p4(void *arg) { (void)arg; uart_puts("P4 start\n"); add_task(testtask_p2, NULL, g_test_priorities[1]); uart_puts("P4 end\n"); }
static void testtask_seed(void *arg) {
    (void)arg;
    g_test_priorities[0] = 10;
    g_test_priorities[1] = 20;
    g_test_priorities[2] = 30;
    g_test_priorities[3] = 40;
    add_task(testtask_p4, NULL, g_test_priorities[3]);
}

void cmd_testtask(void) {
    uart_puts("[testtask] schedule chained priority tasks\n");
    add_timer(testtask_seed, NULL, 0);
    timer_set_next();
}

static void testpreempt_hi(void *arg) { (void)arg; uart_puts("HI preempt task\n"); }
static void testpreempt_timer_cb(void *arg) { (void)arg; add_task(testpreempt_hi, NULL, 100); }
static void testpreempt_low(void *arg) {
    (void)arg;
    uart_puts("LOW start\n");
    add_timer(testpreempt_timer_cb, NULL, 0);
    timer_set_next();
    for (volatile unsigned long i = 0; i < 2000000UL; i++) {}
    uart_puts("LOW end\n");
}
static void testpreempt_seed(void *arg) { (void)arg; add_task(testpreempt_low, NULL, 1); }

void cmd_testpreempt(void) {
    uart_puts("[testpreempt] expect: LOW start -> HI preempt task -> LOW end\n");
    add_timer(testpreempt_seed, NULL, 0);
    timer_set_next();
}

void timer_set_next(void) {
    unsigned long next = csr_time() + (TIME_FREQ / 100UL);
    if (g_timer_head) next = g_timer_head->expire_tick;
    if (g_has_time_ext) {
        struct sbi_ret ret = sbi_call(SBI_EXT_TIME, SBI_FID_SET_TIMER, next, 0, 0, 0, 0, 0);
        if (ret.error) g_has_time_ext = 0;
    }
    if (!g_has_time_ext) {
        asm volatile("mv a0, %0; li a7, 0; ecall" : : "r"(next) : "a0", "a7", "memory");
    }
}

void sched_on_timer_interrupt(void) {
    g_boot_ticks += 1;
    unsigned long now = csr_time();
    while (1) {
        struct timer_event *ev = timer_pop_expired(now);
        if (!ev) break;
        if (ev->cb) ev->cb(ev->arg);
        ev->used = 0;
    }
    timer_set_next();
    run_task_queue();
}

void sched_on_external_interrupt(void) {
    int plic_irq = plic_claim();
    if (plic_irq == (int)UART_IRQ) uart_handle_irq();
    if (plic_irq) plic_complete(plic_irq);
    run_task_queue();
}

void sched_set_boot_hartid(unsigned long hartid) {
    g_boot_hartid = hartid;
}

void sched_set_time_ext(int has_ext) {
    g_has_time_ext = has_ext;
}

void sched_timeout_schedule(struct timeout_message *m, int sec) {
    add_timer(timeout_timer_cb, m, sec);
    timer_set_next();
}
