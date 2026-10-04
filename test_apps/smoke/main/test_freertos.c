/*
 * FreeRTOS primitives, pthread and SMP.
 */
#include "test_smoke.h"

/* --------------- 8. FreeRTOS primitives --------------- */

/* 8a. Task creation */
static volatile bool g_task_flag = false;
static SemaphoreHandle_t g_task_sem = NULL;

static void test_task_fn(void *arg)
{
    g_task_flag = true;
    xSemaphoreGive(g_task_sem);
    vTaskDelete(NULL);
}

bool test_freertos_task(void)
{
    const char *TN = "freertos_task";

    g_task_flag = false;
    g_task_sem = xSemaphoreCreateBinary();
    if (!g_task_sem) {
        FAIL_MSG(TN, "failed to create semaphore");
        return false;
    }

    BaseType_t ret = xTaskCreate(test_task_fn, "tst_task", 2048, NULL, 5, NULL);
    if (ret != pdPASS) {
        FAIL_MSG(TN, "xTaskCreate failed");
        vSemaphoreDelete(g_task_sem);
        return false;
    }

    if (xSemaphoreTake(g_task_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "task did not signal within 500ms");
        vSemaphoreDelete(g_task_sem);
        return false;
    }

    if (!g_task_flag) {
        FAIL_MSG(TN, "flag not set");
        vSemaphoreDelete(g_task_sem);
        return false;
    }

    vSemaphoreDelete(g_task_sem);
    return true;
}

/* 8b. Binary semaphore */
static SemaphoreHandle_t g_bsem = NULL;

static void sem_giver_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreGive(g_bsem);
    vTaskDelete(NULL);
}

bool test_freertos_semaphore(void)
{
    const char *TN = "freertos_semaphore";

    g_bsem = xSemaphoreCreateBinary();
    if (!g_bsem) {
        FAIL_MSG(TN, "create sem failed");
        return false;
    }

    xTaskCreate(sem_giver_task, "sem_give", 2048, NULL, 5, NULL);

    if (xSemaphoreTake(g_bsem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "semaphore not given within 500ms");
        vSemaphoreDelete(g_bsem);
        return false;
    }

    vSemaphoreDelete(g_bsem);
    return true;
}

/* 8c. Queue */
static QueueHandle_t g_queue = NULL;

static void queue_sender_task(void *arg)
{
    int val = 12345;
    xQueueSend(g_queue, &val, portMAX_DELAY);
    vTaskDelete(NULL);
}

bool test_freertos_queue(void)
{
    const char *TN = "freertos_queue";

    g_queue = xQueueCreate(1, sizeof(int));
    if (!g_queue) {
        FAIL_MSG(TN, "create queue failed");
        return false;
    }

    xTaskCreate(queue_sender_task, "q_send", 2048, NULL, 5, NULL);

    int received = 0;
    if (xQueueReceive(g_queue, &received, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "queue receive timed out");
        vQueueDelete(g_queue);
        return false;
    }

    if (received != 12345) {
        FAIL_MSG(TN, "expected 12345, got %d", received);
        vQueueDelete(g_queue);
        return false;
    }

    vQueueDelete(g_queue);
    return true;
}

/* 8d. Task notification */
static TaskHandle_t g_notify_task = NULL;
static volatile bool g_notify_received = false;
static SemaphoreHandle_t g_notify_sem = NULL;

