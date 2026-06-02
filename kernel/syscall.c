#include "kernel/common.h"

#define SYS_GETPID 0
#define SYS_UART_READ 1
#define SYS_UART_WRITE 2
#define SYS_EXEC 3
#define SYS_FORK 4
#define SYS_WAITPID 5
#define SYS_EXIT 6
#define SYS_STOP 7
#define SYS_DISPLAY 8
#define SYS_USLEEP 9
#define SYS_SIGNAL 10
#define SYS_SIGRETURN 11
#define SYS_KILL 12
#define SYS_MMAP 13

#define USER_STACK_TOP 0x0000004000000000UL
#define USER_STACK_SIZE (PAGE_SIZE * 4)
#define USER_STACK_BASE (USER_STACK_TOP - USER_STACK_SIZE)
#define MMAP_BASE 0x0000002000000000UL
#define MMAP_LIMIT USER_STACK_BASE

#define MMAP_PROT_NONE 0
#define MMAP_PROT_READ 1
#define MMAP_PROT_WRITE 2
#define MMAP_PROT_EXEC 4

#define MAP_ANONYMOUS 0x20
#define MAP_POPULATE 0x8000

static void bytes_copy(unsigned char *dst, const unsigned char *src, unsigned long n);

static unsigned long irq_save(void) {
    unsigned long sstatus = 0;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrci sstatus, (1 << 1)");
    return sstatus;
}

static void irq_restore(unsigned long sstatus) {
    if (sstatus & (1UL << 1)) asm volatile("csrsi sstatus, (1 << 1)");
}

static unsigned char *user_ptr(unsigned long uaddr) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->pgd) return NULL;
    unsigned long pa = vm_walk_pa(cur->pgd, uaddr);
    if (!pa) return NULL;
    return (unsigned char *)PHYS_TO_VIRT_U(pa);
}

static int copy_from_user(char *dst, const char *src, int max_len) {
    if (!dst || !src || max_len <= 0) return -1;
    for (int i = 0; i < max_len; i++) {
        unsigned char *p = user_ptr((unsigned long)src + (unsigned long)i);
        if (!p) return -1;
        dst[i] = (char)(*p);
        if (dst[i] == '\0') return i + 1;
    }
    return max_len;
}

static int copy_to_user(char *dst, const char *src, long len) {
    if (!dst || !src || len < 0) return -1;
    for (long i = 0; i < len; i++) {
        unsigned char *p = user_ptr((unsigned long)dst + (unsigned long)i);
        if (!p) return -1;
        *p = (unsigned char)src[i];
    }
    return 0;
}

static unsigned long prot_to_pte(int prot) {
    unsigned long pte = PTE_V | PTE_U | PTE_A | PTE_D;
    if (prot & MMAP_PROT_READ) pte |= PTE_R;
    if (prot & MMAP_PROT_WRITE) pte |= (PTE_R | PTE_W);
    if (prot & MMAP_PROT_EXEC) pte |= (PTE_R | PTE_X);
    return pte;
}

static int ranges_overlap(unsigned long a_start, unsigned long a_len, unsigned long b_start,
                          unsigned long b_len) {
    unsigned long a_end = a_start + a_len;
    unsigned long b_end = b_start + b_len;
    return (a_start < b_end) && (b_start < a_end);
}

static int va_overlaps_task_regions(struct task_struct *task, unsigned long start, unsigned long len) {
    if (!task || len == 0) return 1;
    if (ranges_overlap(start, len, 0, task->user_code_size)) return 1;
    if (ranges_overlap(start, len, task->user_stack_base, task->user_stack_size)) return 1;
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (!task->mmap_regions[i].used) continue;
        if (ranges_overlap(start, len, task->mmap_regions[i].start, task->mmap_regions[i].length)) return 1;
    }
    return 0;
}

static int task_find_free_region_slot(struct task_struct *task) {
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (!task->mmap_regions[i].used) return i;
    }
    return -1;
}

