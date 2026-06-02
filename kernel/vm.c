#include "kernel/common.h"

#define PMD_SIZE (1UL << 21)
#define PGD_SIZE (1UL << 30)
#define ENTRIES_PER_TABLE 512
#define LINEAR_MAP_GIB 4
#define PTE_LEAF_MASK (PTE_R | PTE_W | PTE_X)

#define MAKE_PTE(pa, flags) ((((unsigned long)(pa)) >> 12) << 10 | (flags))
#define PTE_PA(pte) (((pte) >> 10) << 12)

static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    g_kernel_pgd[ENTRIES_PER_TABLE];

static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    g_linear_pmd[LINEAR_MAP_GIB][ENTRIES_PER_TABLE];

static unsigned long __attribute__((section(".data"), aligned(PAGE_SIZE)))
    g_mmio_pmd[ENTRIES_PER_TABLE];

static unsigned long to_phys_addr(unsigned long x) {
    return (x >= PAGE_OFFSET) ? VIRT_TO_PHYS_U(x) : x;
}

static unsigned long *pte_to_virt_table(unsigned long pte) {
    return (unsigned long *)PHYS_TO_VIRT_U(PTE_PA(pte));
}

static unsigned long *walk_create(unsigned long *root, unsigned long va) {
    unsigned long idx2 = (va >> 30) & 0x1ff;
    unsigned long idx1 = (va >> 21) & 0x1ff;
    if ((root[idx2] & PTE_V) == 0) {
        unsigned long *new_l1 = (unsigned long *)allocate(PAGE_SIZE);
        if (!new_l1) return NULL;
        for (unsigned long i = 0; i < ENTRIES_PER_TABLE; i++) new_l1[i] = 0;
        root[idx2] = MAKE_PTE(to_phys_addr((unsigned long)new_l1), PTE_V);
    } else if (root[idx2] & PTE_LEAF_MASK) {
        return NULL;
    }
    unsigned long *l1 = pte_to_virt_table(root[idx2]);
    if ((l1[idx1] & PTE_V) == 0) {
        unsigned long *new_l0 = (unsigned long *)allocate(PAGE_SIZE);
        if (!new_l0) return NULL;
        for (unsigned long i = 0; i < ENTRIES_PER_TABLE; i++) new_l0[i] = 0;
        l1[idx1] = MAKE_PTE(to_phys_addr((unsigned long)new_l0), PTE_V);
    } else if (l1[idx1] & PTE_LEAF_MASK) {
        return NULL;
    }
    return pte_to_virt_table(l1[idx1]);
}

static void map_mmio_2m(unsigned long pa) {
    unsigned long va = PHYS_TO_VIRT_U(pa);
    unsigned long idx = (va >> 21) & 0x1ff;
    g_mmio_pmd[idx] = MAKE_PTE(pa & ~(PMD_SIZE - 1UL), PROT_MMIO);
}

void setup_vm(void) {
    const unsigned long ram_base = 0x80000000UL;
    const unsigned long low_ram_pgd = (ram_base >> 30) & 0x1ff;
    const unsigned long high_ram_pgd = (PHYS_TO_VIRT_U(ram_base) >> 30) & 0x1ff;
    const unsigned long high_mmio_pgd = (PAGE_OFFSET >> 30) & 0x1ff;

    for (unsigned long i = 0; i < LINEAR_MAP_GIB; i++) {
        for (unsigned long j = 0; j < ENTRIES_PER_TABLE; j++) {
            unsigned long pa = ram_base + i * PGD_SIZE + j * PMD_SIZE;
            g_linear_pmd[i][j] = MAKE_PTE(pa, PROT_KERNEL);
        }
        g_kernel_pgd[low_ram_pgd + i] = MAKE_PTE(to_phys_addr((unsigned long)&g_linear_pmd[i][0]), PTE_V);
        g_kernel_pgd[high_ram_pgd + i] = MAKE_PTE(to_phys_addr((unsigned long)&g_linear_pmd[i][0]), PTE_V);
    }

    map_mmio_2m(0x0c000000UL);
    map_mmio_2m(0x0c200000UL);
    map_mmio_2m(0x10000000UL);
    map_mmio_2m(0x10100000UL);
    g_kernel_pgd[high_mmio_pgd] = MAKE_PTE(to_phys_addr((unsigned long)g_mmio_pmd), PTE_V);

    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP(to_phys_addr((unsigned long)g_kernel_pgd)))
        : "memory");
}

void drop_identity_map(void) {
    const unsigned long low_ram_pgd = (0x80000000UL >> 30) & 0x1ff;
    for (unsigned long i = 0; i < LINEAR_MAP_GIB; i++) {
        g_kernel_pgd[low_ram_pgd + i] = 0;
    }
    asm volatile("sfence.vma zero, zero" : : : "memory");
}

