/*
 * Load, run and hash one PIE case (pie.h). The random parts of a case's state
 * come from its seed here rather than from the generated table, which keeps
 * the table small enough to commit.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "pie.h"

/* Set by pie_trap_entry (harness.S) when a case's instruction traps. */
volatile uint32_t pie_trap_cause;
extern void pie_trap_entry(void);

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static void fill(uint32_t *s, void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        b[i] = (uint8_t)xorshift(s);
    }
}

void pie_load_case(pie_state_t *st, const pie_case_t *c)
{
    uint32_t s = c->seed | 1;

    memset(st, 0, sizeof(*st));
    fill(&s, st->q, sizeof(st->q));
    fill(&s, st->qacc, sizeof(st->qacc));
    fill(&s, &st->xacc, 5);
    fill(&s, st->ua, sizeof(st->ua));
    fill(&s, st->x, sizeof(st->x));
    fill(&s, st->mem, sizeof(st->mem));
    st->sar = c->sar;
    st->sar_bytes = c->sar_bytes;
    st->cfg = c->cfg;
    st->fft_bw = c->fft_bw;
    if (c->qidx >= 0) {
        /* rs1 + q.h[sel] * 4 must stay inside the buffer */
        uint8_t *q = st->q[c->qidx];
        for (int i = 0; i < 16; i++) {
            q[i] = (i % 2 == 0) ? q[i] % 32 : 0;
        }
    }
    for (int k = 0; k < 3; k++) {
        int r = c->gpr[k].reg;
        if (r < 0) {
            continue;
        }
        st->x[r] = c->gpr[k].kind == PIE_GPR_ADDR ? (uint32_t)(uintptr_t)&st->mem[c->gpr[k].v]
                                                  : (uint32_t)(int32_t)c->gpr[k].v;
    }
}

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

void pie_run_case(pie_state_t *st, const pie_case_t *c, uint32_t out[PIE_N_HASH])
{
    pie_load_case(st, c);
    pie_trap_cause = 0;
    /* CFG is not part of the saved task context: keep the scheduler off the
     * core while a case runs. */
    portDISABLE_INTERRUPTS();
    /* A context switch since the last case leaves the PIE disabled until the
     * next PIE instruction takes the lazy coprocessor enable. Take it here,
     * where the exception goes to IDF, not inside the case, where it would go
     * to the harness. With interrupts off the task stays on this core. */
    __asm__ volatile("esp.zero.q q0");
    uint32_t mtvec;
    __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
    /* keep the mode bits (CLIC); only the exception base moves */
    __asm__ volatile("csrw mtvec, %0" :: "r"((uint32_t)(uintptr_t)pie_trap_entry | (mtvec & 0x3f)));
    c->fn(st);
    __asm__ volatile("csrw mtvec, %0" :: "r"(mtvec));
    portENABLE_INTERRUPTS();

    uint32_t x[16];
    memcpy(x, st->x, sizeof(x));
    for (int k = 0; k < 3; k++) {
        if (c->gpr[k].reg >= 0 && c->gpr[k].kind == PIE_GPR_ADDR) {
            x[c->gpr[k].reg] -= (uint32_t)(uintptr_t)st->mem;
        }
    }
    out[PIE_H_TRAP] = pie_trap_cause;
    out[PIE_H_Q] = fnv1a(st->q, sizeof(st->q));
    out[PIE_H_QACC] = fnv1a(st->qacc, sizeof(st->qacc));
    out[PIE_H_XACC] = fnv1a(&st->xacc, sizeof(st->xacc));
    out[PIE_H_CTRL] = fnv1a(&st->sar, 16);
    out[PIE_H_GPR] = fnv1a(x, sizeof(x));
    out[PIE_H_UA] = fnv1a(st->ua, sizeof(st->ua));
    out[PIE_H_MEM] = fnv1a(st->mem, sizeof(st->mem));
}
