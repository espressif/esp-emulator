/*
 * Load and store the whole PIE state at t1 (pie.h). Only x10 and t0 are used
 * besides t1: x10 because PIE addresses come from PIE-reachable GPRs, and a
 * case function (PIE_CASE) loads the GPR operands after pie_load_state
 * returns and stores them before pie_store_state runs.
 */
#include "pie.h"

    .text

    .global pie_load_gprs
    .p2align 2
pie_load_gprs:
    lw      x8, 216(t1)
    lw      x9, 220(t1)
    lw      x10, 224(t1)
    lw      x11, 228(t1)
    lw      x12, 232(t1)
    lw      x13, 236(t1)
    lw      x14, 240(t1)
    lw      x15, 244(t1)
    lw      x24, 248(t1)
    lw      x25, 252(t1)
    lw      x26, 256(t1)
    lw      x27, 260(t1)
    lw      x28, 264(t1)
    lw      x29, 268(t1)
    lw      x30, 272(t1)
    lw      x31, 276(t1)
    ret

    .global pie_load_state
    .p2align 2
pie_load_state:
    addi    x10, t1, 0
    esp.vld.128.ip  q0, x10, 16
    esp.vld.128.ip  q1, x10, 16
    esp.vld.128.ip  q2, x10, 16
    esp.vld.128.ip  q3, x10, 16
    esp.vld.128.ip  q4, x10, 16
    esp.vld.128.ip  q5, x10, 16
    esp.vld.128.ip  q6, x10, 16
    esp.vld.128.ip  q7, x10, 16
    esp.ld.qacc.l.l.128.ip  x10, 16
    esp.ld.qacc.l.h.128.ip  x10, 16
    esp.ld.qacc.h.l.128.ip  x10, 16
    esp.ld.qacc.h.h.128.ip  x10, 16
    esp.ld.xacc.ip  x10, 8
    lw      x10, 200(t1)
    esp.movx.w.sar  x10
    lw      x10, 204(t1)
    esp.movx.w.sar.bytes    x10
    lw      x10, 212(t1)
    esp.movx.w.fft.bit.width    x10
    addi    x10, t1, 288
    esp.ld.ua.state.ip  x10, 16
    /* last: CFG.mis_ld would force-align the loads above (all aligned anyway) */
    lw      x10, 208(t1)
    esp.movx.w.cfg  x10
    ret

    .global pie_store_state
    .p2align 2
pie_store_state:
    /* first: nothing below changes CFG, and esp.movx.r.cfg clears vxsat */
    esp.movx.r.cfg  x10
    sw      x10, 208(t1)
    esp.movx.r.sar  x10
    sw      x10, 200(t1)
    esp.movx.r.sar.bytes    x10
    sw      x10, 204(t1)
    esp.movx.r.fft.bit.width    x10
    sw      x10, 212(t1)
    addi    x10, t1, 0
    esp.vst.128.ip  q0, x10, 16
    esp.vst.128.ip  q1, x10, 16
    esp.vst.128.ip  q2, x10, 16
    esp.vst.128.ip  q3, x10, 16
    esp.vst.128.ip  q4, x10, 16
    esp.vst.128.ip  q5, x10, 16
    esp.vst.128.ip  q6, x10, 16
    esp.vst.128.ip  q7, x10, 16
    esp.st.qacc.l.l.128.ip  x10, 16
    esp.st.qacc.l.h.128.ip  x10, 16
    esp.st.qacc.h.l.128.ip  x10, 16
    esp.st.qacc.h.h.128.ip  x10, 16
    esp.st.s.xacc.ip    x10, 8
    addi    x10, t1, 288
    esp.st.ua.state.ip  x10, 16
    ret

/*
 * Exceptions inside a case land here (pie_run_case points mtvec at
 * pie_trap_entry, interrupts off): record mcause, then resume at
 * pie_case_end, which stores the state and returns as every case does, so a
 * trapping instruction is compared like any other.
 */
    /* CLIC mode (P4, S31) vectors synchronous exceptions to the mtvec base,
     * which must be 64-byte aligned. */
    /* In IRAM: a 64-byte-aligned function in flash text shifts .flash.text
     * into the rodata placeholder on ESP32-P4's linker script. */
    .section .iram1.pie_trap, "ax"
    .global pie_trap_entry
    .p2align 6
pie_trap_entry:
    csrr    t0, mcause
    la      t2, pie_trap_cause
    sw      t0, 0(t2)
    la      t0, pie_case_end
    csrw    mepc, t0
    mret

    .text
    .global pie_case_end
    .p2align 2
pie_case_end:
    sw      x8, 216(t1)
    sw      x9, 220(t1)
    sw      x10, 224(t1)
    sw      x11, 228(t1)
    sw      x12, 232(t1)
    sw      x13, 236(t1)
    sw      x14, 240(t1)
    sw      x15, 244(t1)
    sw      x24, 248(t1)
    sw      x25, 252(t1)
    sw      x26, 256(t1)
    sw      x27, 260(t1)
    sw      x28, 264(t1)
    sw      x29, 268(t1)
    sw      x30, 272(t1)
    sw      x31, 276(t1)
    call    pie_store_state
    lw      ra, 0(sp)
    lw      s0, 4(sp)
    lw      s1, 8(sp)
    lw      s8, 12(sp)
    lw      s9, 16(sp)
    lw      s10, 20(sp)
    lw      s11, 24(sp)
    addi    sp, sp, 32
    ret