static unsigned long find_mmap_base(struct task_struct *task, unsigned long hint, unsigned long len) {
    unsigned long base = hint ? align_up_ul(hint, PAGE_SIZE) : 0;
    if (base < MMAP_BASE) base = MMAP_BASE;
    while (base + len <= MMAP_LIMIT) {
        if (!va_overlaps_task_regions(task, base, len)) return base;
        base += PAGE_SIZE;
    }
    return 0;
}

static int region_prot_for_va(struct task_struct *task, unsigned long va, int *prot_out) {
    if (!task || !prot_out) return -1;
    if (va < task->user_code_size) {
        *prot_out = MMAP_PROT_READ | MMAP_PROT_EXEC;
        return 0;
    }
    if (va >= task->user_stack_base && va < task->user_stack_base + task->user_stack_size) {
        *prot_out = MMAP_PROT_READ | MMAP_PROT_WRITE;
        return 0;
    }
    for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
        if (!task->mmap_regions[i].used) continue;
        unsigned long start = task->mmap_regions[i].start;
        unsigned long end = start + task->mmap_regions[i].length;
        if (va >= start && va < end) {
            *prot_out = task->mmap_regions[i].prot;
            return 0;
        }
    }
    return -1;
}

int handle_user_page_fault(unsigned long scause_code, unsigned long fault_addr) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user || !cur->pgd) return -1;

    unsigned long page_va = fault_addr & ~(PAGE_SIZE - 1UL);
    unsigned long *pte = vm_lookup_pte(cur->pgd, page_va);
    if (scause_code == 15UL && pte && ((*pte & (PTE_V | PTE_COW)) == (PTE_V | PTE_COW))) {
        unsigned long old_pa = ((*pte >> 10) << 12);
        unsigned long flags = *pte & ((1UL << 10) - 1);
        int refs = mm_page_refcount(old_pa);
        if (refs > 1) {
            unsigned long new_page = (unsigned long)allocate(PAGE_SIZE);
            if (!new_page) return -1;
            bytes_copy((unsigned char *)new_page, (const unsigned char *)PHYS_TO_VIRT_U(old_pa), PAGE_SIZE);
            unsigned long new_flags = (flags | PTE_W | PTE_D) & ~PTE_COW;
            *pte = (((unsigned long)VIRT_TO_PHYS_U(new_page) >> 12) << 10) | new_flags;
            if (mm_page_put(old_pa) < 0) return -1;
        } else if (refs == 1) {
            unsigned long new_flags = (flags | PTE_W | PTE_D) & ~PTE_COW;
            *pte = ((old_pa >> 12) << 10) | new_flags;
        } else {
            return -1;
        }
        vm_flush_tlb();
        uart_puts("[Permission fault]: ");
        uart_hex(fault_addr);
        uart_puts("\n");
        return 0;
    }

    int need = 0;
    if (scause_code == 12UL) need = MMAP_PROT_EXEC;
    else if (scause_code == 13UL) need = MMAP_PROT_READ;
    else if (scause_code == 15UL) need = MMAP_PROT_WRITE;
    else return -1;

    int prot = 0;
    if (region_prot_for_va(cur, fault_addr, &prot) < 0) return -1;
    if ((prot & need) == 0) return -1;

    if (vm_walk_pa(cur->pgd, page_va)) return -1;

    unsigned long page = (unsigned long)allocate(PAGE_SIZE);
    if (!page) return -1;
    for (unsigned long i = 0; i < PAGE_SIZE; i++) ((unsigned char *)page)[i] = 0;
    if (vm_map_pages(cur->pgd, page_va, PAGE_SIZE, VIRT_TO_PHYS_U(page), prot_to_pte(prot)) < 0) return -1;

    uart_puts("[Translation fault]: ");
    uart_hex(fault_addr);
    uart_puts("\n");
    return 0;
}

