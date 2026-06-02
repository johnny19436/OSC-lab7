#ifndef KERNEL_COMMON_H
#define KERNEL_COMMON_H

#include <stddef.h>
#include <stdint.h>

extern char uart_getc(void);
extern void uart_putc(char c);
extern void uart_puts(const char *s);
extern void uart_hex(unsigned long h);
extern void uart_set_base(unsigned long base);
extern unsigned long uart_get_base(void);
extern void uart_irq_init(void);
extern void uart_handle_irq(void);
extern char __kernel_start, __kernel_end;

#define LOAD_ADDR 0x20000000UL
#define BOOT_MAGIC_LE 0x544F4F42UL
#define MAX_KERNEL_SZ (16UL * 1024UL * 1024UL)
#define PAGE_SIZE 4096UL
#define PAGE_OFFSET 0xffffffc000000000UL
#define SATP_SV39 (8UL << 60)
#define MAKE_SATP(pgd_pa) (SATP_SV39 | ((unsigned long)(pgd_pa) >> 12))
#define PTE_V (1UL << 0)
#define PTE_R (1UL << 1)
#define PTE_W (1UL << 2)
#define PTE_X (1UL << 3)
#define PTE_U (1UL << 4)
#define PTE_G (1UL << 5)
#define PTE_A (1UL << 6)
#define PTE_D (1UL << 7)
#define PTE_COW (1UL << 8)
#define PROT_KERNEL (PTE_V | PTE_R | PTE_W | PTE_X | PTE_G | PTE_A | PTE_D)
#define PROT_MMIO (PTE_V | PTE_R | PTE_W | PTE_G | PTE_A | PTE_D)
#define PROT_USER_RX (PTE_V | PTE_U | PTE_R | PTE_X | PTE_A | PTE_D)
#define PROT_USER_RW (PTE_V | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D)
#define PHYS_TO_VIRT_U(x) ((unsigned long)(x) + PAGE_OFFSET)
#define VIRT_TO_PHYS_U(x) ((unsigned long)(x) - PAGE_OFFSET)
#define MAX_ORDER 10
#define MAX_POOLS 8
#define MAX_ALLOC_SIZE (1UL << 20)
#define STARTUP_RESERVED_MAX 16
#define INITRD_BASE PHYS_TO_VIRT_U(0x0000000088200000UL)
#define STACK_SIZE PAGE_SIZE
#define TIME_FREQ 10000000UL
#define UART_IRQ 10UL
#define SCAUSE_INT (1UL << 63)
#define SCAUSE_ECALL_U 8UL
#define SCAUSE_TIMER 5UL
#define SCAUSE_EXT 9UL
#define SBI_EXT_TIME 0x54494D45UL
#define SBI_EXT_BASE 0x10UL
#define SBI_FID_SET_TIMER 0UL
#define SBI_FID_PROBE_EXT 3UL
#define PLIC_BASE PHYS_TO_VIRT_U(0x0c000000UL)
#define PLIC_PRIORITY(irq) (PLIC_BASE + (irq) * 4)
#define PLIC_ENABLE(hart) (PLIC_BASE + 0x002080 + (hart) * 0x100)
#define PLIC_THRESHOLD(hart) (PLIC_BASE + 0x201000 + (hart) * 0x2000)
#define PLIC_CLAIM(hart) (PLIC_BASE + 0x201004 + (hart) * 0x2000)

#define FB_BASE PHYS_TO_VIRT_U(0x87000000UL)
#define FB_WIDTH 1920
#define FB_HEIGHT 1080
#define FB_BPP 4

#define DTB_FATAL(msg)                                                           \
    do {                                                                         \
        uart_puts("[lab3][fatal] ");                                             \
        uart_puts(msg);                                                          \
        uart_puts("\n");                                                         \
        for (;;) {                                                               \
        }                                                                        \
    } while (0)

#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE 0x00000002u
#define FDT_PROP 0x00000003u
#define FDT_NOP 0x00000004u
#define FDT_END 0x00000009u

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

struct page {
    int order;
    int refcount;
    int state;
    int pool_idx;
    struct list_head node;
};

struct chunk_pool {
    size_t chunk_size;
    struct list_head free_chunks;
};

struct reserve_range {
    uintptr_t start;
    uintptr_t end;
};

enum page_state {
    PAGE_ALLOC = 0,
    PAGE_FREE_HEAD = 1,
    PAGE_FREE_TAIL = 2,
    PAGE_RESERVED = 3,
};

struct cpio_newc_header {
    char c_magic[6];
    char c_ino[8];
    char c_mode[8];
    char c_uid[8];
    char c_gid[8];
    char c_nlink[8];
    char c_mtime[8];
    char c_filesize[8];
    char c_devmajor[8];
    char c_devminor[8];
    char c_rdevmajor[8];
    char c_rdevminor[8];
    char c_namesize[8];
    char c_check[8];
};

struct pt_regs {
    unsigned long ra, sp, gp, tp, t0, t1, t2, s0, s1, a0, a1, a2, a3, a4, a5, a6, a7;
    unsigned long s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, t3, t4, t5, t6;
    unsigned long sepc, sstatus, scause, stval;
};

struct sbi_ret {
    long error;
    long value;
};

typedef void (*timer_callback_t)(void *arg);
typedef void (*task_callback_t)(void *arg);

#define MAX_TIMER_EVENTS 64
#define MAX_TASK_EVENTS 128
#define MAX_TIMEOUT_MSG 96

struct timer_event {
    int used;
    unsigned long expire_tick;
    timer_callback_t cb;
    void *arg;
    struct timer_event *next;
};

