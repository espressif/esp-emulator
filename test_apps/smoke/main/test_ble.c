/*
 * BLE advertising via NimBLE.
 */
#include "test_smoke.h"

/* --------------- 10c. BLE advertise (chips with BLE) ---------------
 *
 * bleprph-style legacy advertisement carrying a known Complete Local Name.
 * The emulator's built-in BLE controller captures the advertising payload;
 * the smoke harness asserts the name is discoverable without an external
 * scanner. Runs entirely on the built-in controller (no real radio).
 *
 * Gated on CONFIG_BT_NIMBLE_ENABLED (set for C3/C5/C6/H2/S3/S31 in
 * sdkconfig.defaults.<target>): the emulator's built-in BLE controller models
 * C3 and S3 via the VHCI path (S3 with an Xtensa callback trampoline), C6
 * natively, H2 and C5 via the C6 HCI path, and S31 via its BTDM transport, so
 * targets without an HCI model leave NimBLE disabled and skip this test. */
#if CONFIG_BT_NIMBLE_ENABLED
#define EMU_BLE_DEVICE_NAME "esp-emu-ble"

static SemaphoreHandle_t g_ble_sem = NULL;
static uint8_t g_ble_own_addr_type;

static void emu_ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;

    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    if (ble_gap_adv_set_fields(&fields) != 0) { return; }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(g_ble_own_addr_type, NULL, BLE_HS_FOREVER,
                          &adv_params, NULL, NULL) != 0) {
        return;
    }

    ESP_LOGI(TAG, "  BLE: advertising as '%s'", name);
    xSemaphoreGive(g_ble_sem);  /* given only once advertising is live */
}

static void emu_ble_on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) { return; }
    if (ble_hs_id_infer_auto(0, &g_ble_own_addr_type) != 0) { return; }
    emu_ble_advertise();
}

static void emu_ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool test_ble(void)
{
    const char *TN = "ble";

    g_ble_sem = xSemaphoreCreateBinary();
    if (!g_ble_sem) { FAIL_MSG(TN, "create semaphore failed"); return false; }

    if (nimble_port_init() != ESP_OK) {
        FAIL_MSG(TN, "nimble_port_init failed");
        vSemaphoreDelete(g_ble_sem);
        return false;
    }

    ble_hs_cfg.sync_cb = emu_ble_on_sync;
    if (ble_svc_gap_device_name_set(EMU_BLE_DEVICE_NAME) != 0) {
        FAIL_MSG(TN, "set device name failed");
        goto fail;
    }

    nimble_port_freertos_init(emu_ble_host_task);

    /* The semaphore is given from emu_ble_advertise() only after
     * ble_gap_adv_start() succeeds, so taking it proves advertising is live. */
    if (xSemaphoreTake(g_ble_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        FAIL_MSG(TN, "advertising did not start within 10s");
        goto fail;
    }

    vSemaphoreDelete(g_ble_sem);
    return true;

fail:
    vSemaphoreDelete(g_ble_sem);
    return false;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */
