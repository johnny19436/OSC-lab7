#include "kernel/common.h"

static uintptr_t g_mm_base;
static uintptr_t g_mm_end;
static size_t g_page_count;
static struct page *g_pages;
static struct list_head g_free_area[MAX_ORDER + 1];
static struct chunk_pool g_pools[MAX_POOLS];
static int g_mm_ready_flag;
static struct reserve_range g_startup_reserved[STARTUP_RESERVED_MAX];
static int g_startup_reserved_nr;
static uintptr_t g_startup_cur;
static uintptr_t g_startup_end;
static const void *g_fdt_ptr;

static unsigned long irq_save(void) {
    unsigned long sstatus = 0;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrci sstatus, (1 << 1)");
    return sstatus;
}

static void irq_restore(unsigned long sstatus) {
    if (sstatus & (1UL << 1)) asm volatile("csrsi sstatus, (1 << 1)");
}

static void memory_reserve(uintptr_t base, size_t size);

static void startup_reserve_add(uintptr_t start, uintptr_t end) {
    if (start >= end || g_startup_reserved_nr >= STARTUP_RESERVED_MAX) return;
    g_startup_reserved[g_startup_reserved_nr].start = start;
    g_startup_reserved[g_startup_reserved_nr].end = end;
    g_startup_reserved_nr++;
}

static int startup_overlap(uintptr_t s, uintptr_t e, uintptr_t *ov_end) {
    for (int i = 0; i < g_startup_reserved_nr; i++) {
        uintptr_t rs = g_startup_reserved[i].start;
        uintptr_t re = g_startup_reserved[i].end;
        if (e <= rs || s >= re) continue;
        *ov_end = re;
        return 1;
    }
    return 0;
}

static void startup_init(uintptr_t base, uintptr_t size) {
    g_startup_cur = align_up_ul(base, PAGE_SIZE);
    g_startup_end = base + size;
}

static void *startup_alloc_aligned(size_t size, size_t align) {
    uintptr_t p = align_up_ul(g_startup_cur, (uintptr_t)align);
    while (p + size <= g_startup_end) {
        uintptr_t ov_end = 0;
        if (!startup_overlap(p, p + size, &ov_end)) {
            g_startup_cur = p + size;
            return (void *)PHYS_TO_VIRT_U(p);
        }
        p = align_up_ul(ov_end, (uintptr_t)align);
    }
    return NULL;
}

static inline uintptr_t page_to_addr(size_t idx) { return g_mm_base + idx * PAGE_SIZE; }
static inline size_t addr_to_pfn(uintptr_t addr) { return (size_t)((addr - g_mm_base) / PAGE_SIZE); }
static void log_block(const char *tag, size_t idx, unsigned int order) { (void)tag; (void)idx; (void)order; }

static void add_free_block(size_t idx, unsigned int order) {
    struct page *p = &g_pages[idx];
    size_t cnt = 1UL << order;
    p->order = (int)order;
    p->refcount = 0;
    p->state = PAGE_FREE_HEAD;
    p->pool_idx = -1;
    list_add_tail(&p->node, &g_free_area[order]);
    for (size_t i = 1; i < cnt && idx + i < g_page_count; i++) {
        g_pages[idx + i].order = -1;
        g_pages[idx + i].refcount = 0;
        g_pages[idx + i].state = PAGE_FREE_TAIL;
        g_pages[idx + i].pool_idx = -1;
    }
    log_block("[+] add", idx, order);
}

static void remove_free_block(struct page *p, unsigned int order) {
    size_t idx = (size_t)(p - g_pages);
    list_del_init(&p->node);
    for (size_t i = 0; i < (1UL << order) && idx + i < g_page_count; i++) {
        g_pages[idx + i].state = PAGE_ALLOC;
        g_pages[idx + i].pool_idx = -1;
    }
    log_block("[-] del", idx, order);
}

static struct page *get_buddy(struct page *page, unsigned int order) {
    size_t idx = (size_t)(page - g_pages);
    size_t buddy_idx = idx ^ (1UL << order);
    if (buddy_idx >= g_page_count) return NULL;
    return &g_pages[buddy_idx];
}

