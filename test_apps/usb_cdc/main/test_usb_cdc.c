/*
 * TinyUSB CDC-ACM console on every USB-OTG port the chip has.
 *
 * Boot logs go to UART0. For each port, tinyusb_console_init() moves stdio to
 * the CDC port, so its RESULT_ lines reach the host only if the emulated
 * controller enumerated and carried the bulk IN data. The host then types
 * "ping"; the echo proves the bulk OUT direction. Chips with two controllers
 * (P4: HS, then FS) tear the stack down and bring it up on the next port.
 *
 * Output waits for the host to open the port (DTR): TinyUSB empties the CDC
 * TX FIFO when the configuration is set, so anything printed earlier is lost,
 * on silicon as much as here.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#define OPEN_TIMEOUT_US (5 * 1000 * 1000)
#define ECHO_TIMEOUT_US (20 * 1000 * 1000)
/* Long enough for the host to collect the last bulk IN before teardown. */
#define DRAIN_MS        50

static void read_line(char *line, size_t size)
{
    size_t n = 0;
    const int64_t deadline = esp_timer_get_time() + ECHO_TIMEOUT_US;
    while (n + 1 < size && esp_timer_get_time() < deadline) {
        int c = fgetc(stdin);
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(1);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                break;
            }
            continue;
        }
        line[n++] = (char)c;
    }
    line[n] = '\0';
}

static void console_up(tinyusb_port_t port)
{
    printf("usb_cdc: installing TinyUSB on port %d\n", port);
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.port = port;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
    setvbuf(stdout, NULL, _IONBF, 0);

    const int64_t open_deadline = esp_timer_get_time() + OPEN_TIMEOUT_US;
    while (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0) && esp_timer_get_time() < open_deadline) {
        vTaskDelay(1);
    }
}

static void console_down(void)
{
    vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    ESP_ERROR_CHECK(tinyusb_console_deinit(TINYUSB_CDC_ACM_0));
    ESP_ERROR_CHECK(tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0));
    ESP_ERROR_CHECK(tinyusb_driver_uninstall());
}

/* Reaching the host at all is the IN case; the echo is the OUT case. */
static void test_port(tinyusb_port_t port, int *passed, int *failed)
{
    printf("RESULT_CDC_HELLO_P%d: stdout over TinyUSB CDC-ACM\n", port);
    printf("RESULT_CDC_PROMPT_P%d:\n", port);

    char line[32];
    read_line(line, sizeof(line));
    printf("RESULT_CDC_ECHO_P%d: %s\n", port, line);

    *passed += 1;
    if (strcmp(line, "ping") == 0) {
        *passed += 1;
    } else {
        *failed += 1;
        printf("FAIL cdc_echo_p%d: expected \"ping\", got \"%s\"\n", port, line);
    }
}

void app_main(void)
{
#if SOC_USB_OTG_PERIPH_NUM > 1
    const tinyusb_port_t ports[] = { TINYUSB_PORT_HIGH_SPEED_0, TINYUSB_PORT_FULL_SPEED_0 };
#else
    const tinyusb_port_t ports[] = { TINYUSB_PORT_FULL_SPEED_0 };
#endif
    const size_t n_ports = sizeof(ports) / sizeof(ports[0]);
    int passed = 0;
    int failed = 0;

    for (size_t i = 0; i < n_ports; i++) {
        console_up(ports[i]);
        test_port(ports[i], &passed, &failed);
        if (i + 1 < n_ports) {
            console_down();
        }
    }
    printf("RESULT_SUMMARY: passed=%d failed=%d target=%s\n", passed, failed, CONFIG_IDF_TARGET);
}