static int find_initrd_file(const char *filename, unsigned long *entry, unsigned long *size) {
    char *p = (char *)INITRD_BASE;
    int want = c_strlen(filename) + 1;
    while (c_memcmp(p + sizeof(struct cpio_newc_header), "TRAILER!!!", 10)) {
        struct cpio_newc_header *hdr = (struct cpio_newc_header *)p;
        int namesize = hex_to_int(hdr->c_namesize, 8);
        int filesize = hex_to_int(hdr->c_filesize, 8);
        int headsize = align_up_i((int)sizeof(struct cpio_newc_header) + namesize, 4);
        int datasize = align_up_i(filesize, 4);
        if (namesize == want && !c_memcmp(p + sizeof(struct cpio_newc_header), filename, want)) {
            *entry = (unsigned long)(p + headsize);
            *size = (unsigned long)filesize;
            return 0;
        }
        p += headsize + datasize;
    }
    return -1;
}

int spawn_user_process(const char *filename, int parent_pid) {
    unsigned long image = 0, image_size = 0;
    if (!filename) return -1;
    if (find_initrd_file(filename, &image, &image_size) < 0) return -1;
    unsigned long pgd = vm_user_pgd_create();
    if (!pgd) return -1;

    unsigned long mapped = 0;
    while (mapped < image_size) {
        unsigned long page = (unsigned long)allocate(PAGE_SIZE);
        if (!page) {
            vm_destroy_user_pgd(pgd);
            return -1;
        }
        unsigned long left = image_size - mapped;
        unsigned long chunk = left < PAGE_SIZE ? left : PAGE_SIZE;
        for (unsigned long i = 0; i < PAGE_SIZE; i++) {
            ((unsigned char *)page)[i] = (i < chunk) ? ((unsigned char *)image)[mapped + i] : 0;
        }
        if (vm_map_pages(pgd, mapped, PAGE_SIZE, VIRT_TO_PHYS_U(page), PROT_USER_RX) < 0) {
            vm_destroy_user_pgd(pgd);
            return -1;
        }
        mapped += PAGE_SIZE;
    }

    for (unsigned long off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
        unsigned long stack_page = (unsigned long)allocate(PAGE_SIZE);
        if (!stack_page) {
            vm_destroy_user_pgd(pgd);
            return -1;
        }
        for (unsigned long i = 0; i < PAGE_SIZE; i++) ((unsigned char *)stack_page)[i] = 0;
        if (vm_map_pages(pgd, USER_STACK_BASE + off, PAGE_SIZE, VIRT_TO_PHYS_U(stack_page), PROT_USER_RW) < 0) {
            vm_destroy_user_pgd(pgd);
            return -1;
        }
    }
    return task_create_user(0, USER_STACK_BASE, USER_STACK_SIZE, pgd, mapped, parent_pid, NULL);
}

static void bytes_copy(unsigned char *dst, const unsigned char *src, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) dst[i] = src[i];
}

static void regs_copy(struct pt_regs *dst, const struct pt_regs *src) {
    bytes_copy((unsigned char *)dst, (const unsigned char *)src, sizeof(struct pt_regs));
}

static volatile int g_uart_write_lock = 0;
static int g_uart_line_owner_pid = -1;

static void uart_write_lock(void) {
    while (__sync_lock_test_and_set(&g_uart_write_lock, 1)) {
    }
}

static void uart_write_unlock(void) {
    __sync_lock_release(&g_uart_write_lock);
}

static long sys_getpid(void) {
    struct task_struct *cur = get_current();
    return cur ? cur->pid : 0;
}

static long sys_uart_read(char *buf, long count) {
    if (!buf || count < 0) return -1;
    if (count == 0) return 0;
    uart_write_lock();
    g_uart_line_owner_pid = -1;
    uart_write_unlock();
    long n = 0;
    while (n < count) {
        unsigned long sstatus = 0;
        asm volatile("csrr %0, sstatus" : "=r"(sstatus));
        if ((sstatus & (1UL << 1)) == 0) {
            asm volatile("csrsi sstatus, (1 << 1)");
        }
        char c = uart_getc();
        if ((sstatus & (1UL << 1)) == 0) {
            asm volatile("csrci sstatus, (1 << 1)");
        }
        if (copy_to_user(buf + n, &c, 1) < 0) return -1;
        n++;
        if (c == '\n') break;
        if (n >= 1) break;
    }
    return n;
}

