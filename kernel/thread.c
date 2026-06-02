#include "kernel/common.h"

#define THREAD_STACK_PAGES 1
extern void ret_from_exception(void);

static struct task_struct *g_run_queue;
static struct task_struct *g_all_tasks;
static struct task_struct *g_current_task;
static int g_next_pid = 1;

static void enqueue_task(struct task_struct *task) {
    if (!g_run_queue) {
        g_run_queue = task;
        task->next = task;
        return;
    }
    struct task_struct *tail = g_run_queue;
    while (tail->next != g_run_queue) tail = tail->next;
    tail->next = task;
    task->next = g_run_queue;
}

static void all_tasks_add(struct task_struct *task) {
    task->all_next = g_all_tasks;
    g_all_tasks = task;
}

struct task_struct *get_current(void) {
    return g_current_task;
}

static void thread_trampoline(void) {
    struct task_struct *cur = get_current();
    if (cur && cur->entry) cur->entry(cur->arg);
    thread_exit();
}

static void thread_context_init(struct task_struct *task, unsigned long ra, unsigned long sp) {
    task->thread.ra = ra;
    task->thread.sp = sp;
    for (int i = 0; i < 12; i++) task->thread.s[i] = 0;
}

static void task_init_signal_fields(struct task_struct *task) {
    task->sig_handler = 0;
    task->sig_pending = 0;
    task->in_signal = 0;
    task->sig_stack_base = 0;
}

static void task_init_common(struct task_struct *task, int pid, int parent_pid) {
    task->pid = pid;
    task->state = TASK_RUNNABLE;
    task->is_user = 0;
    task->parent_pid = parent_pid;
    task->wait_pid = -1;
    task->exit_status = 0;
    task->pgd = vm_kernel_pgd();
    task->user_code_size = 0;
    task->user_stack_base = 0;
    task->user_stack_size = 0;
    task->mmap_base_hint = 0;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        task->mmap_regions[i].used = 0;
        task->mmap_regions[i].prot = 0;
        task->mmap_regions[i].flags = 0;
        task->mmap_regions[i].start = 0;
        task->mmap_regions[i].length = 0;
    }
    task_init_signal_fields(task);
    task->entry = NULL;
    task->arg = NULL;
    task->next = task;
    task->all_next = NULL;
}

static void pt_regs_zero(struct pt_regs *frame) {
    for (unsigned long i = 0; i < sizeof(struct pt_regs) / sizeof(unsigned long); i++) {
        ((unsigned long *)frame)[i] = 0;
    }
}

static void pt_regs_copy(struct pt_regs *dst, const struct pt_regs *src) {
    for (unsigned long i = 0; i < sizeof(struct pt_regs) / sizeof(unsigned long); i++) {
        ((unsigned long *)dst)[i] = ((unsigned long *)src)[i];
    }
}

void thread_system_init(void) {
    struct task_struct *boot = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!boot) DTB_FATAL("thread init alloc failed");
    task_init_common(boot, 0, -1);
    boot->kernel_stack_base = 0;
    asm volatile("mv %0, sp" : "=r"(boot->thread.sp));
    boot->thread.ra = 0;
    g_run_queue = boot;
    g_current_task = boot;
    all_tasks_add(boot);
}

int thread_create(void (*fn)(void *), void *arg) {
    if (!fn) return -1;
    struct task_struct *task = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!task) return -1;
    unsigned long stack = (unsigned long)allocate(PAGE_SIZE * THREAD_STACK_PAGES);
    if (!stack) {
        free(task);
        return -1;
    }
    task_init_common(task, g_next_pid++, 0);
    task->kernel_stack_base = stack;
    task->entry = fn;
    task->arg = arg;
    thread_context_init(task, (unsigned long)thread_trampoline,
                        stack + PAGE_SIZE * THREAD_STACK_PAGES);
    enqueue_task(task);
    all_tasks_add(task);
    return task->pid;
}

void schedule(void) {
    struct task_struct *prev = get_current();
    if (!prev || !g_run_queue) return;
    struct task_struct *next = prev->next;
    while (next != prev && next->state != TASK_RUNNABLE) next = next->next;
    if (next->state != TASK_RUNNABLE || next == prev) return;
    vm_switch_pgd(next->pgd);
    g_current_task = next;
    switch_to(prev, next);
}

void thread_exit(void) {
    struct task_struct *cur = get_current();
    if (cur && cur->pid != 0) task_mark_exit(cur, 0);
    while (1) schedule();
}

void kill_zombies(void) {
    (void)get_current();
    (void)g_all_tasks;
}

static void thread_demo_fn(void *arg) {
    unsigned long n = (unsigned long)arg;
    for (int i = 0; i < 5; i++) {
        uart_puts("Thread id: ");
        uart_hex((unsigned long)get_current()->pid);
        uart_puts(" ");
        uart_hex((unsigned long)i);
        uart_puts(" ");
        uart_hex(n);
        uart_puts("\n");
        for (volatile unsigned long j = 0; j < 70000000UL; j++) {
        }
        schedule();
    }
    thread_exit();
}

