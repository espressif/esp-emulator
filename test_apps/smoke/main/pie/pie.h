/*
 * PIE single-instruction cases (ESP32-P4, ESP32-S31): the state a case loads
 * before its instruction and stores after it, the case table that
 * tools/internal/pie/silicon/gen.py emits, and the runner. How test_pie.c
 * uses them, checked or printed: tools/internal/pie/silicon/README.md.
 */
#pragma once

#define PIE_MEM_SIZE 1024

/* Field offsets of pie_state_t, for harness.S and the generated cases.S. */
#define PIE_OFF_QACC 128
#define PIE_OFF_XACC 192
#define PIE_OFF_SAR  200
#define PIE_OFF_X    216
#define PIE_OFF_UA   288
#define PIE_OFF_MEM  304

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((aligned(16))) {
    uint8_t q[8][16];       /* q0..q7 */
    uint8_t qacc[64];       /* QACC l.l, l.h, h.l, h.h */
    uint64_t xacc;          /* XACC[39:0], stored sign-extended */
    uint32_t sar;
    uint32_t sar_bytes;
    uint32_t cfg;
    uint32_t fft_bw;
    uint32_t x[16];         /* x8..x15, x24..x31 */
    uint32_t pad[2];
    uint8_t ua[16];         /* UA_STATE */
    uint8_t mem[PIE_MEM_SIZE];
} pie_state_t;

_Static_assert(offsetof(pie_state_t, qacc) == PIE_OFF_QACC, "harness.S offsets");
_Static_assert(offsetof(pie_state_t, xacc) == PIE_OFF_XACC, "harness.S offsets");
_Static_assert(offsetof(pie_state_t, sar) == PIE_OFF_SAR, "harness.S offsets");
_Static_assert(offsetof(pie_state_t, x) == PIE_OFF_X, "harness.S offsets");
_Static_assert(offsetof(pie_state_t, ua) == PIE_OFF_UA, "harness.S offsets");
_Static_assert(offsetof(pie_state_t, mem) == PIE_OFF_MEM, "harness.S offsets");

enum {
    PIE_GPR_ADDR = 1,       /* x[reg] = &mem[v] */
    PIE_GPR_STEP = 2,       /* x[reg] = v, an address increment */
};

/* One case. q, QACC, XACC, UA_STATE, the GPRs and the buffer are filled from
 * `seed` (pie_load_case); the fields below override that. */
typedef struct {
    void (*fn)(pie_state_t *st);
    const char *name;
    uint32_t seed;
    uint8_t sar;
    uint8_t sar_bytes;
    uint16_t cfg;
    uint8_t fft_bw;
    /* esp.ldxq/stxq index the buffer with the halfwords of this q register:
     * pie_load_case keeps them in range. -1 for every other instruction. */
    int8_t qidx;
    struct {
        int8_t reg;         /* index into x[], -1 for none */
        int8_t kind;        /* PIE_GPR_ADDR or PIE_GPR_STEP */
        int16_t v;
    } gpr[3];
} pie_case_t;

/* What a case left behind: the trap cause (0 if none) and one FNV-1a hash
 * per state component. GPRs that held a buffer address are hashed as an
 * offset into it, so the hashes do not depend on where the image puts it. */
enum { PIE_H_TRAP, PIE_H_Q, PIE_H_QACC, PIE_H_XACC, PIE_H_CTRL, PIE_H_GPR, PIE_H_UA, PIE_H_MEM,
       PIE_N_HASH };

/* The generated case table (cases.c). The committed subset (pie/) also
 * carries the hashes each case produced on silicon; the full set (pie/full/)
 * instead lists the cases to print in full (gen.py --dump). */
extern const pie_case_t pie_cases[];
extern const int pie_n_cases;
extern const uint32_t pie_expect[][PIE_N_HASH];
extern const int pie_dump[];
extern const int pie_n_dump;

void pie_load_case(pie_state_t *st, const pie_case_t *c);
/* Runs one case with interrupts off and exceptions caught, then hashes. */
void pie_run_case(pie_state_t *st, const pie_case_t *c, uint32_t out[PIE_N_HASH]);

#else /* __ASSEMBLER__ */

/* One case function: save the callee-saved registers the GPR set covers,
 * load the whole state, run the one instruction (`word`, so any assembler
 * that knows the harness's own instructions builds it), store it all back. */
.macro PIE_CASE n, word
    .global pie_case_\n
    .p2align 2
pie_case_\n:
    addi    sp, sp, -32
    sw      ra, 0(sp)
    sw      s0, 4(sp)
    sw      s1, 8(sp)
    sw      s8, 12(sp)
    sw      s9, 16(sp)
    sw      s10, 20(sp)
    sw      s11, 24(sp)
    mv      t1, a0
    call    pie_load_state
    call    pie_load_gprs
    .word   \word
    call    pie_case_end    /* never returns here; ra is restored from the frame */
.endm

#endif /* __ASSEMBLER__ */