static long sys_uart_write(const char *buf, long count) {
    if (!buf || count < 0) return -1;
    struct task_struct *cur = get_current();
    int pid = cur ? cur->pid : -1;

    while (1) {
        uart_write_lock();
        if (g_uart_line_owner_pid == -1 || g_uart_line_owner_pid == pid) {
            g_uart_line_owner_pid = pid;
            break;
        }
        uart_write_unlock();
        schedule();
    }

    int release_owner = 0;
    for (long i = 0; i < count; i++) {
        char c = 0;
        if (copy_from_user(&c, buf + i, 1) < 0) {
            uart_write_unlock();
            return -1;
        }
        uart_putc(c);
        if (c == '\n') release_owner = 1;
    }
    if (release_owner) g_uart_line_owner_pid = -1;
    uart_write_unlock();
    return count;
}

static long sys_exec(struct pt_regs *regs, const char *path) {
    char filename[128];
    unsigned long image = 0, image_size = 0;
    struct task_struct *cur = get_current();
    unsigned long old_pgd = (cur && cur->is_user) ? cur->pgd : 0;
    if (!path) return -1;
    if (copy_from_user(filename, path, sizeof(filename)) < 0) return -1;
    filename[sizeof(filename) - 1] = '\0';
    if (find_initrd_file(filename, &image, &image_size) < 0) return -1;

    unsigned long new_pgd = vm_user_pgd_create();
    if (!new_pgd) return -1;
    unsigned long mapped = 0;
    while (mapped < image_size) {
        unsigned long page = (unsigned long)allocate(PAGE_SIZE);
        if (!page) {
            vm_destroy_user_pgd(new_pgd);
            return -1;
        }
        unsigned long left = image_size - mapped;
        unsigned long chunk = left < PAGE_SIZE ? left : PAGE_SIZE;
        for (unsigned long i = 0; i < PAGE_SIZE; i++) {
            ((unsigned char *)page)[i] = (i < chunk) ? ((unsigned char *)image)[mapped + i] : 0;
        }
        if (vm_map_pages(new_pgd, mapped, PAGE_SIZE, VIRT_TO_PHYS_U(page), PROT_USER_RX) < 0) {
            vm_destroy_user_pgd(new_pgd);
            return -1;
        }
        mapped += PAGE_SIZE;
    }
    for (unsigned long off = 0; off < USER_STACK_SIZE; off += PAGE_SIZE) {
        unsigned long stack_page = (unsigned long)allocate(PAGE_SIZE);
        if (!stack_page) {
            vm_destroy_user_pgd(new_pgd);
            return -1;
        }
        for (unsigned long i = 0; i < PAGE_SIZE; i++) ((unsigned char *)stack_page)[i] = 0;
        if (vm_map_pages(new_pgd, USER_STACK_BASE + off, PAGE_SIZE, VIRT_TO_PHYS_U(stack_page), PROT_USER_RW) < 0) {
            vm_destroy_user_pgd(new_pgd);
            return -1;
        }
    }

    if (cur) {
        cur->is_user = 1;
        cur->pgd = new_pgd;
        cur->user_code_size = mapped;
        cur->user_stack_base = USER_STACK_BASE;
        cur->user_stack_size = USER_STACK_SIZE;
        cur->mmap_base_hint = 0;
        for (int i = 0; i < MAX_MMAP_REGIONS; i++) {
            cur->mmap_regions[i].used = 0;
            cur->mmap_regions[i].prot = 0;
            cur->mmap_regions[i].flags = 0;
            cur->mmap_regions[i].start = 0;
            cur->mmap_regions[i].length = 0;
        }
        cur->sig_pending = 0;
        cur->in_signal = 0;
        if (cur->sig_stack_base) {
            free((void *)cur->sig_stack_base);
            cur->sig_stack_base = 0;
        }
    }
    vm_switch_pgd(new_pgd);
    if (old_pgd && old_pgd != new_pgd) vm_destroy_user_pgd(old_pgd);
    regs->sepc = 0;
    regs->sp = USER_STACK_TOP - 0x100;
    regs->a0 = 0;
    regs->a1 = 0;
    regs->a2 = 0;
    regs->a3 = 0;
    regs->a4 = 0;
    regs->a5 = 0;
    regs->a6 = 0;
    regs->a7 = 0;
    regs->sstatus &= ~(1UL << 8);
    return 0;
}