static struct page *alloc_pages(unsigned int order) {
    if (!g_mm_ready_flag || order > MAX_ORDER) return NULL;
    unsigned int cur = order;
    while (cur <= MAX_ORDER && list_empty(&g_free_area[cur])) cur++;
    if (cur > MAX_ORDER) return NULL;
    struct page *p = container_of(g_free_area[cur].next, struct page, node);
    remove_free_block(p, cur);
    size_t idx = (size_t)(p - g_pages);
    while (cur > order) {
        cur--;
        size_t buddy_idx = idx + (1UL << cur);
        add_free_block(buddy_idx, cur);
        log_block("[*] split", idx, cur);
    }
    p->order = (int)order;
    p->refcount = 1;
    p->state = PAGE_ALLOC;
    p->pool_idx = -1;
    return p;
}

static void free_pages(struct page *p) {
    if (!p || !g_mm_ready_flag) return;
    size_t idx = (size_t)(p - g_pages);
    unsigned int order = (unsigned int)p->order;
    p->refcount = 0;
    p->pool_idx = -1;
    while (order < MAX_ORDER) {
        struct page *buddy = get_buddy(&g_pages[idx], order);
        if (!buddy) break;
        size_t bidx = (size_t)(buddy - g_pages);
        if (buddy->state != PAGE_FREE_HEAD || buddy->order != (int)order) break;
        remove_free_block(buddy, order);
        if (bidx < idx) idx = bidx;
        order++;
    }
    add_free_block(idx, order);
}

static void mark_reserved_block(size_t idx, unsigned int order) {
    size_t cnt = 1UL << order;
    for (size_t i = 0; i < cnt && idx + i < g_page_count; i++) {
        g_pages[idx + i].state = PAGE_RESERVED;
        g_pages[idx + i].refcount = 1;
        g_pages[idx + i].order = -1;
        g_pages[idx + i].pool_idx = -1;
    }
}

static void memory_reserve(uintptr_t base, size_t size) {
    if (!g_mm_ready_flag || size == 0) return;
    uintptr_t rs = base, re = base + size;
    if (re <= g_mm_base || rs >= g_mm_end) return;
    if (rs < g_mm_base) rs = g_mm_base;
    if (re > g_mm_end) re = g_mm_end;
    size_t start_pfn = addr_to_pfn(rs);
    size_t end_pfn = addr_to_pfn(align_up_ul(re, PAGE_SIZE));
    for (int order = MAX_ORDER; order >= 0; order--) {
        struct list_head *head = &g_free_area[order];
        struct list_head *it = head->next;
        while (it != head) {
            struct list_head *next = it->next;
            struct page *p = container_of(it, struct page, node);
            size_t idx = (size_t)(p - g_pages);
            size_t block = 1UL << order;
            size_t bs = idx, be = idx + block;
            if (be <= start_pfn || bs >= end_pfn) {
                it = next;
                continue;
            }
            remove_free_block(p, (unsigned int)order);
            if (start_pfn <= bs && be <= end_pfn) {
                mark_reserved_block(idx, (unsigned int)order);
            } else if (order == 0) {
                mark_reserved_block(idx, 0);
            } else {
                add_free_block(idx, (unsigned int)(order - 1));
                add_free_block(idx + (1UL << (order - 1)), (unsigned int)(order - 1));
            }
            it = next;
        }
    }
}

static unsigned int next_pow2_order(unsigned long pages) {
    unsigned int order = 0;
    unsigned long n = 1;
    while (n < pages) {
        n <<= 1;
        order++;
    }
    return order;
}

static void init_pools(void) {
    static const size_t pool_sizes[MAX_POOLS] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    for (int i = 0; i < MAX_POOLS; i++) {
        g_pools[i].chunk_size = pool_sizes[i];
        list_init(&g_pools[i].free_chunks);
    }
}

static int pool_for_size(size_t size) {
    for (int i = 0; i < MAX_POOLS; i++) {
        if (size <= g_pools[i].chunk_size) return i;
    }
    return -1;
}