struct task_event {
    int used;
    int running;
    int priority;
    unsigned long seq;
    task_callback_t cb;
    void *arg;
    struct task_event *next;
};

struct timeout_message {
    int used;
    unsigned long created_sec;
    unsigned long expire_sec;
    char text[MAX_TIMEOUT_MSG];
};

#define TASK_UNUSED 0
#define TASK_RUNNABLE 1
#define TASK_ZOMBIE 2
#define TASK_WAITING 3
#define SIGTERM 15
#define MAX_MMAP_REGIONS 32

struct vm_region {
    int used;
    int prot;
    int flags;
    unsigned long start;
    unsigned long length;
};

struct task_struct {
    struct thread_struct {
        unsigned long ra;
        unsigned long sp;
        unsigned long s[12];
    } thread;
    int pid;
    int state;
    int is_user;
    int parent_pid;
    int wait_pid;
    int exit_status;
    unsigned long kernel_stack_base;
    unsigned long pgd;
    unsigned long user_code_size;
    unsigned long user_stack_base;
    unsigned long user_stack_size;
    unsigned long mmap_base_hint;
    struct vm_region mmap_regions[MAX_MMAP_REGIONS];
    unsigned long sig_handler;
    int sig_pending;
    int in_signal;
    unsigned long sig_stack_base;
    struct pt_regs sig_saved_regs;
    void (*entry)(void *);
    void *arg;
    struct task_struct *next;
    struct task_struct *all_next;
};

#define container_of(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type, member)))

uint32_t bswap32(uint32_t x);
const void *align_up_ptr(const void *ptr, size_t align);
uintptr_t align_up_ul(uintptr_t x, uintptr_t align);
struct sbi_ret sbi_call(unsigned long ext, unsigned long fid, unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3, unsigned long arg4,
                        unsigned long arg5);
unsigned long csr_time(void);
int hex_to_int(const char *s, int n);
int align_up_i(int n, int byte);
int c_memcmp(const void *s1, const void *s2, int n);
int c_strlen(const char *s);
int c_strcmp(const char *a, const char *b);
int c_strncmp(const char *a, const char *b, int n);
int c_atoi(const char *s, int *ok);
void c_strcpy_limit(char *dst, const char *src, int cap);
void trim_spaces(char *s);
int str_eq(const char *a, const char *b);
void uart_put_dec(unsigned long n);
void list_init(struct list_head *h);
int list_empty(const struct list_head *h);
void list_add_tail(struct list_head *n, struct list_head *h);
void list_add(struct list_head *n, struct list_head *h);
void list_del_init(struct list_head *n);

int fdt_is_valid(const void *fdt);
uint64_t fdt_read_be_u64(const void *p);
int fdt_path_offset(const void *fdt, const char *path);
const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp);
unsigned long probe_uart_base_from_fdt(const void *fdt);
int probe_initrd_region(const void *fdt, uintptr_t *start, uintptr_t *end);

void *allocate(size_t size);
void free(void *ptr);
void mm_init(const void *fdt);
void test_alloc_1(void);
int mm_ready(void);
int mm_page_get(unsigned long pa);
int mm_page_put(unsigned long pa);
int mm_page_refcount(unsigned long pa);

void add_task(task_callback_t cb, void *arg, int priority);
void add_timer(void (*callback)(void *), void *arg, int sec);
void timer_set_next(void);
void run_task_queue(void);
void plic_init(void);
void cmd_testtask(void);
void cmd_testpreempt(void);
int timeout_alloc_slot(void);
struct timeout_message *timeout_get_pool(void);
unsigned long boot_ticks(void);
void sched_on_timer_interrupt(void);
void sched_on_external_interrupt(void);
void sched_set_boot_hartid(unsigned long hartid);
void sched_set_time_ext(int has_ext);
void sched_timeout_schedule(struct timeout_message *m, int sec);

void switch_to(struct task_struct *prev, struct task_struct *next);
void thread_system_init(void);
int thread_create(void (*fn)(void *), void *arg);
void schedule(void);
void thread_exit(void) __attribute__((noreturn));
void kill_zombies(void);
struct task_struct *get_current(void);
struct task_struct *task_find_by_pid(int pid);
int task_create_user(unsigned long entry, unsigned long user_stack_base, unsigned long user_stack_size,
                     unsigned long pgd, unsigned long user_code_size, int parent_pid,
                     struct pt_regs *seed_regs);
void task_mark_exit(struct task_struct *task, int status);
void task_wake_waiter(int child_pid);
int waitpid_internal(int pid);
void cmd_testthread(void);

int exec_prog(const char *filename);
long syscall_dispatch(struct pt_regs *regs);
void signal_try_deliver(struct pt_regs *regs);
int handle_user_page_fault(unsigned long scause_code, unsigned long fault_addr);
int spawn_user_process(const char *filename, int parent_pid);
void video_init(void);
void video_bmp_display(unsigned int *bmp_image, int width, int height);
void shell(void);
void do_trap(struct pt_regs *regs);
void start_kernel(unsigned long hartid, const void *fdt);
void setup_vm(void);
void drop_identity_map(void);
unsigned long vm_kernel_pgd(void);
void vm_switch_pgd(unsigned long pgd);
unsigned long vm_user_pgd_create(void);
int vm_map_pages(unsigned long pgd, unsigned long va, unsigned long size, unsigned long pa,
                 unsigned long prot);
unsigned long vm_walk_pa(unsigned long pgd, unsigned long va);
unsigned long *vm_lookup_pte(unsigned long pgd, unsigned long va);
void vm_flush_tlb(void);
void vm_destroy_user_pgd(unsigned long pgd);

#endif
