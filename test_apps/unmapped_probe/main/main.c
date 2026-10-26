/*
 * unmapped_probe — what an ESP32-S3 core does on a load/store to memory that
 * is not there, or that the access width cannot reach.
 *
 * One probe per boot: a probe that faults goes through IDF's real panic
 * handler (wrapped only to record EXCCAUSE/EXCVADDR in RTC memory) and the
 * reboot runs the next one. The last boot prints the whole table as
 *   PROBE:<n> <op> <addr> ok=<value>   or   PROBE:<n> <op> <addr> exc=<cause> vaddr=<addr>
 * followed by PROBE:done. `expected_esp32s3.txt` beside this app is the table
 * those lines are checked against — by tests/xtensa_unmapped_probe.rs (this
 * app, booted in the emulator) and tests/xtensa_unmapped.rs (the same accesses
 * as single instructions). Its `source` column marks the rows a real ESP32-S3
 * printed that the emulator does not model yet.
 *
 * On a board: idf.py -p PORT flash monitor (USB-Serial-JTAG works; the panic
 * handler prints there too).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_private/panic_internal.h"
#include "xtensa_context.h"

typedef enum { L32, L16, L8, S32R, SC1Z, SC1N, L32X, L32XH, L32XU, S32X } op_t;
typedef struct { op_t op; uint32_t addr; } probe_t;

static const probe_t PROBES[] = {
    /* holes inside the data/instruction regions (0x20000000-0x5FFFFFFF) */
    {L32, 0x48862a00}, {L16, 0x48862a00}, {L8, 0x48862a00},
    {L32, 0x44000000}, {L16, 0x44000000}, {L8, 0x44000000},
    {L32, 0x46000000},
    {L32, 0x4C000000},
    {L32, 0x2A000100}, {L16, 0x2A000100}, {L8, 0x2A000100},
    {L32, 0x3E000000},
    {L32, 0x3D800000},
    {L32, 0x50100000},
    {L32, 0x40800000},
    /* a hole in the peripheral region (0x60000000-0x7FFFFFFF) */
    {L32, 0x61000000}, {L8, 0x61000000},
    /* write to a hole, then read it back */
    {S32R, 0x48862a00},
    {S32R, 0x2A000100},
    /* instruction memory: word loads are fine, narrow loads are not */
    {L32, 0x40378000}, {L16, 0x40378000}, {L8, 0x40378000},
    {L32, 0x42000100}, {L16, 0x42000100}, {L8, 0x42000100},
    /* regions IDF marks illegal (0, 4-7) */
    {L32, 0x00000000}, {L32, 0x10000000}, {L32, 0x80000000}, {L32, 0xE0000000},
    {S32R, 0x10000000},
    /* the instruction-RAM port (0x40000000-0x43FFFFFF): unaligned words, and
       narrow access to ROM and to a hole inside it */
    {L32, 0x40378002}, {L32, 0x42000102}, {S32R, 0x40378002},
    {L8, 0x40000000}, {L8, 0x40800000},
    /* s32c1i on a hole, SCOMPARE1 = 0 (matches a hole's 0) and non-zero */
    {SC1Z, 0x48862a00}, {SC1N, 0x48862a00},
    /* the SRAM data window's edges, then word accesses that straddle them */
    {L32, 0x3fd00000}, {L32, 0x3fc87ffc},
    {L32X, 0x3fcffffe}, {L32XH, 0x3fc87ffe},
    {S32X, 0x3fcffffe}, {S32X, 0x3fc87ffe},
    /* the same at the end of RTC FAST memory, with aligned controls */
    {S32R, 0x3fcffffc}, {S32R, 0x600ffffc}, {L32, 0x60100000},
    {L32X, 0x600ffffe}, {S32X, 0x600ffffe},
    /* an aligned write just below SRAM's data window */
    {S32R, 0x3fc87ffc},
    /* the hole-to-SRAM load again, with no write before it */
    {L32XU, 0x3fc87ffe},
};
#define NPROBES (sizeof(PROBES) / sizeof(PROBES[0]))

#define MAGIC 0x50524F42u
typedef struct { uint32_t state; uint32_t val; uint32_t cause; uint32_t vaddr; } result_t;
enum { R_NONE = 0, R_OK = 1, R_EXC = 2 };
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_next;
static RTC_NOINIT_ATTR result_t s_res[NPROBES];

void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (s_magic == MAGIC && s_next > 0 && s_next <= NPROBES && info->frame) {
        const XtExcFrame *f = (const XtExcFrame *)info->frame;
        result_t *r = &s_res[s_next - 1];
        if (r->state == R_NONE) {
            r->state = R_EXC;
            r->cause = f->exccause;
            r->vaddr = f->excvaddr;
        }
    }
    __real_esp_panic_handler(info);
}

static const char *op_name(op_t op)
{
    switch (op) {
    case L32: return "l32i"; case L16: return "l16ui"; case L8: return "l8ui";
    case S32R: return "s32i+l32i";
    case SC1Z: return "s32c1i-0"; case SC1N: return "s32c1i-1";
    case L32X: return "l32i-x"; case L32XH: return "l32i-xh";
    case L32XU: return "l32i-xu"; case S32X: return "s32i-x";
    default: return "?";
    }
}

/* Compare-and-swap 0xA5C3F00D into addr; returns the old value s32c1i loads. */
static uint32_t s32c1i(uint32_t addr, uint32_t cmp)
{
    uint32_t v = 0xA5C3F00Du;
    __asm__ volatile("wsr %1, scompare1\n\tisync\n\ts32c1i %0, %2, 0"
                     : "+r"(v) : "r"(cmp), "r"(addr) : "memory");
    return v;
}

