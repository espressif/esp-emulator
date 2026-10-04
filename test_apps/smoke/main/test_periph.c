/*
 * Console output, GPIO and RMT.
 */
#include "test_smoke.h"
#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif

/* --------------- 5. UART / printf --------------- */

bool test_printf(void)
{
    printf("  printf test: int=%d str=%s hex=0x%x unsigned=%u ptr=%p\n",
           -123, "test_string", 0xCAFE, 99u, (void *)0x12345678);

    /* Long string - verify no crash */
    char long_str[256];
    memset(long_str, 'A', sizeof(long_str) - 1);
    long_str[sizeof(long_str) - 1] = '\0';
    printf("  long string (255 chars): %.20s...\n", long_str);

    return true;
}

/* --------------- 6. GPIO --------------- */

bool test_gpio(void)
{
    const char *TN = "gpio";
    esp_err_t err;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "gpio_config: %s", esp_err_to_name(err));
        return false;
    }

    err = gpio_set_level(2, 1);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "set_level high: %s", esp_err_to_name(err));
        return false;
    }

    /* Read back - may return 0 in emulator (no loopback), just check no crash */
    int level = gpio_get_level(2);
    ESP_LOGI(TAG, "  GPIO2 read back: %d", level);

    err = gpio_set_level(2, 0);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "set_level low: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

/* --------------- 6b. RMT TX --------------- */

#if SOC_RMT_SUPPORTED
/* Transmit 128 symbols through a 48-word channel — more than 2.5 memory
 * blocks, so the driver's ping-pong path (TX_THR_EVENT refills + wrap mode)
 * must work, not just TX_END. A second short transmit checks the channel
 * survives reuse. */
bool test_rmt_tx(void)
{
    const char *TN = "rmt_tx";
    esp_err_t err;

    rmt_channel_handle_t chan = NULL;
    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = 8,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz, 0.1 us per tick */
        .trans_queue_depth = 4,
    };
    err = rmt_new_tx_channel(&cfg, &chan);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_new_tx_channel: %s", esp_err_to_name(err));
        return false;
    }

    rmt_encoder_handle_t copy_enc = NULL;
    rmt_copy_encoder_config_t enc_cfg = {};
    err = rmt_new_copy_encoder(&enc_cfg, &copy_enc);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_new_copy_encoder: %s", esp_err_to_name(err));
        rmt_del_channel(chan);
        return false;
    }

    err = rmt_enable(chan);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_enable: %s", esp_err_to_name(err));
        goto fail;
    }

    static rmt_symbol_word_t syms[128];
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        /* WS2812-ish 1.0 us bit: 0.4 us high, 0.6 us low. */
        syms[i] = (rmt_symbol_word_t) {
            .level0 = 1, .duration0 = 4,
            .level1 = 0, .duration1 = 6,
        };
    }

    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    err = rmt_transmit(chan, copy_enc, syms, sizeof(syms), &tx_cfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_transmit: %s", esp_err_to_name(err));
        goto fail;
    }
    err = rmt_tx_wait_all_done(chan, 1000);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_tx_wait_all_done (ping-pong): %s", esp_err_to_name(err));
        goto fail;
    }

    /* Channel reuse: a short second frame must also complete. */
    err = rmt_transmit(chan, copy_enc, syms, 8 * sizeof(syms[0]), &tx_cfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_transmit #2: %s", esp_err_to_name(err));
        goto fail;
    }
    err = rmt_tx_wait_all_done(chan, 1000);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "rmt_tx_wait_all_done #2: %s", esp_err_to_name(err));
        goto fail;
    }

    rmt_disable(chan);
    rmt_del_encoder(copy_enc);
    rmt_del_channel(chan);
    return true;

fail:
    rmt_disable(chan);
    rmt_del_encoder(copy_enc);
    rmt_del_channel(chan);
    return false;
}
#endif /* SOC_RMT_SUPPORTED */

/* --------------- 7. LEDC --------------- */

#if SOC_LEDC_SUPPORTED
/* ledc_channel_config() reserves this pin for the lifetime of the process, so
 * it must not overlap a pin a later test claims: S31's EMAC defaults take
 * GPIO 5-6 and 8-19 (MDC is 5), and the RMT test uses GPIO 8. */
#define LEDC_TEST_GPIO 4

static volatile int s_ledc_fade_cbs;

static bool ledc_on_fade_end(const ledc_cb_param_t *param, void *user_arg)
{
    if (param->event == LEDC_FADE_END_EVT) {
        s_ledc_fade_cbs++;
    }
    return false;
}

/* 5 kHz / 13-bit PWM: set a duty and read it back, then run a 200 ms
 * hardware fade with LEDC_FADE_WAIT_DONE — completion travels through the
 * LEDC fade-end interrupt, and the blocking time must track the request. */