static int fork_share_range(unsigned long parent_pgd, unsigned long child_pgd, unsigned long va_start,
                            unsigned long va_len, int writable_region, int *cow_touched) {
    for (unsigned long off = 0; off < va_len; off += PAGE_SIZE) {
        unsigned long va = va_start + off;
        unsigned long *ppte = vm_lookup_pte(parent_pgd, va);
        if (!ppte) continue;
        unsigned long pte = *ppte;
        unsigned long pa = (pte >> 10) << 12;
        unsigned long flags = pte & ((1UL << 10) - 1);
        unsigned long child_flags = flags;
        if (writable_region && (flags & (PTE_W | PTE_COW))) {
            unsigned long cow_flags = (flags & ~PTE_W) | PTE_COW;
            *ppte = ((pa >> 12) << 10) | cow_flags;
            child_flags = cow_flags;
            if (cow_touched) *cow_touched = 1;
        }
        if (vm_map_pages(child_pgd, va, PAGE_SIZE, pa, child_flags) < 0) return -1;
        if (mm_page_get(pa) < 0) return -1;
    }
    return 0;
}

static long sys_fork(struct pt_regs *regs) {
    unsigned long irq_state = irq_save();
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user || !cur->pgd || !cur->user_stack_size) {
        irq_restore(irq_state);
        return -1;
    }
    struct pt_regs child_regs;
    bytes_copy((unsigned char *)&child_regs, (const unsigned char *)regs, sizeof(struct pt_regs));
    unsigned long child_pgd = vm_user_pgd_create();
    if (!child_pgd) {
        irq_restore(irq_state);
        return -1;
    }

    int cow_touched = 0;
    if (fork_share_range(cur->pgd, child_pgd, 0, cur->user_code_size, 0, &cow_touched) < 0) {
        vm_destroy_user_pgd(child_pgd);
        irq_restore(irq_state);
        return -1;
    }
    if (fork_share_range(cur->pgd, child_pgd, cur->user_stack_base, cur->user_stack_size, 1, &cow_touched) <
        0) {
        vm_destroy_user_pgd(child_pgd);
        irq_restore(irq_state);
        return -1;
    }
    for (int r = 0; r < MAX_MMAP_REGIONS; r++) {
        if (!cur->mmap_regions[r].used) continue;
        unsigned long start = cur->mmap_regions[r].start;
        unsigned long len = cur->mmap_regions[r].length;
        int wr = (cur->mmap_regions[r].prot & MMAP_PROT_WRITE) != 0;
        if (fork_share_range(cur->pgd, child_pgd, start, len, wr, &cow_touched) < 0) {
            vm_destroy_user_pgd(child_pgd);
            irq_restore(irq_state);
            return -1;
        }
    }
    if (cow_touched) vm_flush_tlb();

    child_regs.a0 = 0;
    child_regs.sepc = regs->sepc + 4;
    int child_pid =
        task_create_user(child_regs.sepc, cur->user_stack_base, cur->user_stack_size, child_pgd,
                         cur->user_code_size, cur->pid, &child_regs);
    if (child_pid < 0) vm_destroy_user_pgd(child_pgd);
    struct task_struct *child = task_find_by_pid(child_pid);
    if (child) {
        child->sig_handler = cur->sig_handler;
        child->mmap_base_hint = cur->mmap_base_hint;
        for (int i = 0; i < MAX_MMAP_REGIONS; i++) child->mmap_regions[i] = cur->mmap_regions[i];
    }
    irq_restore(irq_state);
    return child_pid;
}