static void refill_pool(int pool_idx) {
    struct page *p = alloc_pages(0);
    if (!p) return;
    size_t idx = (size_t)(p - g_pages);
    p->pool_idx = pool_idx;
    uintptr_t base = PHYS_TO_VIRT_U(page_to_addr(idx));
    size_t chunk = g_pools[pool_idx].chunk_size;
    for (size_t off = 0; off + chunk <= PAGE_SIZE; off += chunk) {
        struct list_head *node = (struct list_head *)(base + off);
        list_init(node);
        list_add(node, &g_pools[pool_idx].free_chunks);
    }
}

void *allocate(size_t size) {
    unsigned long sstatus = irq_save();
    if (!g_mm_ready_flag || size == 0 || size > MAX_ALLOC_SIZE) {
        irq_restore(sstatus);
        return NULL;
    }
    if (size < PAGE_SIZE) {
        int pool_idx = pool_for_size(size);
        if (pool_idx < 0) {
            irq_restore(sstatus);
            return NULL;
        }
        if (list_empty(&g_pools[pool_idx].free_chunks)) {
            refill_pool(pool_idx);
            if (list_empty(&g_pools[pool_idx].free_chunks)) {
                irq_restore(sstatus);
                return NULL;
            }
        }
        struct list_head *n = g_pools[pool_idx].free_chunks.next;
        list_del_init(n);
        irq_restore(sstatus);
        return (void *)n;
    }
    unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned int order = next_pow2_order(pages);
    struct page *p = alloc_pages(order);
    if (!p) {
        irq_restore(sstatus);
        return NULL;
    }
    p->pool_idx = -1;
    void *ret = (void *)PHYS_TO_VIRT_U(page_to_addr((size_t)(p - g_pages)));
    irq_restore(sstatus);
    return ret;
}

void free(void *ptr) {
    unsigned long sstatus = irq_save();
    if (!ptr || !g_mm_ready_flag) {
        irq_restore(sstatus);
        return;
    }
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t addr_phys = (addr >= PAGE_OFFSET) ? VIRT_TO_PHYS_U(addr) : addr;
    if (addr_phys < g_mm_base || addr_phys >= g_mm_end) {
        irq_restore(sstatus);
        return;
    }
    size_t idx = addr_to_pfn(addr_phys);
    struct page *p = &g_pages[idx];
    if (p->pool_idx >= 0) {
        uintptr_t page_base = PHYS_TO_VIRT_U(page_to_addr(idx));
        size_t chunk = g_pools[p->pool_idx].chunk_size;
        if (((addr - page_base) % chunk) != 0) {
            irq_restore(sstatus);
            return;
        }
        list_add((struct list_head *)ptr, &g_pools[p->pool_idx].free_chunks);
        irq_restore(sstatus);
        return;
    }
    free_pages(p);
    irq_restore(sstatus);
}

static int probe_memory_region(const void *fdt, uintptr_t *base, uintptr_t *size) {
    if (!fdt_is_valid(fdt)) return -1;
    int off = fdt_path_offset(fdt, "/memory");
    if (off < 0) return -1;
    int len = 0;
    const uint32_t *reg = (const uint32_t *)fdt_getprop(fdt, off, "reg", &len);
    if (!reg || len < 16) return -1;
    *base = (uintptr_t)fdt_read_be_u64(reg);
    *size = (uintptr_t)fdt_read_be_u64(reg + 2);
    return 0;
}

