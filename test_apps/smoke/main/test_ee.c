/*
 * ESP32-S3 TIE (`ee.*`): a few cases per instruction, each checked against
 * what silicon produced. With sdkconfig.ee_full, the full case set instead,
 * printed for the board-vs-emulator differential test.
 */
#include "test_smoke.h"

#if EMU_TEST_EE
#include "ee/ee.h"

#if CONFIG_EMU_TEST_EE_FULL
/* --------------- ee.*, full set ---------------
 * The board-vs-emulator differential test (tools/internal/pie/silicon): print
 * every case's complete final state on one line for compare_s3.py, with a
 * hash of the line's text so a line the USB-Serial-JTAG console mangled can
 * be dropped and taken from another capture. */

static char s_line[4096];
static size_t s_len;

static void put(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_line + s_len, sizeof(s_line) - s_len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s_len = s_len + n < sizeof(s_line) ? s_len + n : sizeof(s_line) - 1;
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

static void put_hex(const char *tag, const void *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    const uint8_t *b = p;
    put(" %s=", tag);
    for (size_t i = 0; i < n && s_len + 2 < sizeof(s_line); i++) {
        s_line[s_len++] = digits[b[i] >> 4];
        s_line[s_len++] = digits[b[i] & 15];
    }
    s_line[s_len] = 0;
}

/* The buffer as runs of bytes that differ from the initial fill. */
static void put_mem_diff(const ee_state_t *st, uint32_t seed)
{
    static uint8_t ref[EE_MEM_SIZE];
    ee_fill_mem(ref, seed);
    put(" m=");
    for (int i = 0; i < EE_MEM_SIZE;) {
        if (st->mem[i] == ref[i]) {
            i++;
            continue;
        }
        put("%d:", i);
        while (i < EE_MEM_SIZE && st->mem[i] != ref[i]) {
            put("%02x", st->mem[i]);
            i++;
        }
        put(",");
    }
}

void ee_print_all(void)
{
    static ee_state_t st;
    uint32_t h[EE_N_HASH];

    ee_begin();
    /* `mem=`: where this image keeps the buffer, so compare_s3.py and
     * gen_s3.py --expect can read address registers as offsets. */
    printf("EE_BEGIN %d mem=%08x\n", ee_n_cases, (unsigned)(uintptr_t)st.mem);
    for (int i = 0; i < ee_n_cases; i++) {
        const ee_case_t *c = &ee_cases[i];
        ee_run_case(&st, c, h);
        s_len = 0;
        put("S %d %s T%lx", i, c->name, (unsigned long)h[EE_H_TRAP]);
        put_hex("q", st.q, sizeof(st.q));
        put_hex("qa", st.qacc_l, 40);
        put_hex("ax", st.accx, 8);
        put(" c=%lx,%lx,%lx,%lx", (unsigned long)st.sar, (unsigned long)st.sar_byte,
            (unsigned long)st.fft_bit_width, (unsigned long)st.gpio_out);
        put_hex("ua", st.ua, 16);
        put_hex("f", st.f, sizeof(st.f));
        put_hex("a", &st.a[2], 14 * 4);
        put_mem_diff(&st, c->mem_seed);
        if (c->gate) {
            put(" g=%lx", (unsigned long)st.a[0]);
        }
        printf("%s k=%08lx\n", s_line, (unsigned long)fnv1a(s_line, s_len));
        /* let the idle task feed the task watchdog */
        if (i % 64 == 63) {
            vTaskDelay(1);
        }
    }
    printf("EE_DONE %d\n", ee_n_cases);
    ee_end();
}

#else

/* --------------- ee.* ---------------
 * Each case loads a TIE state, runs one instruction and compares the trap
 * cause and per-component hashes with an ESP32-S3 board's. The cases are the
 * smoke subset of tools/internal/pie/silicon, which generates them and
 * records the expected hashes (gen_s3.py --expect). */

static const char *const k_ee_part[EE_N_HASH] = {
    "trap", "q", "qacc", "accx", "ctrl", "ua", "f", "a", "mem", "gate",
};

bool test_ee(void)
{
    static ee_state_t st;
    uint32_t h[EE_N_HASH];
    int bad = 0;

    ee_begin();
    for (int i = 0; i < ee_n_cases; i++) {
        ee_run_case(&st, &ee_cases[i], h);
        for (int k = 0; k < EE_N_HASH; k++) {
            if (h[k] != ee_expect[i][k]) {
                if (bad < 10) {
                    FAIL_MSG("ee", "case %d %s: %s %08" PRIx32 ", silicon %08" PRIx32, i,
                             ee_cases[i].name, k_ee_part[k], h[k], ee_expect[i][k]);
                }
                bad++;
                break;
            }
        }
        if (i % 64 == 63) {
            vTaskDelay(1);
        }
    }
    ee_end();
    ESP_LOGI(TAG, "  ee: %d cases, %d differ from silicon", ee_n_cases, bad);
    return bad == 0;
}
#endif /* CONFIG_EMU_TEST_EE_FULL */
#endif /* EMU_TEST_EE */
