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
#include <algorithm>
#include <cmath>
#include <ctime>
#include <iterator>

static const char* TAG = "tab5x_userdemo";

#if CONFIG_TAB5X_DRIVER_SMOKE_TEST
static void run_system_sensor_smoke()
{
    auto* hal = GetHAL();

    hal->updateImuData();
    const auto& imu   = hal->imuData;
    const float accel = std::sqrt(imu.accelX * imu.accelX + imu.accelY * imu.accelY + imu.accelZ * imu.accelZ);
    const bool imu_ok = std::isfinite(accel) && accel > 0.05f && accel < 100.0f && std::isfinite(imu.gyroX) &&
                        std::isfinite(imu.gyroY) && std::isfinite(imu.gyroZ);
    if (imu_ok) {
        ESP_LOGI(TAG, "TAB5X_IMU_DRIVER_TEST_PASS accel=%.3f gyro=%.3f,%.3f,%.3f", accel, imu.gyroX, imu.gyroY,
                 imu.gyroZ);
    } else {
        ESP_LOGE(TAG, "TAB5X_IMU_DRIVER_TEST_FAIL accel=%.3f gyro=%.3f,%.3f,%.3f", accel, imu.gyroX, imu.gyroY,
                 imu.gyroZ);
    }

    hal->updatePowerMonitorData();
    const auto& power = hal->powerMonitorData;
    const bool power_ok =
        std::isfinite(power.busVoltage) && power.busVoltage > 0.1f && power.busVoltage < 10.0f &&
        std::isfinite(power.shuntVoltage) && std::isfinite(power.busPower) && std::isfinite(power.shuntCurrent);
    if (power_ok) {
        ESP_LOGI(TAG, "TAB5X_POWER_MONITOR_TEST_PASS bus_v=%.4f shunt_v=%.6f power=%.4f current=%.4f",
                 power.busVoltage, power.shuntVoltage, power.busPower, power.shuntCurrent);
    } else {
        ESP_LOGE(TAG, "TAB5X_POWER_MONITOR_TEST_FAIL bus_v=%.4f shunt_v=%.6f power=%.4f current=%.4f",
                 power.busVoltage, power.shuntVoltage, power.busPower, power.shuntCurrent);
    }

    const int cpu_temp    = hal->getCpuTemp();
    const bool cpu_temp_ok = cpu_temp >= 0 && cpu_temp <= 120;
    if (cpu_temp_ok) {
        ESP_LOGI(TAG, "TAB5X_CPU_TEMP_TEST_PASS celsius=%d", cpu_temp);
    } else {
        ESP_LOGE(TAG, "TAB5X_CPU_TEMP_TEST_FAIL celsius=%d", cpu_temp);
    }

    const auto addresses = hal->i2cScan(true);
    constexpr uint8_t required_addresses[] = {0x32, 0x36, 0x41, 0x68};
    const bool i2c_ok = std::all_of(std::begin(required_addresses), std::end(required_addresses),
                                    [&addresses](uint8_t address) {
                                        return std::find(addresses.begin(), addresses.end(), address) != addresses.end();
                                    });
    if (i2c_ok) {
        ESP_LOGI(TAG, "TAB5X_I2C_REQUIRED_DEVICES_TEST_PASS count=%u", static_cast<unsigned>(addresses.size()));
    } else {
        ESP_LOGE(TAG, "TAB5X_I2C_REQUIRED_DEVICES_TEST_FAIL count=%u", static_cast<unsigned>(addresses.size()));
    }

    struct tm rtc_before = {};
    struct tm rtc_after  = {};
    hal->getRtcTime(&rtc_before);
    vTaskDelay(pdMS_TO_TICKS(2100));
    hal->getRtcTime(&rtc_after);
    const bool rtc_fields_ok = rtc_before.tm_mon >= 0 && rtc_before.tm_mon <= 11 && rtc_before.tm_mday >= 1 &&
                               rtc_before.tm_mday <= 31 && rtc_after.tm_mon >= 0 && rtc_after.tm_mon <= 11 &&
                               rtc_after.tm_mday >= 1 && rtc_after.tm_mday <= 31;
    const time_t rtc_before_time = rtc_fields_ok ? mktime(&rtc_before) : static_cast<time_t>(-1);
    const time_t rtc_after_time  = rtc_fields_ok ? mktime(&rtc_after) : static_cast<time_t>(-1);
    const double rtc_delta =
        (rtc_before_time != static_cast<time_t>(-1) && rtc_after_time != static_cast<time_t>(-1))
            ? difftime(rtc_after_time, rtc_before_time)
            : -1;
    const bool rtc_ok = rtc_fields_ok && rtc_delta >= 1 && rtc_delta <= 5;
    if (rtc_ok) {
        ESP_LOGI(TAG, "TAB5X_RTC_TEST_PASS time=%04d-%02d-%02dT%02d:%02d:%02d delta=%.0f",
                 rtc_after.tm_year + 1900, rtc_after.tm_mon + 1, rtc_after.tm_mday, rtc_after.tm_hour,
                 rtc_after.tm_min, rtc_after.tm_sec, rtc_delta);
    } else {
        ESP_LOGE(TAG, "TAB5X_RTC_TEST_FAIL time=%04d-%02d-%02dT%02d:%02d:%02d delta=%.0f",
                 rtc_after.tm_year + 1900, rtc_after.tm_mon + 1, rtc_after.tm_mday, rtc_after.tm_hour,
                 rtc_after.tm_min, rtc_after.tm_sec, rtc_delta);
    }

    ESP_LOGI(TAG, "TAB5X_INTERFACE_STATE usb_c=%d usb_a_hid=%d headphone=%d", hal->usbCDetect(),
             hal->usbADetect(), hal->headPhoneDetect());
    ESP_LOGI(TAG, "TAB5X_SYSTEM_SENSOR_SMOKE_DONE");
}

static void driver_smoke_test_task(void*)
{
    constexpr TickType_t audio_timeout = pdMS_TO_TICKS(20000);
    constexpr TickType_t camera_run    = pdMS_TO_TICKS(5000);

    // Let the startup animation and its audio finish before exercising the shared codec.
    vTaskDelay(pdMS_TO_TICKS(12000));

    while (true) {
        run_system_sensor_smoke();

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
