/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <app.h>
#include <hal/hal.h>
#include <memory>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "tab5x_userdemo";

#if CONFIG_TAB5X_DRIVER_SMOKE_TEST
static void driver_smoke_test_task(void*)
{
    constexpr TickType_t audio_timeout = pdMS_TO_TICKS(20000);
    constexpr TickType_t camera_run    = pdMS_TO_TICKS(5000);

    // Let the startup animation and its audio finish before exercising the shared codec.
    vTaskDelay(pdMS_TO_TICKS(12000));

    while (true) {
        ESP_LOGI(TAG, "TAB5X_AUDIO_DRIVER_TEST_TRIGGER");
        GetHAL()->startDualMicRecordTest();

        bool audio_started = false;
        TickType_t started_at = xTaskGetTickCount();
        while (xTaskGetTickCount() - started_at < audio_timeout) {
            auto state = GetHAL()->getDualMicRecordTestState();
            audio_started |= state != hal::HalBase::MIC_TEST_IDLE;
            if (audio_started && state == hal::HalBase::MIC_TEST_IDLE) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!audio_started || GetHAL()->getDualMicRecordTestState() != hal::HalBase::MIC_TEST_IDLE) {
            ESP_LOGE(TAG, "TAB5X_AUDIO_DRIVER_TEST_TIMEOUT");
        }

        ESP_LOGI(TAG, "TAB5X_CAMERA_DRIVER_TEST_TRIGGER");
        GetHAL()->lvglLock();
        lv_obj_t* camera_canvas = lv_canvas_create(lv_screen_active());
        lv_obj_set_size(camera_canvas, 1280, 720);
        lv_obj_add_flag(camera_canvas, LV_OBJ_FLAG_HIDDEN);
        GetHAL()->lvglUnlock();

        GetHAL()->startCameraCapture(camera_canvas);
        vTaskDelay(camera_run);
        GetHAL()->stopCameraCapture();

        started_at = xTaskGetTickCount();
        while (GetHAL()->isCameraCapturing() && xTaskGetTickCount() - started_at < pdMS_TO_TICKS(5000)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (!GetHAL()->isCameraCapturing()) {
            GetHAL()->lvglLock();
            lv_obj_delete(camera_canvas);
            GetHAL()->lvglUnlock();
        } else {
            ESP_LOGE(TAG, "TAB5X_CAMERA_DRIVER_TEST_TIMEOUT");
        }

        ESP_LOGI(TAG, "TAB5X_DRIVER_SMOKE_DONE");
        // Repeat so the test center can observe a full cycle after a fixture OTA rollback.
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
#endif

extern "C" void app_main(void)
{
    // 应用层初始化回调
    app::InitCallback_t callback;

    callback.onHalInjection = []() {
        // 注入桌面平台的硬件抽象
        hal::Inject(std::make_unique<HalEsp32>());
    };

    // 应用层启动
    app::Init(callback);
    ESP_LOGI(TAG, "TAB5X_DISPLAY_READY");
#if CONFIG_TAB5X_DRIVER_SMOKE_TEST
    xTaskCreate(driver_smoke_test_task, "driver_smoke", 4096, nullptr, 5, nullptr);
#endif
    while (!app::IsDone()) {
        app::Update();
        vTaskDelay(1);
    }
    app::Destroy();
}