static void reserve_from_reserved_memory_node(const void *fdt) {
    int rsv = fdt_path_offset(fdt, "/reserved-memory");
    if (rsv < 0) return;
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    const uint8_t *q = (const uint8_t *)fdt + bswap32(h->off_dt_struct) + (unsigned int)rsv;
    int depth = 0;
    for (;;) {
        uint32_t token = bswap32(*(const uint32_t *)q);
        q += 4;
        if (token == FDT_BEGIN_NODE) {
            q += (size_t)c_strlen((const char *)q) + 1;
            q = (const uint8_t *)align_up_ptr(q, 4);
            depth++;
            continue;
        }
        if (token == FDT_END_NODE) {
            if (depth == 0) break;
            depth--;
            continue;
        }
        if (token == FDT_PROP) {
            uint32_t len = bswap32(*(const uint32_t *)q);
            q += 4;
            uint32_t nameoff = bswap32(*(const uint32_t *)q);
            q += 4;
            const void *val = (const void *)q;
            q += len;
            q = (const uint8_t *)align_up_ptr(q, 4);
            if (depth == 2) {
                const uint8_t *strs = (const uint8_t *)fdt + bswap32(h->off_dt_strings);
                const char *pname = (const char *)(strs + nameoff);
                if (str_eq(pname, "reg") && len >= 16) {
                    const uint32_t *reg = (const uint32_t *)val;
                    uintptr_t base = (uintptr_t)fdt_read_be_u64(reg);
                    uintptr_t size = (uintptr_t)fdt_read_be_u64(reg + 2);
                    if (size) memory_reserve(base, size);
                }
            }
            continue;
        }
        if (token == FDT_NOP) continue;
        if (token == FDT_END) break;
        break;
    }
}

static void buddy_init(uintptr_t base, uintptr_t size) {
    g_mm_base = align_up_ul(base, PAGE_SIZE);
    g_mm_end = (base + size) & ~(PAGE_SIZE - 1UL);
    if (g_mm_end <= g_mm_base) {
        g_mm_ready_flag = 0;
        return;
    }
    g_page_count = (g_mm_end - g_mm_base) / PAGE_SIZE;
    g_pages = (struct page *)startup_alloc_aligned(g_page_count * sizeof(struct page), PAGE_SIZE);
    if (!g_pages) {
        g_mm_ready_flag = 0;
        DTB_FATAL("frame array allocation failed");
        return;
    }
    for (unsigned int i = 0; i <= MAX_ORDER; i++) list_init(&g_free_area[i]);
    for (size_t i = 0; i < g_page_count; i++) {
        g_pages[i].order = -1;
        g_pages[i].refcount = 1;
        g_pages[i].state = PAGE_RESERVED;
        g_pages[i].pool_idx = -1;
        list_init(&g_pages[i].node);
    }
    size_t idx = 0;
    while (idx < g_page_count) {
        unsigned int order = MAX_ORDER;
        while (order > 0) {
            size_t block = 1UL << order;
            if ((idx & (block - 1)) == 0 && idx + block <= g_page_count) break;
            order--;
        }
        add_free_block(idx, order);
        idx += 1UL << order;
    }
    g_mm_ready_flag = 1;
    init_pools();
}

static void reserve_boot_regions(void) {
    if (fdt_is_valid(g_fdt_ptr)) {
        const struct fdt_header *h = (const struct fdt_header *)g_fdt_ptr;
        uintptr_t dtb_start = (uintptr_t)g_fdt_ptr;
        if (dtb_start >= PAGE_OFFSET) dtb_start = VIRT_TO_PHYS_U(dtb_start);
        uintptr_t dtb_size = (uintptr_t)bswap32(h->totalsize);
        memory_reserve(dtb_start, dtb_size);
    }
    uintptr_t kstart = (uintptr_t)&__kernel_start;
    uintptr_t kend = (uintptr_t)&__kernel_end;
    if (kstart >= PAGE_OFFSET) kstart = VIRT_TO_PHYS_U(kstart);
    if (kend >= PAGE_OFFSET) kend = VIRT_TO_PHYS_U(kend);
    if (kend > kstart) memory_reserve(kstart, kend - kstart);
    uintptr_t initrd_s = 0, initrd_e = 0;
    if (probe_initrd_region(g_fdt_ptr, &initrd_s, &initrd_e) == 0) {
        memory_reserve(initrd_s, initrd_e - initrd_s);
    }
    reserve_from_reserved_memory_node(g_fdt_ptr);
}

