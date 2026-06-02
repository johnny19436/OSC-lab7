#include "kernel/common.h"

#define UART_DEFAULT_BASE PHYS_TO_VIRT_U(0x10000000UL)
static volatile unsigned long g_uart_base = UART_DEFAULT_BASE;

#define UART_IER (uart_reg(0x01))
#define UART_IIR (uart_reg(0x02))
#define UART_MCR (uart_reg(0x04))

void uart_set_base(unsigned long base) {
    if (base) {
        g_uart_base = (base < PAGE_OFFSET) ? PHYS_TO_VIRT_U(base) : base;
    }
}

unsigned long uart_get_base(void) {
    return (unsigned long)g_uart_base;
}

static inline volatile unsigned char *uart_reg(unsigned long off) {
    return (volatile unsigned char *)(g_uart_base + off);
}

#define UART_RBR (uart_reg(0x00))
#define UART_THR (uart_reg(0x00))
#define UART_LSR (uart_reg(0x05))
#define LSR_DR (1u << 0)
#define LSR_TDRQ (1u << 5)
#define IER_RXINT (1u << 0)
#define MCR_OUT2 (1u << 3)

#define RING_SIZE 256
static unsigned char g_rx_ring[RING_SIZE];
static volatile unsigned int g_rx_head;
static volatile unsigned int g_rx_tail;
static int g_irq_mode;

static void local_irq_disable(void) {
    asm volatile("csrci sstatus, (1 << 1)");
}

static void local_irq_enable(void) {
    asm volatile("csrsi sstatus, (1 << 1)");
}

void uart_irq_init(void) {
    g_rx_head = g_rx_tail = 0;
    *UART_IER = 0;
    *UART_IER |= IER_RXINT;
    *UART_MCR |= MCR_OUT2;
    g_irq_mode = 1;
}

void uart_handle_irq(void) {
    while (1) {
        unsigned char iir = *UART_IIR;
        if (iir & 0x01) {
            break;
        }
        switch (iir & 0x0e) {
            case 0x04:
            case 0x0c:
                while ((*UART_LSR & LSR_DR) != 0) {
                    unsigned int next = (g_rx_head + 1) & (RING_SIZE - 1);
                    unsigned char c = (unsigned char)(*UART_RBR & 0xffu);
                    if (next != g_rx_tail) {
                        g_rx_ring[g_rx_head] = c;
                        g_rx_head = next;
                    }
                }
                break;
            default:
                (void)*UART_LSR;
                break;
        }
    }
}

static char normalize_nl(char c) {
    return (c == '\r') ? '\n' : c;
}

char uart_getc(void) {
    char c;
    unsigned long sstatus = 0;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    if (g_irq_mode && (sstatus & (1UL << 1))) {
        while (1) {
            if (g_rx_head != g_rx_tail) {
                local_irq_disable();
                if (g_rx_head != g_rx_tail) {
                    c = (char)g_rx_ring[g_rx_tail];
                    g_rx_tail = (g_rx_tail + 1) & (RING_SIZE - 1);
                    local_irq_enable();
                    return normalize_nl(c);
                }
                local_irq_enable();
            }
        }
    }
    while (((*UART_LSR) & LSR_DR) == 0) {
    }
    c = (char)(*UART_RBR & 0xFFu);
    return normalize_nl(c);
}

void uart_putc(char c) {
    if (c == '\n') {
        uart_putc('\r');
    }
    while (((*UART_LSR) & LSR_TDRQ) == 0) {
    }
    *UART_THR = (unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

void uart_hex(unsigned long val) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        unsigned int nibble = (val >> i) & 0xF;
        uart_putc((char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
    }
}
