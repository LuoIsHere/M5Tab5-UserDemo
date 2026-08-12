/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <driver/gpio.h>
#include <memory>
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "hal_rs485"

// RS485
#define TAB5_RS485_BUF_SIZE (127)
// Timeout threshold for UART = number of symbols (~10 tics) with unchanged state on receive pin
#define TAB5_RS485_READ_TOUT        (3)  // 3.5T * 8 = 28 ticks, TOUT=3 -> ~24..33 ticks
#define TAB5_RS485_PACKET_READ_TICS (100 / portTICK_PERIOD_MS)
#define TAB5_RS485_TASK_STACK_SIZE   (4 * 1024)
#define TAB5_RS485_DIAG_INTERVAL_MS  5000
static uart_port_t tab5_rs485_uart_num = UART_NUM_1;

#define TAB5_SYS_RS485_TX_PIN 20
#define TAB5_SYS_RS485_RX_PIN 21
#define TAB5_SYS_RS485_DE_PIN 34

static esp_err_t tab5_rs485_echo_send(const uart_port_t port, const uint8_t* data, size_t length)
{
    const int written = uart_write_bytes(port, data, length);
    if (written < 0 || static_cast<size_t>(written) != length) {
        ESP_LOGE(TAG, "RS485 send failed: requested=%u written=%d",
                 static_cast<unsigned>(length), written);
        return ESP_FAIL;
    }

    const esp_err_t wait_result = uart_wait_tx_done(port, pdMS_TO_TICKS(1000));
    if (wait_result != ESP_OK) {
        ESP_LOGE(TAG, "RS485 TX completion failed: requested=%u error=%s",
                 static_cast<unsigned>(length), esp_err_to_name(wait_result));
        return wait_result;
    }

    return ESP_OK;
}

/*
 * RS485 diagnostic fields:
 * - stack_hwm_bytes: minimum free task stack observed since task creation.
 * - heap_ok: 1 when all heap regions pass the integrity check, otherwise 0.
 * - free_internal_bytes: current total free internal-RAM heap.
 * - free_spiram_bytes: current total free PSRAM heap.
 */
static void _rs485_test_task(void* param)
{
    (void)param;
    uint8_t data[TAB5_RS485_BUF_SIZE];
    std::vector<uint8_t> tx_data;
    TickType_t last_diag_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "RS485 task started: priority=%u core=%d stack_hwm_bytes=%u",
             static_cast<unsigned>(uxTaskPriorityGet(nullptr)), xPortGetCoreID(),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    while (1) {
        // uart_read_bytes() returns the valid byte count, so clearing the whole buffer is unnecessary.
        int len = uart_read_bytes(tab5_rs485_uart_num, data, sizeof(data), TAB5_RS485_PACKET_READ_TICS);
        if (len < 0) {
            ESP_LOGE(TAG, "UART read failed: %d", len);
        } else if (len > 0) {
            std::lock_guard<std::mutex> lock(GetHAL()->uartMonitorData.mutex);
            for (int i = 0; i < len; i++) {
                GetHAL()->uartMonitorData.rxQueue.push(data[i]);
            }
            while (GetHAL()->uartMonitorData.rxQueue.size() > 4096) {
                GetHAL()->uartMonitorData.rxQueue.pop();
            }
        }

        tx_data.clear();
        {
            std::lock_guard<std::mutex> lock(GetHAL()->uartMonitorData.mutex);
            tx_data.reserve(GetHAL()->uartMonitorData.txQueue.size());
            while (!GetHAL()->uartMonitorData.txQueue.empty()) {
                tx_data.push_back(GetHAL()->uartMonitorData.txQueue.front());
                GetHAL()->uartMonitorData.txQueue.pop();
            }
        }
        if (!tx_data.empty()) {
            // Keep the UI queue behavior unchanged; the helper records any physical TX failure.
            (void)tab5_rs485_echo_send(tab5_rs485_uart_num, tx_data.data(), tx_data.size());
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_diag_tick) >= pdMS_TO_TICKS(TAB5_RS485_DIAG_INTERVAL_MS)) {
            const bool heap_ok = heap_caps_check_integrity_all(true);
            ESP_LOGI(TAG, "RS485 diagnostics: stack_hwm_bytes=%u heap_ok=%d free_internal_bytes=%u free_spiram_bytes=%u",
                     static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), heap_ok,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            last_diag_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void HalEsp32::rs485_init()
{
    mclog::tagInfo(TAG, "rs485 init");

    uart_config_t uart_config;
    uart_config.baud_rate           = 115200;
    uart_config.data_bits           = UART_DATA_8_BITS;
    uart_config.parity              = UART_PARITY_DISABLE;
    uart_config.stop_bits           = UART_STOP_BITS_1;
    uart_config.flow_ctrl           = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 122;
    uart_config.source_clk          = UART_SCLK_DEFAULT;

    // Install UART driver (we don't need an event queue here)
    // In this example we don't even use a buffer for sending data.
    ESP_ERROR_CHECK(uart_driver_install(tab5_rs485_uart_num, TAB5_RS485_BUF_SIZE * 2, 0, 0, NULL, 0));

    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(tab5_rs485_uart_num, &uart_config));

    ESP_LOGI(TAG, "UART set pins, mode and install driver.");

    // ESP32-P4 RS485 half-duplex direction control uses the UART DTR signal.
    ESP_ERROR_CHECK(uart_set_pin(tab5_rs485_uart_num, TAB5_SYS_RS485_TX_PIN, TAB5_SYS_RS485_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, TAB5_SYS_RS485_DE_PIN,
                                 UART_PIN_NO_CHANGE));

    // Set RS485 half duplex mode
    ESP_ERROR_CHECK(uart_set_mode(tab5_rs485_uart_num, UART_MODE_RS485_HALF_DUPLEX));

    // Set read timeout of UART TOUT feature
    ESP_ERROR_CHECK(uart_set_rx_timeout(tab5_rs485_uart_num, TAB5_RS485_READ_TOUT));

    BaseType_t task_result =
        xTaskCreate(_rs485_test_task, "rs485", TAB5_RS485_TASK_STACK_SIZE, nullptr, 5, nullptr);
    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RS485 task");
    }
}
