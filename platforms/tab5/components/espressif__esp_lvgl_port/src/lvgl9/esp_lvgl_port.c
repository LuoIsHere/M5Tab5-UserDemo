/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_priv.h"
#include "lvgl.h"

static const char *TAG = "LVGL";

#define ESP_LVGL_PORT_TASK_MUX_DELAY_MS 10000

/*******************************************************************************
 * Types definitions
 *******************************************************************************/

#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
/*
 * LVGL task checkpoints:
 * A: before acquiring lvgl_mux
 * B: after acquiring lvgl_mux
 * C: before acquiring timer_mux
 * D: after acquiring timer_mux
 * E: before the explicit lv_indev_read() call
 * F: after the explicit lv_indev_read() call
 * G: before lv_timer_handler()
 * H: after lv_timer_handler()
 * I: after releasing lvgl_mux
 *
 * E/F cover only event-driven input reads performed explicitly by
 * lvgl_port_task. Input-device timer callbacks run by lv_timer_handler()
 * are included between G and H instead.
 */
typedef struct {
    uint32_t a_before_lvgl_mux;
    uint32_t b_after_lvgl_mux;
    uint32_t c_before_timer_mux;
    uint32_t d_after_timer_mux;
    uint32_t e_before_indev_read;
    uint32_t f_after_indev_read;
    uint32_t g_before_timer_handler;
    uint32_t h_after_timer_handler;
    uint32_t i_after_lvgl_mux_release;
} lvgl_port_diagnostics_counters_t;
#endif

typedef struct lvgl_port_ctx_s {
    TaskHandle_t lvgl_task;
    SemaphoreHandle_t lvgl_mux;
    SemaphoreHandle_t timer_mux;
    EventGroupHandle_t lvgl_events;
    SemaphoreHandle_t task_init_mux;
    esp_timer_handle_t tick_timer;
#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
    TaskHandle_t diagnostics_task;
    SemaphoreHandle_t diagnostics_stopped_mux;
    bool diagnostics_running;
    lvgl_port_diagnostics_counters_t diagnostics;
#endif
    bool running;
    int task_max_sleep_ms;
    int timer_period_ms;
} lvgl_port_ctx_t;

/*******************************************************************************
 * Local variables
 *******************************************************************************/
static lvgl_port_ctx_t lvgl_port_ctx;

#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
#define LVGL_PORT_DIAGNOSTICS_PERIOD_MS 5000
#define LVGL_PORT_DIAGNOSTICS_TASK_STACK_SIZE 3072
#define LVGL_PORT_DIAGNOSTICS_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define LVGL_PORT_DIAGNOSTICS_HIT(field) \
    ((void)__atomic_add_fetch(&lvgl_port_ctx.diagnostics.field, 1U, __ATOMIC_RELAXED))
#else
#define LVGL_PORT_DIAGNOSTICS_HIT(field) ((void)0)
#endif

/*******************************************************************************
 * Function definitions
 *******************************************************************************/
static void lvgl_port_task(void *arg);
static esp_err_t lvgl_port_tick_init(void);
static void lvgl_port_task_deinit(void);
#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
static void lvgl_port_diagnostics_init(void);
static void lvgl_port_diagnostics_deinit(void);
static void lvgl_port_diagnostics_task(void *arg);
#endif

/*******************************************************************************
 * Public API functions
 *******************************************************************************/

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(cfg->task_affinity < (configNUM_CORES), ESP_ERR_INVALID_ARG, err, TAG,
                      "Bad core number for task! Maximum core number is %d", (configNUM_CORES - 1));

    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));

    /* Tick init */
    lvgl_port_ctx.timer_period_ms = cfg->timer_period_ms;
    /* Create task */
    lvgl_port_ctx.task_max_sleep_ms = cfg->task_max_sleep_ms;
    if (lvgl_port_ctx.task_max_sleep_ms == 0) {
        lvgl_port_ctx.task_max_sleep_ms = 500;
    }
    /* Timer semaphore */
    lvgl_port_ctx.timer_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.timer_mux, ESP_ERR_NO_MEM, err, TAG, "Create timer mutex fail!");
    /* LVGL semaphore */
    lvgl_port_ctx.lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.lvgl_mux, ESP_ERR_NO_MEM, err, TAG, "Create LVGL mutex fail!");
    /* Task init semaphore */
    lvgl_port_ctx.task_init_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.task_init_mux, ESP_ERR_NO_MEM, err, TAG, "Create LVGL task sem fail!");
    /* Task queue */
    lvgl_port_ctx.lvgl_events = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.lvgl_events, ESP_ERR_NO_MEM, err, TAG, "Create LVGL Event Group fail!");

    BaseType_t res;
    if (cfg->task_affinity < 0) {
        res = xTaskCreate(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority,
                          &lvgl_port_ctx.lvgl_task);
    } else {
        res = xTaskCreatePinnedToCore(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(),
                                      cfg->task_priority, &lvgl_port_ctx.lvgl_task, cfg->task_affinity);
    }
    ESP_GOTO_ON_FALSE(res == pdPASS, ESP_FAIL, err, TAG, "Create LVGL task fail!");

    // Wait until taskLVGL starts
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        ret = ESP_ERR_TIMEOUT;
    }

