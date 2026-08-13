#include "tab5x_lvgl_diagnostics.h"

#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)

#include <stdbool.h>
#include <stddef.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LVGL_DIAGNOSTICS_PERIOD_MS 5000
#define LVGL_DIAGNOSTICS_TASK_STACK_SIZE 4096
#define LVGL_DIAGNOSTICS_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

static const char *TAG = "LVGL";

static uint32_t s_counters[TAB5X_LVGL_DIAG_COUNT];
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped_mux;
static bool s_running;
static const void *s_last_timer;
static const void *s_last_timer_callback;
static int32_t s_last_draw_result;

static uint32_t checkpoint_value(tab5x_lvgl_diag_checkpoint_t checkpoint)
{
    return __atomic_load_n(&s_counters[checkpoint], __ATOMIC_RELAXED);
}

static void diagnostics_task(void *arg)
{
    (void)arg;

    while (__atomic_load_n(&s_running, __ATOMIC_RELAXED)) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LVGL_DIAGNOSTICS_PERIOD_MS));
        if (!__atomic_load_n(&s_running, __ATOMIC_RELAXED)) {
            break;
        }

        ESP_LOGI(TAG, "LVGL checkpoints 1: A=%u B=%u C=%u D=%u E=%u F=%u G=%u H=%u I=%u",
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_A),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_B),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_C),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_D),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_E),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_F),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_G),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_H),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_I));
        ESP_LOGI(TAG, "LVGL checkpoints 2: J=%u K=%u L=%u M=%u N=%u O=%u P=%u Q=%u",
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_J),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_K),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_L),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_M),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_N),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_O),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_P),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_Q));
        ESP_LOGI(TAG, "LVGL checkpoints 3: R=%u S=%u T=%u U=%u V=%u W=%u X=%u Y=%u",
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_R),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_S),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_T),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_U),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_V),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_W),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_X),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_Y));
        ESP_LOGI(TAG, "LVGL checkpoints 4: Z=%u AA=%u AB=%u AC=%u AD=%u AE=%u AF=%u AG=%u",
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_Z),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AA),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AB),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AC),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AD),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AE),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AF),
                 (unsigned)checkpoint_value(TAB5X_LVGL_DIAG_AG));
        ESP_LOGI(TAG, "LVGL details: timer=%p callback=%p last_draw_result=%ld",
                 __atomic_load_n(&s_last_timer, __ATOMIC_RELAXED),
                 __atomic_load_n(&s_last_timer_callback, __ATOMIC_RELAXED),
                 (long)__atomic_load_n(&s_last_draw_result, __ATOMIC_RELAXED));
    }

    if (s_stopped_mux != NULL) {
        xSemaphoreGive(s_stopped_mux);
    }
    vTaskDelete(NULL);
}

void tab5x_lvgl_diagnostics_init(void)
{
    if (s_task != NULL) {
        return;
    }

    for (size_t i = 0; i < TAB5X_LVGL_DIAG_COUNT; ++i) {
        __atomic_store_n(&s_counters[i], 0U, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_last_timer, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_timer_callback, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_draw_result, 0, __ATOMIC_RELAXED);

    s_stopped_mux = xSemaphoreCreateBinary();
    if (s_stopped_mux == NULL) {
        ESP_LOGW(TAG, "Failed to create LVGL diagnostics stop semaphore");
        return;
    }

    __atomic_store_n(&s_running, true, __ATOMIC_RELAXED);
    BaseType_t result = xTaskCreate(diagnostics_task, "lvglDiag", LVGL_DIAGNOSTICS_TASK_STACK_SIZE, NULL,
                                    LVGL_DIAGNOSTICS_TASK_PRIORITY, &s_task);
    if (result != pdPASS) {
        __atomic_store_n(&s_running, false, __ATOMIC_RELAXED);
        vSemaphoreDelete(s_stopped_mux);
        s_stopped_mux = NULL;
        ESP_LOGW(TAG, "Failed to create LVGL checkpoint diagnostics task");
        return;
    }

    ESP_LOGI(TAG, "LVGL checkpoint diagnostics enabled (A-AG, 5 s interval)");
    ESP_LOGI(TAG, "A-I=LVGL task, J-M=timer/refresh, N-Q=rotation/PPA, R-Y=flush path");
    ESP_LOGI(TAG, "Z-AE=input callbacks, AF/AG=camera PPA; see source comments for exact meanings");
}

void tab5x_lvgl_diagnostics_deinit(void)
{
    if (s_task != NULL) {
        __atomic_store_n(&s_running, false, __ATOMIC_RELAXED);
        xTaskNotifyGive(s_task);
        if (s_stopped_mux == NULL ||
            xSemaphoreTake(s_stopped_mux, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "LVGL checkpoint diagnostics task did not stop cleanly");
            vTaskDelete(s_task);
        }
        s_task = NULL;
    }

    if (s_stopped_mux != NULL) {
        vSemaphoreDelete(s_stopped_mux);
        s_stopped_mux = NULL;
    }
}

void IRAM_ATTR tab5x_lvgl_diagnostics_hit(tab5x_lvgl_diag_checkpoint_t checkpoint)
{
    if ((unsigned)checkpoint < (unsigned)TAB5X_LVGL_DIAG_COUNT) {
        (void)__atomic_add_fetch(&s_counters[checkpoint], 1U, __ATOMIC_RELAXED);
    }
}

void tab5x_lvgl_diagnostics_set_last_timer(const void *timer, const void *callback)
{
    __atomic_store_n(&s_last_timer, timer, __ATOMIC_RELAXED);
    __atomic_store_n(&s_last_timer_callback, callback, __ATOMIC_RELAXED);
}

void tab5x_lvgl_diagnostics_set_last_draw_result(int32_t result)
{
    __atomic_store_n(&s_last_draw_result, result, __ATOMIC_RELAXED);
}

#endif