bool test_ledc(void)
{
    const char *TN = "ledc";
    esp_err_t err;

    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "ledc_timer_config: %s", esp_err_to_name(err));
        return false;
    }
    ledc_channel_config_t ccfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LEDC_TEST_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "ledc_channel_config: %s", esp_err_to_name(err));
        return false;
    }

    uint32_t freq = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
    if (freq < 4900 || freq > 5100) {
        FAIL_MSG(TN, "freq readback: %u (want ~5000)", (unsigned)freq);
        return false;
    }

    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 4096);
    if (err != ESP_OK) { FAIL_MSG(TN, "ledc_set_duty: %s", esp_err_to_name(err)); return false; }
    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) { FAIL_MSG(TN, "ledc_update_duty: %s", esp_err_to_name(err)); return false; }
    vTaskDelay(pdMS_TO_TICKS(5)); /* > one 200 us PWM period for the latch */
    uint32_t duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (duty != 4096) {
        FAIL_MSG(TN, "duty readback after update: %u (want 4096)", (unsigned)duty);
        return false;
    }

    err = ledc_fade_func_install(0);
    if (err != ESP_OK) { FAIL_MSG(TN, "fade_func_install: %s", esp_err_to_name(err)); return false; }
    ledc_cbs_t cbs = { .fade_cb = ledc_on_fade_end };
    err = ledc_cb_register(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, &cbs, NULL);
    if (err != ESP_OK) { FAIL_MSG(TN, "ledc_cb_register: %s", esp_err_to_name(err)); goto fail; }

    int64_t t0 = esp_timer_get_time();
    err = ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 8191, 200);
    if (err != ESP_OK) { FAIL_MSG(TN, "set_fade_with_time: %s", esp_err_to_name(err)); goto fail; }
    err = ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    if (err != ESP_OK) { FAIL_MSG(TN, "ledc_fade_start: %s", esp_err_to_name(err)); goto fail; }
    int64_t dt_us = esp_timer_get_time() - t0;

    duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (duty != 8191) {
        FAIL_MSG(TN, "duty after fade: %u (want 8191)", (unsigned)duty);
        goto fail;
    }
    /* The hardware fade must take real emulated time in the right ballpark
     * (the driver quantizes scale/cycle, so allow a generous window). */
    if (dt_us < 100000 || dt_us > 450000) {
        FAIL_MSG(TN, "200 ms fade took %lld us", (long long)dt_us);
        goto fail;
    }
    if (s_ledc_fade_cbs != 1) {
        FAIL_MSG(TN, "fade-end callbacks: %d (want 1)", s_ledc_fade_cbs);
        goto fail;
    }

    ledc_fade_func_uninstall();
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    return true;

fail:
    ledc_fade_func_uninstall();
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    return false;
}
#endif /* SOC_LEDC_SUPPORTED */
#if SOC_PCNT_SUPPORTED
#define PCNT_TEST_GPIO 4

static volatile int s_pcnt_watch_hits;
static volatile int s_pcnt_last_watch;

static bool pcnt_on_reach(pcnt_unit_handle_t unit,
                          const pcnt_watch_event_data_t *edata, void *user_ctx)
{
    s_pcnt_watch_hits++;
    s_pcnt_last_watch = edata->watch_point_value;
    return false;
}

/* Count software-driven edges on a pin routed into PCNT through the GPIO
 * matrix (the pin runs INPUT_OUTPUT so gpio_set_level feeds the counter),
 * with a watch point delivering its event through the PCNT interrupt. */