err:
    if (ret != ESP_OK) {
        lvgl_port_deinit();
    }

    return ret;
}

esp_err_t lvgl_port_resume(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (lvgl_port_ctx.tick_timer != NULL) {
        lv_timer_enable(true);
        ret = esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_ctx.timer_period_ms * 1000);
    }

    return ret;
}

esp_err_t lvgl_port_stop(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (lvgl_port_ctx.tick_timer != NULL) {
        lv_timer_enable(false);
        ret = esp_timer_stop(lvgl_port_ctx.tick_timer);
    }

    return ret;
}

esp_err_t lvgl_port_deinit(void)
{
    /* Stop and delete timer */
    if (lvgl_port_ctx.tick_timer != NULL) {
        esp_timer_stop(lvgl_port_ctx.tick_timer);
        esp_timer_delete(lvgl_port_ctx.tick_timer);
        lvgl_port_ctx.tick_timer = NULL;
    }

    /* Stop running task */
    if (lvgl_port_ctx.running) {
        lvgl_port_ctx.running = false;
    }

    /* Wait for stop task */
    if (xSemaphoreTake(lvgl_port_ctx.task_init_mux, pdMS_TO_TICKS(ESP_LVGL_PORT_TASK_MUX_DELAY_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to stop LVGL task");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Stopped LVGL task");

    lvgl_port_task_deinit();

    return ESP_OK;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");

    const TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_port_ctx.lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");
    xSemaphoreGiveRecursive(lvgl_port_ctx.lvgl_mux);
}

esp_err_t lvgl_port_task_wake(lvgl_port_event_type_t event, void *param)
{
    EventBits_t bits = 0;
    if (!lvgl_port_ctx.lvgl_events) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Get unprocessed bits */
    if (xPortInIsrContext() == pdTRUE) {
        bits = xEventGroupGetBitsFromISR(lvgl_port_ctx.lvgl_events);
    } else {
        bits = xEventGroupGetBits(lvgl_port_ctx.lvgl_events);
    }

    /* Set event */
    bits |= event;

    /* Save */
    if (xPortInIsrContext() == pdTRUE) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xEventGroupSetBitsFromISR(lvgl_port_ctx.lvgl_events, bits, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    } else {
        xEventGroupSetBits(lvgl_port_ctx.lvgl_events, bits);
    }

    return ESP_OK;
}

IRAM_ATTR bool lvgl_port_task_notify(uint32_t value)
{
    BaseType_t need_yield = pdFALSE;

    // Notify LVGL task
    if (xPortInIsrContext() == pdTRUE) {
        xTaskNotifyFromISR(lvgl_port_ctx.lvgl_task, value, eNoAction, &need_yield);
    } else {
        xTaskNotify(lvgl_port_ctx.lvgl_task, value, eNoAction);
    }

    return (need_yield == pdTRUE);
}

/*******************************************************************************
 * Private functions
 *******************************************************************************/

/*
 * LVGL diagnostic field:
 * - stack_hwm_bytes: minimum free task stack observed since task creation.
 */
static void lvgl_port_task(void *arg)
{
    TaskHandle_t task_to_notify = (TaskHandle_t)arg;
    EventBits_t events          = 0;
    uint32_t task_delay_ms      = 0;
    lv_indev_t *indev           = NULL;
    TickType_t last_stack_log_tick = xTaskGetTickCount();

    /* Take the task semaphore */
    if (xSemaphoreTake(lvgl_port_ctx.task_init_mux, 0) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take LVGL task sem");
        lvgl_port_task_deinit();
        vTaskDelete(NULL);
    }

    /* LVGL init */
    lv_init();
    /* LVGL is initialized, notify lvgl_port_init() function about it */
    xTaskNotifyGive(task_to_notify);
    /* Tick init */
    lvgl_port_tick_init();
#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
    lvgl_port_diagnostics_init();
#endif

    ESP_LOGI(TAG, "Starting LVGL task: priority=%u core=%d stack_hwm_bytes=%u",
             (unsigned)uxTaskPriorityGet(NULL), xPortGetCoreID(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    lvgl_port_ctx.running = true;
    while (lvgl_port_ctx.running) {
        /* Wait for queue or timeout (sleep task) */
        TickType_t wait = (pdMS_TO_TICKS(task_delay_ms) >= 1 ? pdMS_TO_TICKS(task_delay_ms) : 1);
        events          = xEventGroupWaitBits(lvgl_port_ctx.lvgl_events, 0xFF, pdTRUE, pdFALSE, wait);

        if (lv_display_get_default()) {
            LVGL_PORT_DIAGNOSTICS_HIT(a_before_lvgl_mux);
            if (lvgl_port_lock(0)) {
                LVGL_PORT_DIAGNOSTICS_HIT(b_after_lvgl_mux);

                /* Call read input devices */
                if (events & LVGL_PORT_EVENT_TOUCH) {
                    LVGL_PORT_DIAGNOSTICS_HIT(c_before_timer_mux);
                    xSemaphoreTake(lvgl_port_ctx.timer_mux, portMAX_DELAY);
                    LVGL_PORT_DIAGNOSTICS_HIT(d_after_timer_mux);
                    indev = lv_indev_get_next(NULL);
                    while (indev != NULL) {
                        LVGL_PORT_DIAGNOSTICS_HIT(e_before_indev_read);
                        lv_indev_read(indev);
                        LVGL_PORT_DIAGNOSTICS_HIT(f_after_indev_read);
                        indev = lv_indev_get_next(indev);
                    }
                    xSemaphoreGive(lvgl_port_ctx.timer_mux);
                }

                /*
                 * Handle all ready LVGL timers. The G-H interval can include
                 * input-device, display-refresh, animation, event, and user callbacks.
                 */
                LVGL_PORT_DIAGNOSTICS_HIT(g_before_timer_handler);
                task_delay_ms = lv_timer_handler();
                LVGL_PORT_DIAGNOSTICS_HIT(h_after_timer_handler);
                lvgl_port_unlock();
                LVGL_PORT_DIAGNOSTICS_HIT(i_after_lvgl_mux_release);
            } else {
                task_delay_ms = 1; /*Keep trying*/
            }
        } else {
            task_delay_ms = 1; /*Keep trying*/
        }

        if (task_delay_ms == LV_NO_TIMER_READY) {
            task_delay_ms = lvgl_port_ctx.task_max_sleep_ms;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "LVGL task stack_hwm_bytes=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stack_log_tick = now;
        }

        /* Minimal delay for the task. When there are too many events, leave time for other tasks and interrupts. */
        vTaskDelay(1);
    }

    /* Give semaphore back */
    xSemaphoreGive(lvgl_port_ctx.task_init_mux);

    /* Close task */
    vTaskDelete(NULL);
}

static void lvgl_port_task_deinit(void)
{
#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
    lvgl_port_diagnostics_deinit();
#endif
    if (lvgl_port_ctx.timer_mux) {
        vSemaphoreDelete(lvgl_port_ctx.timer_mux);
    }
    if (lvgl_port_ctx.lvgl_mux) {
        vSemaphoreDelete(lvgl_port_ctx.lvgl_mux);
    }
    if (lvgl_port_ctx.task_init_mux) {
        vSemaphoreDelete(lvgl_port_ctx.task_init_mux);
    }
    if (lvgl_port_ctx.lvgl_events) {
        vEventGroupDelete(lvgl_port_ctx.lvgl_events);
    }
    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    /* Deinitialize LVGL */
    lv_deinit();
#endif
}

#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
static void lvgl_port_diagnostics_task(void *arg)
{
    (void)arg;

    while (__atomic_load_n(&lvgl_port_ctx.diagnostics_running, __ATOMIC_RELAXED)) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LVGL_PORT_DIAGNOSTICS_PERIOD_MS));
        if (!__atomic_load_n(&lvgl_port_ctx.diagnostics_running, __ATOMIC_RELAXED)) {
            break;
        }

        ESP_LOGI(TAG,
                 "LVGL checkpoints: A=%u B=%u C=%u D=%u E=%u F=%u G=%u H=%u I=%u",
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.a_before_lvgl_mux, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.b_after_lvgl_mux, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.c_before_timer_mux, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.d_after_timer_mux, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.e_before_indev_read, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.f_after_indev_read, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.g_before_timer_handler, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.h_after_timer_handler, __ATOMIC_RELAXED),
                 (unsigned)__atomic_load_n(&lvgl_port_ctx.diagnostics.i_after_lvgl_mux_release, __ATOMIC_RELAXED));
    }

    xSemaphoreGive(lvgl_port_ctx.diagnostics_stopped_mux);
    vTaskDelete(NULL);
}

