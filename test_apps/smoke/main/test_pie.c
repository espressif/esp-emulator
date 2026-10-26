/*
 * PIE (ESP32-P4, ESP32-S31): one or two cases per instruction, each checked
 * against what silicon produced. With sdkconfig.pie_full, the full case set
 * instead, printed for the board-vs-emulator differential test.
 */
#include "test_smoke.h"

#if EMU_TEST_PIE
#include "pie/pie.h"

#if CONFIG_EMU_TEST_PIE_FULL
/* --------------- PIE, full set ---------------
 * The board-vs-emulator differential test (tools/internal/pie/silicon): print
 * one line of hashes per case for compare.py; the cases gen.py --dump lists
 * also print their whole state. */

static void print_hex(const char *tag, const void *p, size_t n)
{
    const uint8_t *b = p;
    printf("D %s ", tag);
    for (size_t i = 0; i < n; i++) {
        printf("%02x", b[i]);
    }
    printf("\n");
}

static void dump_state(int i, const pie_state_t *st)
{
    printf("D %d begin\n", i);
    for (int q = 0; q < 8; q++) {
        char tag[4] = {'q', (char)('0' + q), 0};
        print_hex(tag, st->q[q], 16);
    }
    print_hex("qacc", st->qacc, 64);
    print_hex("xacc", &st->xacc, 8);
    printf("D ctrl sar=%" PRIu32 " sar_bytes=%" PRIu32 " cfg=0x%" PRIx32 " fft_bw=%" PRIu32 "\n",
           st->sar, st->sar_bytes, st->cfg, st->fft_bw);
    print_hex("x", st->x, sizeof(st->x));
    print_hex("ua", st->ua, 16);
    print_hex("mem", st->mem, PIE_MEM_SIZE);
}

static bool in_dump_list(int i)
{
    for (int k = 0; k < pie_n_dump; k++) {
        if (pie_dump[k] == i) {
            return true;
        }
    }
    return false;
}

void pie_print_all(void)
{
    static pie_state_t st;

    /* Take PIE ownership for this task with interrupts on: the first PIE
     * instruction traps into the lazy coprocessor enable. */
    __asm__ volatile("esp.zero.q q0");

    printf("PIE_BEGIN %d\n", pie_n_cases);
    for (int i = 0; i < pie_n_cases; i++) {
        uint32_t h[PIE_N_HASH];
        pie_run_case(&st, &pie_cases[i], h);
        printf("R %d %s T%" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
               " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n", i, pie_cases[i].name,
               h[PIE_H_TRAP], h[PIE_H_Q], h[PIE_H_QACC], h[PIE_H_XACC], h[PIE_H_CTRL],
               h[PIE_H_GPR], h[PIE_H_UA], h[PIE_H_MEM]);
        if (in_dump_list(i)) {
            dump_state(i, &st);
        }
        /* let the idle task feed the task watchdog */
        if (i % 64 == 63) {
            vTaskDelay(1);
        }
    }
    register uint32_t cfg __asm__("a0") = 0;
    __asm__ volatile("esp.movx.w.cfg %0" :: "r"(cfg));
    printf("PIE_DONE %d\n", pie_n_cases);
}

#else

/* --------------- PIE ---------------
 * Each case loads a random PIE state, runs one instruction and compares the
 * trap cause and per-component hashes with an ESP32-S31 board's. The cases
 * are the smoke subset of tools/internal/pie/silicon, which generates them
 * and records the expected hashes (gen.py --expect). */

static const char *const k_pie_part[PIE_N_HASH] = {
    "trap", "q", "qacc", "xacc", "ctrl", "gpr", "ua", "mem",
};

bool test_pie(void)
{
    static pie_state_t st;
    uint32_t h[PIE_N_HASH];
    int bad = 0;

    /* Take PIE ownership for this task with interrupts on: the first PIE
     * instruction traps into the lazy coprocessor enable. */
    __asm__ volatile("esp.zero.q q0");

    for (int i = 0; i < pie_n_cases; i++) {
        pie_run_case(&st, &pie_cases[i], h);
        for (int k = 0; k < PIE_N_HASH; k++) {
            if (h[k] != pie_expect[i][k]) {
                if (bad < 10) {
                    FAIL_MSG("pie", "case %d %s: %s %08" PRIx32 ", silicon %08" PRIx32, i,
                             pie_cases[i].name, k_pie_part[k], h[k], pie_expect[i][k]);
                }
                bad++;
                break;
            }
        }
    }
    /* Leave CFG as IDF expects it; it is not part of the task context. PIE
     * register fields reach x8-x15 and x24-x31 only, hence a0. */
    register uint32_t cfg __asm__("a0") = 0;
    __asm__ volatile("esp.movx.w.cfg %0" :: "r"(cfg));
    ESP_LOGI(TAG, "  pie: %d cases, %d differ from silicon", pie_n_cases, bad);
    return bad == 0;
}
#endif /* CONFIG_EMU_TEST_PIE_FULL */
#endif /* EMU_TEST_PIE */