static void notify_waiter_task(void *arg)
{
    uint32_t val = 0;
    if (xTaskNotifyWait(0, ULONG_MAX, &val, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (val == 0xABCD) {
            g_notify_received = true;
        }
    }
    xSemaphoreGive(g_notify_sem);
    vTaskDelete(NULL);
}

bool test_freertos_notification(void)
{
    const char *TN = "freertos_notification";

    g_notify_received = false;
    g_notify_sem = xSemaphoreCreateBinary();
    if (!g_notify_sem) {
        FAIL_MSG(TN, "create sem failed");
        return false;
    }

    xTaskCreate(notify_waiter_task, "notify_w", 2048, NULL, 5, &g_notify_task);
    vTaskDelay(pdMS_TO_TICKS(10)); /* let waiter start */

    xTaskNotify(g_notify_task, 0xABCD, eSetValueWithOverwrite);

    if (xSemaphoreTake(g_notify_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "waiter did not complete within 500ms");
        vSemaphoreDelete(g_notify_sem);
        return false;
    }

    if (!g_notify_received) {
        FAIL_MSG(TN, "notification value mismatch");
        vSemaphoreDelete(g_notify_sem);
        return false;
    }

    vSemaphoreDelete(g_notify_sem);
    return true;
}

/* --------------- 11. FreeRTOS mutex --------------- */

static SemaphoreHandle_t g_mtx = NULL;
static volatile int g_mtx_counter = 0;
static SemaphoreHandle_t g_mtx_done;

static void mutex_worker(void *arg)
{
    for (int i = 0; i < 100; i++) {
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        int v = g_mtx_counter;
        vTaskDelay(1); /* force a yield while holding the lock */
        g_mtx_counter = v + 1;
        xSemaphoreGive(g_mtx);
    }
    xSemaphoreGive(g_mtx_done);
    vTaskDelete(NULL);
}

bool test_freertos_mutex(void)
{
    const char *TN = "freertos_mutex";
    g_mtx = xSemaphoreCreateMutex();
    g_mtx_done = xSemaphoreCreateCounting(2, 0);
    g_mtx_counter = 0;
    if (!g_mtx || !g_mtx_done) { FAIL_MSG(TN, "create failed"); return false; }

    xTaskCreate(mutex_worker, "mtx_a", 2048, NULL, 5, NULL);
    xTaskCreate(mutex_worker, "mtx_b", 2048, NULL, 5, NULL);

    for (int i = 0; i < 2; i++) {
        if (xSemaphoreTake(g_mtx_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            FAIL_MSG(TN, "worker %d did not finish", i);
            goto fail;
        }
    }
    if (g_mtx_counter != 200) {
        FAIL_MSG(TN, "expected counter=200, got %d (lost increments)", g_mtx_counter);
        goto fail;
    }
    vSemaphoreDelete(g_mtx);
    vSemaphoreDelete(g_mtx_done);
    return true;
fail:
    vSemaphoreDelete(g_mtx);
    vSemaphoreDelete(g_mtx_done);
    return false;
}

/* --------------- 12. FreeRTOS counting semaphore --------------- */

bool test_freertos_counting_sem(void)
{
    const char *TN = "freertos_counting_sem";
    SemaphoreHandle_t s = xSemaphoreCreateCounting(5, 0);
    if (!s) { FAIL_MSG(TN, "create failed"); return false; }

    /* Should fail to take when count=0 */
    if (xSemaphoreTake(s, 0) == pdTRUE) {
        FAIL_MSG(TN, "took empty counting sem");
        vSemaphoreDelete(s);
        return false;
    }

    for (int i = 0; i < 3; i++) xSemaphoreGive(s);

    /* Now should take exactly 3 times */
    for (int i = 0; i < 3; i++) {
        if (xSemaphoreTake(s, 0) != pdTRUE) {
            FAIL_MSG(TN, "expected take #%d to succeed", i);
            vSemaphoreDelete(s);
            return false;
        }
    }
    if (xSemaphoreTake(s, 0) == pdTRUE) {
        FAIL_MSG(TN, "took after draining");
        vSemaphoreDelete(s);
        return false;
    }
    vSemaphoreDelete(s);
    return true;
}

/* --------------- 13. FreeRTOS event group --------------- */

#define EVT_BIT_A (1 << 0)
#define EVT_BIT_B (1 << 1)
#define EVT_BIT_C (1 << 2)

static EventGroupHandle_t g_evt = NULL;

static void evt_setter(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(20));
    xEventGroupSetBits(g_evt, EVT_BIT_A);
    vTaskDelay(pdMS_TO_TICKS(20));
    xEventGroupSetBits(g_evt, EVT_BIT_B | EVT_BIT_C);
    vTaskDelete(NULL);
}

bool test_freertos_event_group(void)
{
    const char *TN = "freertos_event_group";
    g_evt = xEventGroupCreate();
    if (!g_evt) { FAIL_MSG(TN, "create failed"); return false; }

    xTaskCreate(evt_setter, "evt_set", 2048, NULL, 5, NULL);

    EventBits_t bits = xEventGroupWaitBits(
        g_evt,
        EVT_BIT_A | EVT_BIT_B | EVT_BIT_C,
        pdTRUE,  /* clear on exit */
        pdTRUE,  /* wait for all */
        pdMS_TO_TICKS(500));

    if ((bits & (EVT_BIT_A | EVT_BIT_B | EVT_BIT_C)) !=
        (EVT_BIT_A | EVT_BIT_B | EVT_BIT_C)) {
        FAIL_MSG(TN, "missing bits, got 0x%x", (unsigned)bits);
        vEventGroupDelete(g_evt);
        return false;
    }
    /* Bits should be cleared on exit */
    EventBits_t after = xEventGroupGetBits(g_evt);
    if (after & (EVT_BIT_A | EVT_BIT_B | EVT_BIT_C)) {
        FAIL_MSG(TN, "bits not cleared after wait, got 0x%x", (unsigned)after);
        vEventGroupDelete(g_evt);
        return false;
    }
    vEventGroupDelete(g_evt);
    return true;
}

/* --------------- 14. FreeRTOS software timer --------------- */

static volatile int g_swtimer_count = 0;
static SemaphoreHandle_t g_swtimer_sem = NULL;

static void swtimer_cb(TimerHandle_t t)
{
    if (++g_swtimer_count >= 3) {
        xSemaphoreGive(g_swtimer_sem);
    }
}

bool test_freertos_software_timer(void)
{
    const char *TN = "freertos_software_timer";
    bool ok = false;
    g_swtimer_count = 0;
    g_swtimer_sem = xSemaphoreCreateBinary();
    TimerHandle_t t = xTimerCreate("sw", pdMS_TO_TICKS(20), pdTRUE, NULL, swtimer_cb);
    if (!t) { FAIL_MSG(TN, "create failed"); goto out; }
    if (xTimerStart(t, 0) != pdPASS) { FAIL_MSG(TN, "start failed"); goto out; }
    if (xSemaphoreTake(g_swtimer_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        FAIL_MSG(TN, "did not fire 3 times in 500ms (got %d)", g_swtimer_count);
        goto out;
    }
    ok = true;
out:
    if (t) { xTimerStop(t, 0); xTimerDelete(t, 0); }
    vSemaphoreDelete(g_swtimer_sem);
    return ok;
}

/* --------------- 16. pthread create/mutex/cond/join --------------- */

static pthread_mutex_t g_pth_mtx;
static pthread_cond_t g_pth_cv;
static int g_pth_state = 0;

static void *pth_worker(void *arg)
{
    pthread_mutex_lock(&g_pth_mtx);
    while (g_pth_state != 1) {
        pthread_cond_wait(&g_pth_cv, &g_pth_mtx);
    }
    g_pth_state = 2;
    pthread_cond_signal(&g_pth_cv);
    pthread_mutex_unlock(&g_pth_mtx);
    return (void *)(intptr_t)0xCAFE;
}

bool test_pthread(void)
{
    const char *TN = "pthread";
    pthread_t th;
    if (pthread_mutex_init(&g_pth_mtx, NULL) != 0) { FAIL_MSG(TN, "mtx init"); return false; }
    if (pthread_cond_init(&g_pth_cv, NULL) != 0)   { FAIL_MSG(TN, "cv init");  return false; }
    g_pth_state = 0;

    if (pthread_create(&th, NULL, pth_worker, NULL) != 0) {
        FAIL_MSG(TN, "pthread_create failed"); return false;
    }

    /* Hand off via cond */
    pthread_mutex_lock(&g_pth_mtx);
    g_pth_state = 1;
    pthread_cond_signal(&g_pth_cv);
    while (g_pth_state != 2) {
        pthread_cond_wait(&g_pth_cv, &g_pth_mtx);
    }
    pthread_mutex_unlock(&g_pth_mtx);

    void *retval = NULL;
    if (pthread_join(th, &retval) != 0) {
        FAIL_MSG(TN, "pthread_join failed"); return false;
    }
    if (retval != (void *)(intptr_t)0xCAFE) {
        FAIL_MSG(TN, "join retval mismatch: %p", retval); return false;
    }
    pthread_cond_destroy(&g_pth_cv);
    pthread_mutex_destroy(&g_pth_mtx);
    return true;
}

/* --------------- 18. SMP pinned tasks (multi-core only) --------------- */

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static volatile int g_smp_core_seen[2] = {-1, -1};
static SemaphoreHandle_t g_smp_done;

static void smp_pinned(void *arg)
{
    int idx = (int)(intptr_t)arg;
    g_smp_core_seen[idx] = xPortGetCoreID();
    xSemaphoreGive(g_smp_done);
    vTaskDelete(NULL);
}

bool test_smp_pinned(void)
{
    const char *TN = "smp_pinned";
    g_smp_core_seen[0] = g_smp_core_seen[1] = -1;
    g_smp_done = xSemaphoreCreateCounting(2, 0);

    xTaskCreatePinnedToCore(smp_pinned, "pin0", 2048, (void *)(intptr_t)0, 5, NULL, 0);
    xTaskCreatePinnedToCore(smp_pinned, "pin1", 2048, (void *)(intptr_t)1, 5, NULL, 1);

    for (int i = 0; i < 2; i++) {
        if (xSemaphoreTake(g_smp_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            FAIL_MSG(TN, "pinned task %d did not run", i);
            vSemaphoreDelete(g_smp_done);
            return false;
        }
    }
    ESP_LOGI(TAG, "  core_seen=[%d, %d]", g_smp_core_seen[0], g_smp_core_seen[1]);
    if (g_smp_core_seen[0] != 0 || g_smp_core_seen[1] != 1) {
        FAIL_MSG(TN, "affinity broken: got [%d,%d]",
                 g_smp_core_seen[0], g_smp_core_seen[1]);
        vSemaphoreDelete(g_smp_done);
        return false;
    }
    vSemaphoreDelete(g_smp_done);
    return true;
}
#endif /* CONFIG_FREERTOS_NUMBER_OF_CORES > 1 */
