/*
 * Load, run and hash one ESP32-S3 TIE case (ee.h). Exceptions inside a case
 * are caught with xt_set_exception_handler, which steps over the instruction
 * so the case's epilogue stores the state as the fault left it.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "xtensa_api.h"
#include "xtensa/corebits.h"

#include "ee.h"

/* The running case's trap cause (0xff: none) and instruction length. */
static volatile uint32_t s_trap_cause;
static volatile uint32_t s_cur_len;

static void ee_trap(XtExcFrame *frame)
{
    s_trap_cause = frame->exccause;
    frame->pc += s_cur_len;
}

/* The causes an `ee.*` instruction can raise. CP3/CP0-disabled stay with
 * ESP-IDF's lazy coprocessor switch, which the gate cases rely on. */
static const int k_causes[] = {EXCCAUSE_ILLEGAL, EXCCAUSE_LOAD_STORE_ERROR, EXCCAUSE_UNALIGNED,
                               EXCCAUSE_LOAD_PROHIBITED, EXCCAUSE_STORE_PROHIBITED};
#define N_CAUSES (sizeof(k_causes) / sizeof(k_causes[0]))
static xt_exc_handler s_prev[N_CAUSES];

void ee_begin(void)
{
    /* Take TIE (CP3) and FPU (CP0) ownership for this task with interrupts on:
     * the first use of each traps into the lazy coprocessor enable. */
    __asm__ volatile("ee.zero.q q0");
    __asm__ volatile("movi a8, 0\n wfr f0, a8" ::: "a8");
    for (size_t k = 0; k < N_CAUSES; k++) {
        s_prev[k] = xt_set_exception_handler(k_causes[k], ee_trap);
    }
}

void ee_end(void)
{
    for (size_t k = 0; k < N_CAUSES; k++) {
        xt_set_exception_handler(k_causes[k], s_prev[k]);
    }
}

void ee_fill_mem(uint8_t *mem, uint32_t seed)
{
    uint32_t x = seed | 1;
    for (int i = 0; i < EE_MEM_SIZE; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        mem[i] = (uint8_t)x;
    }
}

void ee_load_case(ee_state_t *st, const ee_case_t *c)
{
    memset(st, 0, sizeof(*st));
    memcpy(st->q, c->q, sizeof(st->q));
    memcpy(st->qacc_l, c->qacc, 20);
    memcpy(st->qacc_h, c->qacc + 20, 20);
    memcpy(st->accx, c->accx, sizeof(st->accx));
    st->sar = c->sar;
    st->sar_byte = c->sar_byte;
    st->fft_bit_width = c->fft;
    st->gpio_out = c->gpio;
    memcpy(st->ua, c->ua, sizeof(st->ua));
    memcpy(st->f, c->f, sizeof(st->f));
    memcpy(st->a, c->a, sizeof(st->a));
    ee_fill_mem(st->mem, c->mem_seed);
    for (int k = 0; k < 2; k++) {
        if (c->addr[k].reg >= 0) {
            st->a[c->addr[k].reg] = (uint32_t)(uintptr_t)&st->mem[c->addr[k].off];
        }
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

void ee_run_case(ee_state_t *st, const ee_case_t *c, uint32_t out[EE_N_HASH])
{
    ee_load_case(st, c);
    s_trap_cause = 0xff;
    s_cur_len = c->len;
    /* Keep the scheduler off the core while a case runs. */
    portDISABLE_INTERRUPTS();
    c->fn(st);
    portENABLE_INTERRUPTS();

    uint32_t a[14];
    memcpy(a, &st->a[2], sizeof(a));
    for (int k = 0; k < 2; k++) {
        if (c->addr[k].reg >= 2) {
            a[c->addr[k].reg - 2] -= (uint32_t)(uintptr_t)st->mem;
        }
    }
    out[EE_H_TRAP] = s_trap_cause;
    out[EE_H_Q] = fnv1a(st->q, sizeof(st->q));
    out[EE_H_QACC] = fnv1a(st->qacc_l, 40);
    out[EE_H_ACCX] = fnv1a(st->accx, sizeof(st->accx));
    out[EE_H_CTRL] = fnv1a(&st->sar, 16);
    out[EE_H_UA] = fnv1a(st->ua, sizeof(st->ua));
    out[EE_H_F] = fnv1a(st->f, sizeof(st->f));
    out[EE_H_A] = fnv1a(a, sizeof(a));
    out[EE_H_MEM] = fnv1a(st->mem, EE_MEM_SIZE);
    out[EE_H_GATE] = st->a[0];
}
