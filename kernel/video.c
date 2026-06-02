#include "kernel/common.h"

#define QEMU_PACKED __attribute__((packed))

static inline uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint32_t bswap32_local(uint32_t x) { return bswap32(x); }
static inline uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32_local((uint32_t)x) << 32) | (uint64_t)bswap32_local((uint32_t)(x >> 32));
}

#define XRGB8888 875713112u

struct QEMU_PACKED RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

#define FW_CFG_BASE PHYS_TO_VIRT_U(0x10100000UL)
#define FW_CFG_DMA (uint64_t *)(FW_CFG_BASE + 0x10)

#define FW_CFG_DMA_CTL_ERROR 0x01
#define FW_CFG_DMA_CTL_READ 0x02
#define FW_CFG_DMA_CTL_SELECT 0x08
#define FW_CFG_DMA_CTL_WRITE 0x10

#define FW_CFG_FILE_DIR 0x19

struct QEMU_PACKED FWCfgFile {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char name[56];
};

struct QEMU_PACKED FWCfgDmaAccess {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

static unsigned long to_phys_addr(unsigned long x) {
    return (x >= PAGE_OFFSET) ? VIRT_TO_PHYS_U(x) : x;
}

static void fw_cfg_dma_transfer(void *address, uint32_t length, uint32_t control) {
    struct FWCfgDmaAccess access = {
        .control = bswap32_local(control),
        .length = bswap32_local(length),
        .address = bswap64((uint64_t)to_phys_addr((unsigned long)address)),
    };
    *FW_CFG_DMA = bswap64((uint64_t)to_phys_addr((unsigned long)&access));
    while (bswap32_local(access.control) & ~FW_CFG_DMA_CTL_ERROR) {
    }
}

static void fw_cfg_read_entry(void *buf, int e, int len) {
    uint32_t control = ((uint32_t)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_READ;
    fw_cfg_dma_transfer(buf, (uint32_t)len, control);
}

static void fw_cfg_write_entry(void *buf, int e, int len) {
    uint32_t control = ((uint32_t)e << 16) | FW_CFG_DMA_CTL_SELECT | FW_CFG_DMA_CTL_WRITE;
    fw_cfg_dma_transfer(buf, (uint32_t)len, control);
}

static int fw_cfg_find_file(const char *name) {
    uint32_t count = 0;
    fw_cfg_read_entry(&count, FW_CFG_FILE_DIR, (int)sizeof(count));
    count = bswap32_local(count);
    for (uint32_t i = 0; i < count; i++) {
        struct FWCfgFile file;
        fw_cfg_dma_transfer(&file, (uint32_t)sizeof(file), FW_CFG_DMA_CTL_READ);
        if (c_strncmp(name, file.name, (int)sizeof(file.name)) == 0) return (int)bswap16(file.select);
    }
    return -1;
}

static int g_video_inited;

void video_init(void) {
    if (g_video_inited) return;
    struct RAMFBCfg cfg = {
        .addr = bswap64((uint64_t)to_phys_addr((unsigned long)FB_BASE)),
        .fourcc = bswap32_local(XRGB8888),
        .flags = bswap32_local(0),
        .width = bswap32_local(FB_WIDTH),
        .height = bswap32_local(FB_HEIGHT),
        .stride = bswap32_local(FB_WIDTH * FB_BPP),
    };
    int sel = fw_cfg_find_file("etc/ramfb");
    if (sel >= 0) fw_cfg_write_entry(&cfg, sel, (int)sizeof(cfg));
    g_video_inited = 1;
}

static void bytes_copy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
}

static void flush_dcache(void *addr, unsigned long len) {
    (void)addr;
    (void)len;
    asm volatile("fence rw, rw" ::: "memory");
}

void video_bmp_display(unsigned int *bmp_image, int width, int height) {
    if (!bmp_image || width <= 0 || height <= 0) return;
    unsigned int *fb = (unsigned int *)FB_BASE;
    int start_x = (FB_WIDTH - width) / 2;
    int start_y = (FB_HEIGHT - height) / 2;
    for (int y = 0; y < height; y++) {
        void *dst = fb + (start_y + y) * FB_WIDTH + start_x;
        const void *src = bmp_image + y * width;
        unsigned long bytes = (unsigned long)width * sizeof(unsigned int);
        bytes_copy(dst, src, bytes);
        flush_dcache(dst, bytes);
    }
}