bool test_pcnt(void)
{
    const char *TN = "pcnt";
    esp_err_t err;
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;

    pcnt_unit_config_t ucfg = {
        .high_limit = 100,
        .low_limit = -100,
    };
    err = pcnt_new_unit(&ucfg, &unit);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "pcnt_new_unit: %s", esp_err_to_name(err));
        return false;
    }
    pcnt_chan_config_t ccfg = {
        .edge_gpio_num = PCNT_TEST_GPIO,
        .level_gpio_num = -1,
    };
    err = pcnt_new_channel(unit, &ccfg, &chan);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_new_channel: %s", esp_err_to_name(err)); goto fail; }
    err = pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) { FAIL_MSG(TN, "set_edge_action: %s", esp_err_to_name(err)); goto fail; }
    err = pcnt_unit_add_watch_point(unit, 5);
    if (err != ESP_OK) { FAIL_MSG(TN, "add_watch_point: %s", esp_err_to_name(err)); goto fail; }
    pcnt_event_callbacks_t cbs = { .on_reach = pcnt_on_reach };
    err = pcnt_unit_register_event_callbacks(unit, &cbs, NULL);
    if (err != ESP_OK) { FAIL_MSG(TN, "register_event_callbacks: %s", esp_err_to_name(err)); goto fail; }
    err = pcnt_unit_enable(unit);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_unit_enable: %s", esp_err_to_name(err)); goto fail; }
    err = pcnt_unit_clear_count(unit);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_unit_clear_count: %s", esp_err_to_name(err)); goto fail; }
    err = pcnt_unit_start(unit);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_unit_start: %s", esp_err_to_name(err)); goto fail; }

    /* Drive the routed pad from software: 8 rising edges. */
    gpio_set_direction(PCNT_TEST_GPIO, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(PCNT_TEST_GPIO, 0);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(PCNT_TEST_GPIO, 1);
        gpio_set_level(PCNT_TEST_GPIO, 0);
    }
    int count = 0;
    err = pcnt_unit_get_count(unit, &count);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_unit_get_count: %s", esp_err_to_name(err)); goto fail; }
    if (count != 8) { FAIL_MSG(TN, "count after 8 rising edges: %d", count); goto fail; }

    /* The watch point at 5 must have fired exactly once, via the ISR. */
    vTaskDelay(pdMS_TO_TICKS(10));
    if (s_pcnt_watch_hits != 1 || s_pcnt_last_watch != 5) {
        FAIL_MSG(TN, "watch point: hits=%d last=%d (want 1 hit at 5)",
                 s_pcnt_watch_hits, s_pcnt_last_watch);
        goto fail;
    }

    /* Stopped unit must hold its count. */
    err = pcnt_unit_stop(unit);
    if (err != ESP_OK) { FAIL_MSG(TN, "pcnt_unit_stop: %s", esp_err_to_name(err)); goto fail; }
    gpio_set_level(PCNT_TEST_GPIO, 1);
    gpio_set_level(PCNT_TEST_GPIO, 0);
    pcnt_unit_get_count(unit, &count);
    if (count != 8) { FAIL_MSG(TN, "count moved while stopped: %d", count); goto fail; }

    pcnt_unit_disable(unit);
    pcnt_del_channel(chan);
    pcnt_del_unit(unit);
    return true;

fail:
    if (chan) pcnt_del_channel(chan);
    if (unit) { pcnt_unit_disable(unit); pcnt_del_unit(unit); }
    return false;
}
#endif /* SOC_PCNT_SUPPORTED */

#if SOC_I2C_SUPPORTED
/* Nothing is wired to these pins — the emulator's I2C model ignores the GPIO
 * matrix — but the driver validates them, so they must be real on every chip:
 * GPIO 4 is taken by LEDC/PCNT above and 8-19 collides with S31's EMAC. */
#define I2C_TEST_SDA 3
#define I2C_TEST_SCL 7
/* The emulator's built-in 24Cxx-style test slave (periph/i2c.rs). */
#define I2C_TEST_ADDR 0x50

/* Drive the real `i2c_master` driver against the built-in slave: probe finds
 * it and misses an empty address, then a register write and a
 * write-then-read (RSTART) round trip return what was stored. Everything
 * travels through the COMD/FIFO command sequence and the TRANS_COMPLETE
 * interrupt the driver waits on. */
bool test_i2c(void)
{
    const char *TN = "i2c";
    esp_err_t err;
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_TEST_SCL,
        .sda_io_num = I2C_TEST_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }

    err = i2c_master_probe(bus, I2C_TEST_ADDR, 500);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "probe 0x%02x: %s", I2C_TEST_ADDR, esp_err_to_name(err));
        goto fail;
    }
    /* An address with no device must NACK, not time out. */
    err = i2c_master_probe(bus, 0x55, 500);
    if (err != ESP_ERR_NOT_FOUND) {
        FAIL_MSG(TN, "probe of empty 0x55: %s (want NOT_FOUND)", esp_err_to_name(err));
        goto fail;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_TEST_ADDR,
        .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "bus_add_device: %s", esp_err_to_name(err));
        goto fail;
    }

    /* Write [reg=0x20, 0xA5, 0x5A], then read those two bytes back. */
    const uint8_t wr[] = { 0x20, 0xA5, 0x5A };
    err = i2c_master_transmit(dev, wr, sizeof(wr), 1000);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "i2c_master_transmit: %s", esp_err_to_name(err));
        goto fail;
    }

    const uint8_t reg = 0x20;
    uint8_t rd[2] = { 0, 0 };
    err = i2c_master_transmit_receive(dev, &reg, 1, rd, sizeof(rd), 1000);
    if (err != ESP_OK) {
        FAIL_MSG(TN, "i2c_master_transmit_receive: %s", esp_err_to_name(err));
        goto fail;
    }
    if (rd[0] != 0xA5 || rd[1] != 0x5A) {
        FAIL_MSG(TN, "read back 0x%02x 0x%02x (want 0xA5 0x5A)", rd[0], rd[1]);
        goto fail;
    }

    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    return true;

fail:
    if (dev) i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    return false;
}
#endif /* SOC_I2C_SUPPORTED */