unsigned long vm_kernel_pgd(void) {
    return (unsigned long)g_kernel_pgd;
}

void vm_switch_pgd(unsigned long pgd) {
    unsigned long target = pgd ? pgd : (unsigned long)g_kernel_pgd;
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        :
        : "r"(MAKE_SATP(to_phys_addr(target)))
        : "memory");
}

unsigned long vm_user_pgd_create(void) {
    unsigned long *pgd = (unsigned long *)allocate(PAGE_SIZE);
    if (!pgd) return 0;
    for (unsigned long i = 0; i < ENTRIES_PER_TABLE; i++) pgd[i] = 0;
    for (unsigned long i = 256; i < ENTRIES_PER_TABLE; i++) pgd[i] = g_kernel_pgd[i];
    return (unsigned long)pgd;
}

int vm_map_pages(unsigned long pgd, unsigned long va, unsigned long size, unsigned long pa,
                 unsigned long prot) {
    if (!pgd || size == 0) return -1;
    unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    unsigned long *root = (unsigned long *)pgd;
    for (unsigned long i = 0; i < pages; i++) {
        unsigned long cur_va = va + i * PAGE_SIZE;
        unsigned long cur_pa = pa + i * PAGE_SIZE;
        unsigned long *l0 = walk_create(root, cur_va);
        if (!l0) return -1;
        unsigned long idx0 = (cur_va >> 12) & 0x1ff;
        l0[idx0] = MAKE_PTE(cur_pa, prot);
    }
    asm volatile("sfence.vma zero, zero" : : : "memory");
    return 0;
}

unsigned long vm_walk_pa(unsigned long pgd, unsigned long va) {
    if (!pgd) return 0;
    unsigned long *l2 = (unsigned long *)pgd;
    unsigned long e2 = l2[(va >> 30) & 0x1ff];
    if ((e2 & PTE_V) == 0) return 0;
    if (e2 & PTE_LEAF_MASK) return PTE_PA(e2) + (va & ((1UL << 30) - 1));
    unsigned long *l1 = pte_to_virt_table(e2);
    unsigned long e1 = l1[(va >> 21) & 0x1ff];
    if ((e1 & PTE_V) == 0) return 0;
    if (e1 & PTE_LEAF_MASK) return PTE_PA(e1) + (va & ((1UL << 21) - 1));
    unsigned long *l0 = pte_to_virt_table(e1);
    unsigned long e0 = l0[(va >> 12) & 0x1ff];
    if ((e0 & PTE_V) == 0) return 0;
    return PTE_PA(e0) + (va & (PAGE_SIZE - 1));
}

unsigned long *vm_lookup_pte(unsigned long pgd, unsigned long va) {
    if (!pgd) return NULL;
    unsigned long *l2 = (unsigned long *)pgd;
    unsigned long e2 = l2[(va >> 30) & 0x1ff];
    if ((e2 & PTE_V) == 0 || (e2 & PTE_LEAF_MASK)) return NULL;
    unsigned long *l1 = pte_to_virt_table(e2);
    unsigned long e1 = l1[(va >> 21) & 0x1ff];
    if ((e1 & PTE_V) == 0 || (e1 & PTE_LEAF_MASK)) return NULL;
    unsigned long *l0 = pte_to_virt_table(e1);
    unsigned long idx0 = (va >> 12) & 0x1ff;
    if ((l0[idx0] & PTE_V) == 0) return NULL;
    return &l0[idx0];
}

void vm_flush_tlb(void) {
    asm volatile("sfence.vma zero, zero" : : : "memory");
}

void vm_destroy_user_pgd(unsigned long pgd) {
    if (!pgd || pgd == (unsigned long)g_kernel_pgd) return;
    unsigned long *l2 = (unsigned long *)pgd;
    for (unsigned long i = 0; i < 256; i++) {
        unsigned long e2 = l2[i];
        if ((e2 & PTE_V) == 0 || (e2 & PTE_LEAF_MASK)) continue;
        unsigned long *l1 = pte_to_virt_table(e2);
        for (unsigned long j = 0; j < ENTRIES_PER_TABLE; j++) {
            unsigned long e1 = l1[j];
            if ((e1 & PTE_V) == 0 || (e1 & PTE_LEAF_MASK)) continue;
            unsigned long *l0 = pte_to_virt_table(e1);
            for (unsigned long k = 0; k < ENTRIES_PER_TABLE; k++) {
                unsigned long e0 = l0[k];
                if ((e0 & PTE_V) == 0 || (e0 & PTE_LEAF_MASK) == 0) continue;
                mm_page_put(PTE_PA(e0));
            }
            free(l0);
        }
        free(l1);
    }
    free((void *)pgd);
}
