#include "kernel/common.h"

uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8) | ((x & 0xff000000u) >> 24);
}

const void *align_up_ptr(const void *ptr, size_t align) {
    return (const void *)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

uintptr_t align_up_ul(uintptr_t x, uintptr_t align) {
    return (x + align - 1) & ~(align - 1);
}

struct sbi_ret sbi_call(unsigned long ext, unsigned long fid, unsigned long arg0, unsigned long arg1,
                        unsigned long arg2, unsigned long arg3, unsigned long arg4,
                        unsigned long arg5) {
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a3 asm("a3") = arg3;
    register unsigned long a4 asm("a4") = arg4;
    register unsigned long a5 asm("a5") = arg5;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = ext;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
                 : "memory");
    struct sbi_ret ret = {(long)a0, (long)a1};
    return ret;
}

unsigned long csr_time(void) {
    unsigned long t;
    asm volatile("rdtime %0" : "=r"(t));
    return t;
}

int hex_to_int(const char *s, int n) {
    int r = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        r <<= 4;
        if (c >= '0' && c <= '9') {
            r += c - '0';
        } else if (c >= 'a' && c <= 'f') {
            r += c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            r += c - 'A' + 10;
        }
    }
    return r;
}

int align_up_i(int n, int byte) {
    return (n + byte - 1) & ~(byte - 1);
}

int c_memcmp(const void *s1, const void *s2, int n) {
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }
    return 0;
}

int c_strlen(const char *s) {
    int n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int c_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int c_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        if (a[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

int c_atoi(const char *s, int *ok) {
    int v = 0;
    if (!s || *s == '\0') {
        *ok = 0;
        return 0;
    }
    while (*s) {
        if (*s < '0' || *s > '9') {
            *ok = 0;
            return 0;
        }
        v = v * 10 + (*s - '0');
        s++;
    }
    *ok = 1;
    return v;
}

void c_strcpy_limit(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) {
        return;
    }
    while (src[i] && i < cap - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void trim_spaces(char *s) {
    int n = c_strlen(s);
    int i = 0;
    while (s[i] == ' ') {
        i++;
    }
    if (i > 0) {
        int j = 0;
        while (s[i]) {
            s[j++] = s[i++];
        }
        s[j] = '\0';
        n = j;
    }
    while (n > 0 && s[n - 1] == ' ') {
        s[--n] = '\0';
    }
}

int str_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

void uart_put_dec(unsigned long n) {
    char tmp[32];
    int i = 0;
    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n) {
        tmp[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        uart_putc(tmp[--i]);
    }
}

void list_init(struct list_head *h) {
    h->next = h;
    h->prev = h;
}

int list_empty(const struct list_head *h) {
    return h->next == h;
}

void list_add_tail(struct list_head *n, struct list_head *h) {
    n->prev = h->prev;
    n->next = h;
    h->prev->next = n;
    h->prev = n;
}

void list_add(struct list_head *n, struct list_head *h) {
    n->next = h->next;
    n->prev = h;
    h->next->prev = n;
    h->next = n;
}

void list_del_init(struct list_head *n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    list_init(n);
}

int fdt_is_valid(const void *fdt) {
    if (!fdt) {
        return 0;
    }
    return bswap32(((const struct fdt_header *)fdt)->magic) == 0xd00dfeedu;
}

uint64_t fdt_read_be_u64(const void *p) {
    const uint32_t *w = (const uint32_t *)p;
    uint64_t hi = (uint64_t)bswap32(w[0]);
    uint64_t lo = (uint64_t)bswap32(w[1]);
    return (hi << 32) | lo;
}

int fdt_path_offset(const void *fdt, const char *path) {
    if (!fdt || !path || path[0] != '/') return -1;
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    if (!fdt_is_valid(fdt)) return -1;
    const char *comps[32];
    int comp_len[32], ncomp = 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p && ncomp < 32) {
        comps[ncomp] = p;
        int n = 0;
        while (p[n] && p[n] != '/') n++;
        comp_len[ncomp++] = n;
        p += n;
        while (*p == '/') p++;
    }
    const uint8_t *st = (const uint8_t *)fdt + bswap32(hdr->off_dt_struct);
    const uint8_t *q = st;
    const char *stack[32];
    int sp = 0;
    for (;;) {
        uint32_t token = bswap32(*(const uint32_t *)q);
        const uint8_t *token_ptr = q;
        q += 4;
        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)q;
            q += (size_t)c_strlen(name) + 1;
            q = (const uint8_t *)align_up_ptr(q, 4);
            if (sp >= 32) return -1;
            stack[sp++] = name;
            if (sp == 1) continue;
            if (sp - 1 == ncomp) {
                int ok = 1;
                for (int i = 0; i < ncomp; i++) {
                    const char *nn = stack[i + 1];
                    int nlen = c_strlen(nn);
                    int c_has_at = 0;
                    for (int j = 0; j < comp_len[i]; j++) {
                        if (comps[i][j] == '@') c_has_at = 1;
                    }
                    if (!c_has_at) {
                        int j = 0;
                        while (j < nlen && nn[j] != '@') j++;
                        nlen = j;
                    }
                    if (nlen != comp_len[i] || c_strncmp(nn, comps[i], nlen) != 0) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) return (int)(token_ptr - st);
            }
        } else if (token == FDT_END_NODE) {
            if (sp > 0) sp--;
        } else if (token == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t *)q);
            q += 8 + len;
            q = (const uint8_t *)align_up_ptr(q, 4);
        } else if (token == FDT_NOP) {
        } else if (token == FDT_END) {
            break;
        } else {
            return -1;
        }
    }
    return -1;
}