/* The word at addr, from the two aligned words around it (addr & 3 == 2). */
static uint32_t straddled(uint32_t addr)
{
    uint32_t lo = *(volatile uint32_t *)(addr - 2);
    uint32_t hi = *(volatile uint32_t *)(addr + 2);
    return (lo >> 16) | (hi << 16);
}

static uint32_t run_probe(const probe_t *p)
{
    switch (p->op) {
    case L32: return *(volatile uint32_t *)p->addr;
    case L16: return *(volatile uint16_t *)p->addr;
    case L8:  return *(volatile uint8_t *)p->addr;
    case S32R: {
        uint32_t old = 0;
        int ram = (p->addr >> 29) == 1 || (p->addr & 0xFFF00000u) == 0x60000000u;
        if (ram) /* put SRAM back afterwards */
            old = *(volatile uint32_t *)p->addr;
        *(volatile uint32_t *)p->addr = 0xA5C3F00Du;
        __asm__ volatile("memw");
        uint32_t v = *(volatile uint32_t *)p->addr;
        if (ram)
            *(volatile uint32_t *)p->addr = old;
        return v;
    }
    /* "ok" for the straddling rows is 0 / 0xA5C3F00D when each half acts like
       its own aligned access (a hole half reads 0, drops its write). */
    case SC1Z: return s32c1i(p->addr, 0);
    case SC1N: return s32c1i(p->addr, 0x12345678u);
    case L32XU: {
        uint32_t v;
        __asm__ volatile("l32i %0, %1, 0" : "=r"(v) : "r"(p->addr) : "memory");
        return v ^ straddled(p->addr);
    }
    case L32X:
    case L32XH: {
        /* Seed the memory side so its half is not 0: the low word for L32X
           (memory, then a hole), the high word for L32XH (a hole, then memory).
           The hole side is not written. */
        volatile uint32_t *m = (volatile uint32_t *)(p->op == L32X ? p->addr - 2 : p->addr + 2);
        uint32_t old = *m;
        *m = 0x11223344u;
        __asm__ volatile("memw");
        uint32_t v;
        __asm__ volatile("l32i %0, %1, 0" : "=r"(v) : "r"(p->addr) : "memory");
        v ^= straddled(p->addr);
        *m = old;
        return v;
    }
    case S32X: {
        uint32_t lo = *(volatile uint32_t *)(p->addr - 2);
        uint32_t hi = *(volatile uint32_t *)(p->addr + 2);
        __asm__ volatile("s32i %0, %1, 0\n\tmemw" :: "r"(0xA5C3F00Du), "r"(p->addr) : "memory");
        uint32_t v = straddled(p->addr);
        *(volatile uint32_t *)(p->addr - 2) = lo; /* put SRAM back */
        *(volatile uint32_t *)(p->addr + 2) = hi;
        return v;
    }
    default: return 0;
    }
}

static void print_tlb(void)
{
    uint32_t atomctl;
    __asm__ volatile("rsr %0, atomctl" : "=r"(atomctl));
    printf("ATOMCTL:0x%08lx\n", (unsigned long)atomctl);
    for (uint32_t r = 0; r < 8; r++) {
        uint32_t a = r << 29, i, d;
        __asm__ volatile("ritlb1 %0, %1" : "=r"(i) : "r"(a));
        __asm__ volatile("rdtlb1 %0, %1" : "=r"(d) : "r"(a));
        printf("TLB:%u base=0x%08lx itlb_ca=0x%lx dtlb_ca=0x%lx\n", (unsigned)r,
               (unsigned long)a, (unsigned long)(i & 0xF), (unsigned long)(d & 0xF));
    }
}

void app_main(void)
{
    if (s_magic != MAGIC || s_next > NPROBES) {
        memset(s_res, 0, sizeof(s_res));
        s_next = 0;
        s_magic = MAGIC;
        printf("PROBE:start n=%u\n", (unsigned)NPROBES);
        print_tlb();
    }
    while (s_next < NPROBES) {
        uint32_t n = s_next++;
        const probe_t *p = &PROBES[n];
        printf("PROBE:try %u %s 0x%08lx\n", (unsigned)n, op_name(p->op), (unsigned long)p->addr);
        fflush(stdout);
        esp_rom_delay_us(2000); /* let the line drain before a possible fault */
        uint32_t v = run_probe(p);
        s_res[n].state = R_OK;
        s_res[n].val = v;
    }
    for (uint32_t n = 0; n < NPROBES; n++) {
        const probe_t *p = &PROBES[n];
        const result_t *r = &s_res[n];
        if (r->state == R_OK)
            printf("PROBE:%u %s 0x%08lx ok=0x%08lx\n", (unsigned)n, op_name(p->op),
                   (unsigned long)p->addr, (unsigned long)r->val);
        else if (r->state == R_EXC)
            printf("PROBE:%u %s 0x%08lx exc=%lu vaddr=0x%08lx\n", (unsigned)n, op_name(p->op),
                   (unsigned long)p->addr, (unsigned long)r->cause, (unsigned long)r->vaddr);
        else
            printf("PROBE:%u %s 0x%08lx none\n", (unsigned)n, op_name(p->op), (unsigned long)p->addr);
    }
    printf("PROBE:done\n");
    s_magic = 0; /* a later reset starts over */
}
