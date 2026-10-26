/*
 * ESP32-S3 TIE (`ee.*`) single-instruction cases: the state a case loads
 * before its instruction and stores after it, the case table that
 * tools/internal/pie/silicon/gen_s3.py emits, and the runner. How test_ee.c
 * uses them, checked or printed: tools/internal/pie/silicon/README.md.
 */
#pragma once

#define EE_MEM_SIZE 1024

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

/* harness.S and the generated cases.S address this by offset; gen_s3.py
 * mirrors the offsets. */
typedef struct __attribute__((aligned(16))) {
    uint8_t q[8][16];       /* q0..q7 */
    uint32_t qacc_l[5];     /* QACC_L[159:0] as user registers QACC_L_0..4 */
    uint32_t qacc_h[5];     /* QACC_H[159:0] as QACC_H_0..4 */
    uint32_t accx[2];       /* ACCX_0 (ACCX[31:0]), ACCX_1 (ACCX[39:32]) */
    uint32_t sar;           /* the core's SAR */
    uint32_t sar_byte;
    uint32_t fft_bit_width;
    uint32_t gpio_out;
    uint32_t ua[4];         /* UA_STATE_0..3 */
    uint32_t f[16];         /* f0..f15 */
    uint32_t a[16];         /* a2..a15 as the instruction left them; a[0] is CPENABLE
                               after a gate case's instruction, a[1] unused */
    uint8_t mem[EE_MEM_SIZE];
} ee_state_t;

_Static_assert(offsetof(ee_state_t, qacc_l) == 128, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, accx) == 168, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, sar) == 176, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, ua) == 192, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, f) == 208, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, a) == 272, "harness.S offsets");
_Static_assert(offsetof(ee_state_t, mem) == 336, "harness.S offsets");

typedef struct {
    void (*fn)(ee_state_t *st);
    const char *name;
    uint8_t len;            /* instruction bytes, to step over a trap */
    uint8_t gate;           /* run with CPENABLE[3] clear, report CPENABLE */
    uint8_t q[8][16];
    uint8_t qacc[40];       /* QACC_L then QACC_H */
    uint32_t accx[2];
    uint32_t sar;
    uint32_t sar_byte;
    uint32_t fft;
    uint32_t gpio;
    uint8_t ua[16];
    uint32_t f[16];
    uint32_t a[16];
    /* Registers that hold an address: a[reg] = &mem[off] at run time. */
    struct {
        int8_t reg;
        int16_t off;
    } addr[2];
    uint32_t mem_seed;
} ee_case_t;

/* What a case left behind: the trap cause (0xff if none) and one FNV-1a hash
 * per state component. An address register is hashed as an offset into the
 * buffer, so the hashes do not depend on where an image puts it; `gate` is
 * CPENABLE after a gate case's instruction, 0 otherwise. */
enum { EE_H_TRAP, EE_H_Q, EE_H_QACC, EE_H_ACCX, EE_H_CTRL, EE_H_UA, EE_H_F, EE_H_A, EE_H_MEM,
       EE_H_GATE, EE_N_HASH };

/* The generated case table (cases.c). The committed subset (ee/) also
 * carries the hashes each case produced on silicon (gen_s3.py --expect). */
extern const ee_case_t ee_cases[];
extern const int ee_n_cases;
extern const uint32_t ee_expect[][EE_N_HASH];

/* The buffer's initial contents for a case: xorshift32 from `seed | 1`. */
void ee_fill_mem(uint8_t *mem, uint32_t seed);
/* Take the coprocessors and catch the exceptions a case can raise; `ee_end`
 * hands the exception causes back to their previous handlers. */
void ee_begin(void);
void ee_end(void);
void ee_load_case(ee_state_t *st, const ee_case_t *c);
/* Runs one case with interrupts off, then hashes what it left. */
void ee_run_case(ee_state_t *st, const ee_case_t *c, uint32_t out[EE_N_HASH]);

#else /* __ASSEMBLER__ */

/* One case function: load the whole state, put a2..a15 as the case wants
 * them, run the one instruction, store it all back. With `gate` 1 the case
 * clears CPENABLE[3] first: a gated instruction then traps into ESP-IDF's
 * lazy switch, which sets the bit again (same owner, so the state is kept)
 * and retries it, and CPENABLE afterwards (a[0]) tells. a0 holds the state
 * pointer while the instruction runs, so operands name a2..a15 only. */
.macro EE_CASE n, gate, insn:vararg
    .global ee_case_\n
    .type   ee_case_\n, @function
    .align  4
ee_case_\n:
    entry   a1, 32
    s32i    a2, a1, 0
    s32i    a0, a1, 4
    mov     a10, a2
    call8   ee_load_state
    l32i    a0, a1, 0
    .if \gate
    rsr.cpenable a3
    movi    a4, ~8
    and     a3, a3, a4
    wsr.cpenable a3
    rsync
    .endif
    l32i    a2, a0, 280
    l32i    a3, a0, 284
    l32i    a4, a0, 288
    l32i    a5, a0, 292
    l32i    a6, a0, 296
    l32i    a7, a0, 300
    l32i    a8, a0, 304
    l32i    a9, a0, 308
    l32i    a10, a0, 312
    l32i    a11, a0, 316
    l32i    a12, a0, 320
    l32i    a13, a0, 324
    l32i    a14, a0, 328
    l32i    a15, a0, 332
    \insn
    s32i    a2, a0, 280
    s32i    a3, a0, 284
    s32i    a4, a0, 288
    s32i    a5, a0, 292
    s32i    a6, a0, 296
    s32i    a7, a0, 300
    s32i    a8, a0, 304
    s32i    a9, a0, 308
    s32i    a10, a0, 312
    s32i    a11, a0, 316
    s32i    a12, a0, 320
    s32i    a13, a0, 324
    s32i    a14, a0, 328
    s32i    a15, a0, 332
    .if \gate
    rsr.cpenable a2
    s32i    a2, a0, 272
    .endif
    l32i    a0, a1, 4
    l32i    a10, a1, 0
    call8   ee_store_state
    retw
.endm

#endif /* __ASSEMBLER__ */