const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp) {
    if (!fdt_is_valid(fdt)) return NULL;
    const struct fdt_header *hdr = (const struct fdt_header *)fdt;
    const uint8_t *st = (const uint8_t *)fdt + bswap32(hdr->off_dt_struct);
    const uint8_t *strs = (const uint8_t *)fdt + bswap32(hdr->off_dt_strings);
    const uint8_t *q = st + (unsigned int)nodeoffset;
    int in_target = 0;
    for (;;) {
        uint32_t token = bswap32(*(const uint32_t *)q);
        const uint8_t *token_ptr = q;
        q += 4;
        if (token == FDT_BEGIN_NODE) {
            const char *node = (const char *)q;
            q += (size_t)c_strlen(node) + 1;
            q = (const uint8_t *)align_up_ptr(q, 4);
            if ((int)(token_ptr - st) == nodeoffset) in_target = 1;
        } else if (token == FDT_END_NODE) {
            if (in_target) return NULL;
        } else if (token == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t *)q);
            q += 4;
            uint32_t nameoff = bswap32(*(const uint32_t *)q);
            q += 4;
            const void *val = (const void *)q;
            q += len;
            q = (const uint8_t *)align_up_ptr(q, 4);
            if (in_target) {
                const char *pname = (const char *)(strs + nameoff);
                if (c_strcmp(pname, name) == 0) {
                    if (lenp) *lenp = (int)len;
                    return val;
                }
            }
        } else if (token == FDT_NOP) {
        } else if (token == FDT_END) {
            return NULL;
        } else {
            return NULL;
        }
    }
}

unsigned long probe_uart_base_from_fdt(const void *fdt) {
    if (!fdt_is_valid(fdt)) return 0;
    int chosen = fdt_path_offset(fdt, "/chosen");
    const char *stdout_path = NULL;
    int len = 0;
    if (chosen >= 0) stdout_path = (const char *)fdt_getprop(fdt, chosen, "stdout-path", &len);
    if (!stdout_path || len <= 0) return 0;
    char token[128];
    int n = 0;
    while (n < len && n < (int)sizeof(token) - 1 && stdout_path[n] && stdout_path[n] != ':') {
        token[n] = stdout_path[n];
        n++;
    }
    token[n] = '\0';
    if (n == 0) return 0;
    const char *node_path = NULL;
    if (token[0] == '/') {
        node_path = token;
    } else {
        int aliases = fdt_path_offset(fdt, "/aliases");
        if (aliases >= 0) {
            const char *alias = (const char *)fdt_getprop(fdt, aliases, token, &len);
            if (alias && len > 0) node_path = alias;
        }
    }
    if (!node_path) return 0;
    int off = fdt_path_offset(fdt, node_path);
    if (off < 0) return 0;
    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, off, "reg", &len);
    if (!reg || len < 8) return 0;
    uint64_t base = ((uint64_t)bswap32(reg[0]) << 32) | (uint64_t)bswap32(reg[1]);
    return (unsigned long)base;
}

int probe_initrd_region(const void *fdt, uintptr_t *start, uintptr_t *end) {
    if (!fdt_is_valid(fdt)) return -1;
    int chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0) return -1;
    int ls = 0, le = 0;
    const uint32_t *ps = (const uint32_t *)fdt_getprop(fdt, chosen, "linux,initrd-start", &ls);
    const uint32_t *pe = (const uint32_t *)fdt_getprop(fdt, chosen, "linux,initrd-end", &le);
    if (!ps || !pe || ls < 8 || le < 8) return -1;
    *start = (uintptr_t)fdt_read_be_u64(ps);
    *end = (uintptr_t)fdt_read_be_u64(pe);
    return *end > *start ? 0 : -1;
}