static void startup_prepare_reservations(const void *fdt) {
    g_startup_reserved_nr = 0;
    const struct fdt_header *h = (const struct fdt_header *)fdt;
    if (fdt_is_valid(fdt)) {
        uintptr_t dtb_s = (uintptr_t)fdt;
        if (dtb_s >= PAGE_OFFSET) dtb_s = VIRT_TO_PHYS_U(dtb_s);
        uintptr_t dtb_e = dtb_s + (uintptr_t)bswap32(h->totalsize);
        startup_reserve_add(dtb_s, dtb_e);
    }
    uintptr_t kstart = (uintptr_t)&__kernel_start;
    uintptr_t kend = (uintptr_t)&__kernel_end;
    if (kstart >= PAGE_OFFSET) kstart = VIRT_TO_PHYS_U(kstart);
    if (kend >= PAGE_OFFSET) kend = VIRT_TO_PHYS_U(kend);
    startup_reserve_add(kstart, kend);
    uintptr_t initrd_s = 0, initrd_e = 0;
    if (probe_initrd_region(fdt, &initrd_s, &initrd_e) == 0) startup_reserve_add(initrd_s, initrd_e);
}

void mm_init(const void *fdt) {
    g_fdt_ptr = fdt;
    uintptr_t mem_base = 0, mem_size = 0;
    if (probe_memory_region(fdt, &mem_base, &mem_size) < 0) DTB_FATAL("cannot parse /memory reg from DTB");
    startup_prepare_reservations(fdt);
    uintptr_t mem_end = mem_base + mem_size;
    uintptr_t safe_start = mem_base;
    for (int i = 0; i < g_startup_reserved_nr; i++) {
        if (g_startup_reserved[i].end > safe_start) safe_start = g_startup_reserved[i].end;
    }
    safe_start = align_up_ul(safe_start, PAGE_SIZE);
    if (safe_start >= mem_end) DTB_FATAL("no usable memory after reserved regions");
    startup_init(safe_start, mem_end - safe_start);
    buddy_init(safe_start, mem_end - safe_start);
    if (g_startup_cur > safe_start) {
        memory_reserve(safe_start, g_startup_cur - safe_start);
    }
    reserve_boot_regions();
}

void test_alloc_1(void) {
    char *ptr1 = (char *)allocate(4000);
    char *ptr2 = (char *)allocate(8000);
    char *ptr3 = (char *)allocate(4000);
    char *ptr4 = (char *)allocate(4000);
    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);
}

int mm_ready(void) {
    return g_mm_ready_flag;
}

int mm_page_get(unsigned long pa) {
    unsigned long sstatus = irq_save();
    if (!g_mm_ready_flag || pa < g_mm_base || pa >= g_mm_end) {
        irq_restore(sstatus);
        return -1;
    }
    size_t idx = addr_to_pfn(pa & ~(PAGE_SIZE - 1UL));
    struct page *p = &g_pages[idx];
    if (p->state == PAGE_RESERVED || p->order != 0) {
        irq_restore(sstatus);
        return -1;
    }
    p->refcount++;
    irq_restore(sstatus);
    return p->refcount;
}

int mm_page_put(unsigned long pa) {
    unsigned long sstatus = irq_save();
    if (!g_mm_ready_flag || pa < g_mm_base || pa >= g_mm_end) {
        irq_restore(sstatus);
        return -1;
    }
    size_t idx = addr_to_pfn(pa & ~(PAGE_SIZE - 1UL));
    struct page *p = &g_pages[idx];
    if (p->state == PAGE_RESERVED || p->order != 0 || p->refcount <= 0) {
        irq_restore(sstatus);
        return -1;
    }
    p->refcount--;
    int ref = p->refcount;
    if (p->refcount == 0) {
        free_pages(p);
        ref = 0;
    }
    irq_restore(sstatus);
    return ref;
}

int mm_page_refcount(unsigned long pa) {
    unsigned long sstatus = irq_save();
    if (!g_mm_ready_flag || pa < g_mm_base || pa >= g_mm_end) {
        irq_restore(sstatus);
        return -1;
    }
    size_t idx = addr_to_pfn(pa & ~(PAGE_SIZE - 1UL));
    int ref = g_pages[idx].refcount;
    irq_restore(sstatus);
    return ref;
}