static void lvgl_port_diagnostics_init(void)
{
    lvgl_port_ctx.diagnostics_stopped_mux = xSemaphoreCreateBinary();
    if (lvgl_port_ctx.diagnostics_stopped_mux == NULL) {
        ESP_LOGW(TAG, "Failed to create LVGL diagnostics stop semaphore");
        return;
    }

    __atomic_store_n(&lvgl_port_ctx.diagnostics_running, true, __ATOMIC_RELAXED);
    BaseType_t res = xTaskCreate(lvgl_port_diagnostics_task, "lvglDiag", LVGL_PORT_DIAGNOSTICS_TASK_STACK_SIZE, NULL,
                                 LVGL_PORT_DIAGNOSTICS_TASK_PRIORITY, &lvgl_port_ctx.diagnostics_task);
    if (res != pdPASS) {
        __atomic_store_n(&lvgl_port_ctx.diagnostics_running, false, __ATOMIC_RELAXED);
        vSemaphoreDelete(lvgl_port_ctx.diagnostics_stopped_mux);
        lvgl_port_ctx.diagnostics_stopped_mux = NULL;
        ESP_LOGW(TAG, "Failed to create LVGL checkpoint diagnostics task");
        return;
    }

    ESP_LOGI(TAG, "LVGL checkpoint diagnostics enabled (5 s interval)");
    ESP_LOGI(TAG, "A/B=LVGL mutex before/after, C/D=timer mutex before/after, E/F=indev read before/after");
    ESP_LOGI(TAG, "G/H=timer handler before/after, I=LVGL mutex released");
}