static long sys_waitpid(long pid) {
    return (long)waitpid_internal((int)pid);
}

static long sys_exit(long status) {
    struct task_struct *cur = get_current();
    task_mark_exit(cur, (int)status);
    schedule();
    while (1) {
        schedule();
    }
    return 0;
}

static long sys_stop(long pid) {
    struct task_struct *target = task_find_by_pid((int)pid);
    if (!target || target->pid == 0 || target->state == TASK_UNUSED) return -1;
    task_mark_exit(target, -1);
    return 0;
}

static long sys_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    if (!bmp_image || width == 0 || height == 0) return -1;
    if (width > FB_WIDTH || height > FB_HEIGHT) return -1;

    video_init();

    unsigned int *fb = (unsigned int *)FB_BASE;
    int start_x = (FB_WIDTH - (int)width) / 2;
    int start_y = (FB_HEIGHT - (int)height) / 2;

    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            unsigned long src = (unsigned long)bmp_image + (((unsigned long)y * width + x) * 4UL);
            unsigned char *b0 = user_ptr(src + 0);
            unsigned char *b1 = user_ptr(src + 1);
            unsigned char *b2 = user_ptr(src + 2);
            unsigned char *b3 = user_ptr(src + 3);
            if (!b0 || !b1 || !b2 || !b3) return -1;
            unsigned int pixel = ((unsigned int)(*b0)) | ((unsigned int)(*b1) << 8) |
                                 ((unsigned int)(*b2) << 16) | ((unsigned int)(*b3) << 24);
            fb[(start_y + (int)y) * FB_WIDTH + (start_x + (int)x)] = pixel;
        }
    }
    asm volatile("fence rw, rw" ::: "memory");
    return 0;
}

static long sys_mmap(unsigned long addr, unsigned long length, int prot, int flags) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user || !cur->pgd) return -1;

    unsigned long len = align_up_ul(length, PAGE_SIZE);
    if (len == 0) return -1;

    int slot = task_find_free_region_slot(cur);
    if (slot < 0) return -1;

    unsigned long base = 0;
    unsigned long exact = addr ? (addr & ~(PAGE_SIZE - 1UL)) : 0;
    if (exact && !va_overlaps_task_regions(cur, exact, len) && exact >= MMAP_BASE &&
        exact + len <= MMAP_LIMIT) {
        base = exact;
    } else {
        unsigned long hint = addr ? addr : (cur->mmap_base_hint ? cur->mmap_base_hint : MMAP_BASE);
        base = find_mmap_base(cur, hint, len);
        if (!base) return -1;
    }

    unsigned long pte_flags = prot_to_pte(prot);
    if (flags & MAP_POPULATE) {
        if ((pte_flags & (PTE_R | PTE_W | PTE_X)) != 0) {
            for (unsigned long off = 0; off < len; off += PAGE_SIZE) {
                unsigned long page = (unsigned long)allocate(PAGE_SIZE);
                if (!page) return -1;
                for (unsigned long i = 0; i < PAGE_SIZE; i++) ((unsigned char *)page)[i] = 0;
                if (vm_map_pages(cur->pgd, base + off, PAGE_SIZE, VIRT_TO_PHYS_U(page), pte_flags) < 0) {
                    return -1;
                }
            }
        }
    }

    cur->mmap_regions[slot].used = 1;
    cur->mmap_regions[slot].prot = prot;
    cur->mmap_regions[slot].flags = flags;
    cur->mmap_regions[slot].start = base;
    cur->mmap_regions[slot].length = len;
    cur->mmap_base_hint = base + len;
    return (long)base;
}

