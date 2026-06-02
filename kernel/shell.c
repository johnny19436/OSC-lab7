#include "kernel/common.h"

static unsigned int read_u32_le(void) {
    unsigned int v = 0;
    v |= (unsigned int)(unsigned char)uart_getc();
    v |= (unsigned int)(unsigned char)uart_getc() << 8;
    v |= (unsigned int)(unsigned char)uart_getc() << 16;
    v |= (unsigned int)(unsigned char)uart_getc() << 24;
    return v;
}

static void recv_bytes(unsigned char *dst, unsigned long n) {
    for (unsigned long i = 0; i < n; i++) dst[i] = (unsigned char)uart_getc();
}

static void do_load(void) {
    uart_puts("Waiting for BOOT header...\n");
    unsigned int magic = read_u32_le();
    unsigned int size = read_u32_le();
    if (magic != BOOT_MAGIC_LE) return;
    if ((unsigned long)size == 0 || (unsigned long)size > MAX_KERNEL_SZ) return;
    recv_bytes((unsigned char *)LOAD_ADDR, (unsigned long)size);
    ((void (*)(void))LOAD_ADDR)();
}

int exec_prog(const char *filename) {
    struct task_struct *cur = get_current();
    int ppid = cur ? cur->pid : 0;
    int pid = spawn_user_process(filename, ppid);
    if (pid < 0) return -1;
    return waitpid_internal(pid) < 0 ? -1 : 0;
}

static void shell_print_help(void) {
    uart_puts("help   - show commands\n");
    uart_puts("load   - load kernel over uart\n");
    uart_puts("exec   - execute user program from initramfs\n");
}

static void cmd_settimeout(const char *args) {
    if (!args || *args == '\0') {
        uart_puts("Usage: settimeout SECONDS MESSAGE\n");
        return;
    }
    while (*args == ' ') args++;
    char secbuf[16];
    int si = 0;
    while (*args && *args != ' ' && si < (int)sizeof(secbuf) - 1) secbuf[si++] = *args++;
    secbuf[si] = '\0';
    while (*args == ' ') args++;
    if (*args == '\0') {
        uart_puts("Usage: settimeout SECONDS MESSAGE\n");
        return;
    }
    int ok = 0;
    int sec = c_atoi(secbuf, &ok);
    if (!ok || sec < 0) return;
    int slot = timeout_alloc_slot();
    if (slot < 0) return;
    struct timeout_message *pool = timeout_get_pool();
    struct timeout_message *m = &pool[slot];
    m->created_sec = boot_ticks();
    m->expire_sec = boot_ticks() + (unsigned long)sec;
    c_strcpy_limit(m->text, args, MAX_TIMEOUT_MSG);
    sched_timeout_schedule(m, sec);
}

void shell(void) {
    char buf[128];
    char *args;
    for (;;) {
        int len = 0;
        uart_puts("lab6> ");
        for (;;) {
            char c = uart_getc();
            if (c == '\b' || c == 127) {
                if (len > 0) {
                    len--;
                    uart_putc('\b');
                    uart_putc(' ');
                    uart_putc('\b');
                }
                continue;
            }
            if (c == '\n') {
                uart_putc('\n');
                buf[len] = '\0';
                break;
            }
            if (len < (int)sizeof(buf) - 1) {
                buf[len++] = c;
                uart_putc(c);
            }
        }
        if (len == 0) continue;
        trim_spaces(buf);
        if (buf[0] == '\0') continue;
        args = buf;
        while (*args && *args != ' ') args++;
        if (*args) {
            *args++ = '\0';
            while (*args == ' ') args++;
        }
        if (str_eq(buf, "help")) shell_print_help();
        else if (str_eq(buf, "load")) do_load();
        else if (str_eq(buf, "testmm")) test_alloc_1();
        else if (str_eq(buf, "testtask")) cmd_testtask();
        else if (str_eq(buf, "testpreempt")) cmd_testpreempt();
        else if (str_eq(buf, "testthread")) cmd_testthread();
        else if (str_eq(buf, "exec")) {
            if (*args == '\0') uart_puts("Usage: exec FILENAME\n");
            else if (exec_prog(args) < 0) uart_puts("exec failed\n");
        } else if (str_eq(buf, "settimeout")) {
            if (*args == '\0') uart_puts("Usage: settimeout SECONDS MESSAGE\n");
            else cmd_settimeout(args);
        } else uart_puts("Unknown command\n");
        kill_zombies();
        schedule();
    }
}