static void lvgl_port_diagnostics_deinit(void)
{
    if (lvgl_port_ctx.diagnostics_task != NULL) {
        __atomic_store_n(&lvgl_port_ctx.diagnostics_running, false, __ATOMIC_RELAXED);
        xTaskNotifyGive(lvgl_port_ctx.diagnostics_task);
        if (lvgl_port_ctx.diagnostics_stopped_mux == NULL ||
            xSemaphoreTake(lvgl_port_ctx.diagnostics_stopped_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "LVGL checkpoint diagnostics task did not stop cleanly");
            vTaskDelete(lvgl_port_ctx.diagnostics_task);
        }
        lvgl_port_ctx.diagnostics_task = NULL;
    }

    if (lvgl_port_ctx.diagnostics_stopped_mux != NULL) {
        vSemaphoreDelete(lvgl_port_ctx.diagnostics_stopped_mux);
        lvgl_port_ctx.diagnostics_stopped_mux = NULL;
    }
}
#endif

static void lvgl_port_tick_increment(void *arg)
{
    xSemaphoreTake(lvgl_port_ctx.timer_mux, portMAX_DELAY);
    /* Tell LVGL how many milliseconds have elapsed */
    lv_tick_inc(lvgl_port_ctx.timer_period_ms);
    xSemaphoreGive(lvgl_port_ctx.timer_mux);
}

static esp_err_t lvgl_port_tick_init(void)
{
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_port_tick_increment,
        .name     = "LVGL tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_port_ctx.tick_timer), TAG,
                        "Creating LVGL timer filed!");
    return esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_ctx.timer_period_ms * 1000);
}