void cmd_testthread(void) {
    uart_puts("[be1] spawn 3 kernel threads\n");
    for (unsigned long i = 0; i < 3; i++) thread_create(thread_demo_fn, (void *)i);
}

struct task_struct *task_find_by_pid(int pid) {
    struct task_struct *it = g_all_tasks;
    while (it) {
        if (it->pid == pid && it->state != TASK_UNUSED) return it;
        it = it->all_next;
    }
    return NULL;
}

static int task_alloc_common(struct task_struct **out, unsigned long *kstack_out) {
    struct task_struct *task = (struct task_struct *)allocate(sizeof(struct task_struct));
    if (!task) return -1;
    unsigned long kstack = (unsigned long)allocate(PAGE_SIZE * THREAD_STACK_PAGES);
    if (!kstack) {
        free(task);
        return -1;
    }
    *out = task;
    *kstack_out = kstack;
    return 0;
}

int task_create_user(unsigned long entry, unsigned long user_stack_base, unsigned long user_stack_size,
                     unsigned long pgd, unsigned long user_code_size, int parent_pid,
                     struct pt_regs *seed_regs) {
    struct task_struct *task;
    unsigned long kstack = 0;
    if (task_alloc_common(&task, &kstack) < 0) return -1;
    struct pt_regs *frame =
        (struct pt_regs *)(kstack + PAGE_SIZE * THREAD_STACK_PAGES - sizeof(struct pt_regs));
    pt_regs_zero(frame);
    if (seed_regs) pt_regs_copy(frame, seed_regs);
    frame->sepc = entry;
    frame->sp = user_stack_base + user_stack_size - 0x100;
    frame->sstatus &= ~(1UL << 8);
    frame->sstatus |= (1UL << 5);

    task_init_common(task, g_next_pid++, parent_pid);
    task->is_user = 1;
    task->kernel_stack_base = kstack;
    task->pgd = pgd;
    task->user_code_size = user_code_size;
    task->user_stack_base = user_stack_base;
    task->user_stack_size = user_stack_size;
    if (task->is_user && task->pgd && task->user_stack_size) {
        for (unsigned long va = task->user_stack_base;
             va < task->user_stack_base + task->user_stack_size; va += PAGE_SIZE) {
            if (!vm_walk_pa(task->pgd, va)) {
                unsigned long page = (unsigned long)allocate(PAGE_SIZE);
                if (!page) {
                    free((void *)kstack);
                    free(task);
                    return -1;
                }
                for (unsigned long i = 0; i < PAGE_SIZE; i++) ((unsigned char *)page)[i] = 0;
                if (vm_map_pages(task->pgd, va, PAGE_SIZE, VIRT_TO_PHYS_U(page), PROT_USER_RW) < 0) {
                    free((void *)kstack);
                    free(task);
                    return -1;
                }
            }
        }
    }
    thread_context_init(task, (unsigned long)ret_from_exception, (unsigned long)frame);
    enqueue_task(task);
    all_tasks_add(task);
    return task->pid;
}

void task_wake_waiter(int child_pid) {
    struct task_struct *it = g_all_tasks;
    while (it) {
        if (it->state == TASK_WAITING && (it->wait_pid == -1 || it->wait_pid == child_pid)) {
            it->state = TASK_RUNNABLE;
            it->wait_pid = -1;
        }
        it = it->all_next;
    }
}

void task_mark_exit(struct task_struct *task, int status) {
    if (!task || task->pid == 0) return;
    task->exit_status = status;
    task->state = TASK_ZOMBIE;
    task_wake_waiter(task->pid);
}

static void task_free_resources(struct task_struct *task) {
    if (!task || task->state != TASK_ZOMBIE) return;
    if (task->is_user && task->pgd) {
        vm_destroy_user_pgd(task->pgd);
        task->pgd = 0;
    }
    if (task->sig_stack_base) {
        free((void *)task->sig_stack_base);
        task->sig_stack_base = 0;
    }
    if (task->kernel_stack_base) {
        free((void *)task->kernel_stack_base);
        task->kernel_stack_base = 0;
    }
    task->state = TASK_UNUSED;
}

int waitpid_internal(int pid) {
    struct task_struct *cur = get_current();
    if (!cur) return -1;
    while (1) {
        struct task_struct *it = g_all_tasks;
        int found_child = 0;
        while (it) {
            if (it->state != TASK_UNUSED && it->parent_pid == cur->pid &&
                (pid == -1 || it->pid == pid)) {
                found_child = 1;
                if (it->state == TASK_ZOMBIE) {
                    int retpid = it->pid;
                    task_free_resources(it);
                    return retpid;
                }
            }
            it = it->all_next;
        }
        if (!found_child) return -1;
        cur->state = TASK_WAITING;
        cur->wait_pid = pid;
        schedule();
        cur->state = TASK_RUNNABLE;
        cur->wait_pid = -1;
    }
}