static long sys_usleep(unsigned int usec) {
    unsigned long ticks = ((unsigned long)usec * TIME_FREQ) / 1000000UL;
    unsigned long start = csr_time();
    unsigned long end = start + ticks;
    while ((long)(csr_time() - end) < 0) {
        schedule();
    }
    return 0;
}

static long sys_signal(long signum, void (*handler)(void)) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user) return -1;
    if (signum != SIGTERM) return -1;
    long old = (long)cur->sig_handler;
    cur->sig_handler = (unsigned long)handler;
    return old;
}

static long sys_sigreturn(struct pt_regs *regs) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user || !cur->in_signal) return -1;
    regs_copy(regs, &cur->sig_saved_regs);
    cur->in_signal = 0;
    cur->sig_pending = 0;
    if (cur->sig_stack_base) {
        free((void *)cur->sig_stack_base);
        cur->sig_stack_base = 0;
    }
    uart_puts("[sigreturn] signal handler finished\n");
    return 0;
}

static long sys_kill(long pid, long signum) {
    struct task_struct *target = task_find_by_pid((int)pid);
    if (!target || target->state == TASK_UNUSED || target->pid == 0) return -1;
    if (signum != SIGTERM) return -1;
    target->sig_pending = (int)signum;
    return 0;
}

void signal_try_deliver(struct pt_regs *regs) {
    struct task_struct *cur = get_current();
    if (!cur || !cur->is_user || cur->state != TASK_RUNNABLE) return;
    if (cur->in_signal || cur->sig_pending != SIGTERM) return;
    if ((regs->sstatus & (1UL << 8)) != 0) return;
    if (cur->sig_handler == 0) {
        task_mark_exit(cur, -SIGTERM);
        return;
    }

    unsigned long sig_stack = (unsigned long)allocate(PAGE_SIZE);
    if (!sig_stack) {
        task_mark_exit(cur, -SIGTERM);
        return;
    }
    cur->sig_stack_base = sig_stack;
    cur->in_signal = 1;
    regs_copy(&cur->sig_saved_regs, regs);

    unsigned int *tr = (unsigned int *)sig_stack;
    tr[0] = 0x00b00893;
    tr[1] = 0x00000073;
    tr[2] = 0x0000006f;

    regs->sp = sig_stack + PAGE_SIZE;
    regs->ra = sig_stack;
    regs->sepc = cur->sig_handler;
}

long syscall_dispatch(struct pt_regs *regs) {
    unsigned long n = regs->a7;
    switch (n) {
        case SYS_GETPID:
            return sys_getpid();
        case SYS_UART_READ:
            return sys_uart_read((char *)regs->a0, (long)regs->a1);
        case SYS_UART_WRITE:
            return sys_uart_write((const char *)regs->a0, (long)regs->a1);
        case SYS_EXEC:
            return sys_exec(regs, (const char *)regs->a0);
        case SYS_FORK:
            return sys_fork(regs);
        case SYS_WAITPID:
            return sys_waitpid((long)regs->a0);
        case SYS_EXIT:
            return sys_exit((long)regs->a0);
        case SYS_STOP:
            return sys_stop((long)regs->a0);
        case SYS_DISPLAY:
            return sys_display((unsigned int *)regs->a0, (unsigned int)regs->a1, (unsigned int)regs->a2);
        case SYS_USLEEP:
            return sys_usleep((unsigned int)regs->a0);
        case SYS_SIGNAL:
            return sys_signal((long)regs->a0, (void (*)(void))regs->a1);
        case SYS_SIGRETURN:
            return sys_sigreturn(regs);
        case SYS_KILL:
            return sys_kill((long)regs->a0, (long)regs->a1);
        case SYS_MMAP:
            return sys_mmap((unsigned long)regs->a0, (unsigned long)regs->a1, (int)regs->a2,
                            (int)regs->a3);
        default:
            return -1;
    }
}
