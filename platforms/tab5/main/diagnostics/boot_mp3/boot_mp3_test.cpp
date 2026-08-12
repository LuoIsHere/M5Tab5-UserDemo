/*
 * Isolated boot-time MP3 playback diagnostic for Tab5X.
 *
 * Keep this file independent from the normal application and HAL layers. The
 * isolation makes it possible to compare MP3 decoding/I2S behavior with and
 * without UI, recording, networking, camera, USB, and RS485 workloads.
 */
#include "diagnostics/boot_mp3/boot_mp3_test.h"

#include "sdkconfig.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "audio_player.h"
#include "bsp/m5stack_tab5.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "boot_mp3";
constexpr uint32_t kRequiredSampleRateHz = 48000;
constexpr TickType_t kPlayerStartTimeout = pdMS_TO_TICKS(2000);
constexpr TickType_t kStatePollInterval = pdMS_TO_TICKS(20);
constexpr TickType_t kRepeatDelay = pdMS_TO_TICKS(1000);
constexpr TickType_t kIdleDelay = pdMS_TO_TICKS(10000);
#if CONFIG_TAB5X_BOOT_MP3_REPEAT
constexpr bool kRepeatPlayback = true;
#else
constexpr bool kRepeatPlayback = false;
#endif

extern const uint8_t canon_in_d_mp3_start[] asm("_binary_canon_in_d_mp3_start");
extern const uint8_t canon_in_d_mp3_end[] asm("_binary_canon_in_d_mp3_end");

std::atomic_bool s_playback_started{false};

void log_heap_snapshot(const char *stage)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    // free: current available bytes; largest: maximum contiguous allocation;
    // low: historical minimum free bytes since boot. The largest internal block
    // is especially important because the Audio Task requests one 8192-byte stack.
    ESP_LOGI(TAG,
             "TAB5X_BOOT_MP3_HEAP stage=%s internal_free/largest/low=%u/%u/%u "
             "dma_free/largest/low=%u/%u/%u psram_free/largest/low=%u/%u/%u",
             stage,
             static_cast<unsigned>(heap_caps_get_free_size(internal_caps)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(internal_caps)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(internal_caps)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_DMA)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
}

esp_err_t boot_audio_mute(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bsp_codec_config_t *codec = bsp_get_codec_handle();
    if (codec == nullptr || codec->set_mute == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec->set_mute(setting == AUDIO_PLAYER_MUTE);
}

esp_err_t boot_audio_clock(uint32_t rate, uint32_t bits_per_sample, i2s_slot_mode_t channel)
{
    // Match the normal Music Test path: RX/AEC and TX share the 48 kHz codec/I2S
    // setup, so accepting a 44.1 kHz reconfiguration would add another variable.
    if (rate != kRequiredSampleRateHz) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_UNSUPPORTED_RATE rate=%" PRIu32, rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bsp_codec_config_t *codec = bsp_get_codec_handle();
    if (codec == nullptr || codec->i2s_reconfig_clk_fn == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec->i2s_reconfig_clk_fn(rate, bits_per_sample, channel);
}

void boot_audio_event(audio_player_cb_ctx_t *ctx)
{
    if (ctx == nullptr) {
        return;
    }
    if (ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_PLAYING ||
        ctx->audio_event == AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT) {
        s_playback_started.store(true, std::memory_order_release);
    }
    ESP_LOGI(TAG, "TAB5X_BOOT_MP3_EVENT event=%d state=%d",
             static_cast<int>(ctx->audio_event), static_cast<int>(audio_player_get_state()));
}

FILE *open_embedded_mp3(size_t *file_size)
{
    const ptrdiff_t embedded_size = canon_in_d_mp3_end - canon_in_d_mp3_start;
    if (embedded_size <= 1) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_INVALID_EMBEDDED_FILE size=%td", embedded_size);
        return nullptr;
    }

    // EMBED_TXTFILES appends a trailing NUL byte which is not part of the MP3.
    const size_t size = static_cast<size_t>(embedded_size) - 1U;
    FILE *fp = fmemopen(const_cast<uint8_t *>(canon_in_d_mp3_start), size, "rb");
    if (fp != nullptr && file_size != nullptr) {
        *file_size = size;
    }
    return fp;
}

esp_err_t initialize_audio_hardware()
{
    ESP_LOGI(TAG, "TAB5X_BOOT_MP3_CODEC_INIT_BEGIN");

    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_I2C_INIT_FAILED error=%s", esp_err_to_name(ret));
        return ret;
    }

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_I2C_HANDLE_MISSING");
        return ESP_ERR_INVALID_STATE;
    }

    // The board IO expander drives SPK_EN. Codec initialization alone is not
    // sufficient to produce physical speaker output.
    bsp_io_expander_pi4ioe_init(i2c_bus);
    vTaskDelay(pdMS_TO_TICKS(200));
    bsp_codec_init();

    bsp_codec_config_t *codec = bsp_get_codec_handle();
    if (codec == nullptr || codec->set_volume == nullptr || codec->i2s_write == nullptr ||
        codec->set_mute == nullptr || codec->i2s_reconfig_clk_fn == nullptr) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_CODEC_HANDLE_INCOMPLETE");
        return ESP_ERR_INVALID_STATE;
    }

    ret = static_cast<esp_err_t>(codec->set_volume(CONFIG_TAB5X_BOOT_MP3_VOLUME));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_VOLUME_FAILED volume=%d error=%s",
                 CONFIG_TAB5X_BOOT_MP3_VOLUME, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TAB5X_BOOT_MP3_CODEC_INIT_DONE volume=%d", CONFIG_TAB5X_BOOT_MP3_VOLUME);
    return ESP_OK;
}

esp_err_t run_playback_session()
{
    bsp_codec_config_t *codec = bsp_get_codec_handle();
    if (codec == nullptr || codec->i2s_write == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    bool player_created = false;
    FILE *fp = nullptr;
    size_t mp3_size = 0;

    log_heap_snapshot("before-player-new");
    audio_player_config_t config = {
        .mute_fn = boot_audio_mute,
        .clk_set_fn = boot_audio_clock,
        .write_fn = codec->i2s_write,
        .priority = 7,
        .coreID = 1,
        .force_stereo = false,
        .write_fn2 = nullptr,
        .write_ctx = nullptr,
    };

    result = audio_player_new(config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_PLAYER_CREATE_FAILED error=%s", esp_err_to_name(result));
        return result;
    }
    player_created = true;
    log_heap_snapshot("after-player-new");

    do {
        result = audio_player_callback_register(boot_audio_event, nullptr);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_CALLBACK_FAILED error=%s", esp_err_to_name(result));
            break;
        }

        fp = open_embedded_mp3(&mp3_size);
        if (fp == nullptr) {
            result = ESP_FAIL;
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_OPEN_FAILED");
            break;
        }

        s_playback_started.store(false, std::memory_order_release);
        const int64_t start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "TAB5X_BOOT_MP3_PLAY_BEGIN bytes=%u", static_cast<unsigned>(mp3_size));

        result = audio_player_play(fp);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_PLAY_REQUEST_FAILED error=%s", esp_err_to_name(result));
            break;
        }
        // audio_player owns and closes the FILE only after play() queues it.
        fp = nullptr;

        const TickType_t wait_start = xTaskGetTickCount();
        while (true) {
            const audio_player_state_t state = audio_player_get_state();
            if (state == AUDIO_PLAYER_STATE_PLAYING) {
                s_playback_started.store(true, std::memory_order_release);
            } else if (state == AUDIO_PLAYER_STATE_IDLE) {
                if (s_playback_started.load(std::memory_order_acquire)) {
                    result = audio_player_get_last_error();
                    break;
                }
                if ((xTaskGetTickCount() - wait_start) > kPlayerStartTimeout) {
                    result = ESP_ERR_TIMEOUT;
                    ESP_LOGE(TAG, "TAB5X_BOOT_MP3_START_TIMEOUT");
                    break;
                }
            } else if (state == AUDIO_PLAYER_STATE_SHUTDOWN) {
                result = ESP_ERR_INVALID_STATE;
                ESP_LOGE(TAG, "TAB5X_BOOT_MP3_UNEXPECTED_SHUTDOWN");
                break;
            }
            vTaskDelay(kStatePollInterval);
        }

        const uint64_t duration_ms = static_cast<uint64_t>((esp_timer_get_time() - start_us) / 1000);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "TAB5X_BOOT_MP3_PLAY_END result=%s duration_ms=%" PRIu64,
                     esp_err_to_name(result), duration_ms);
        } else {
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_PLAY_END result=%s duration_ms=%" PRIu64,
                     esp_err_to_name(result), duration_ms);
        }
        log_heap_snapshot("session-end");
    } while (false);

    if (fp != nullptr) {
        fclose(fp);
    }
    if (player_created) {
        const esp_err_t delete_result = audio_player_delete();
        if (delete_result != ESP_OK) {
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_PLAYER_DELETE_FAILED error=%s", esp_err_to_name(delete_result));
            if (result == ESP_OK) {
                result = delete_result;
            }
        }
    }
    log_heap_snapshot("after-player-delete");
    return result;
}

}  // namespace

void tab5_boot_mp3_test_run()
{
    ESP_LOGW(TAG, "TAB5X_BOOT_MP3_ISOLATION_BEGIN normal_application=disabled repeat=%d",
             static_cast<int>(kRepeatPlayback));
    log_heap_snapshot("entry");

    const esp_err_t init_result = initialize_audio_hardware();
    if (init_result != ESP_OK) {
        ESP_LOGE(TAG, "TAB5X_BOOT_MP3_ISOLATION_INIT_FAILED error=%s", esp_err_to_name(init_result));
        while (true) {
            vTaskDelay(kIdleDelay);
        }
    }
    log_heap_snapshot("after-codec-init");

    do {
        const esp_err_t play_result = run_playback_session();
        if (play_result == ESP_OK) {
            ESP_LOGI(TAG, "TAB5X_BOOT_MP3_ISOLATION_DONE result=%s", esp_err_to_name(play_result));
        } else {
            ESP_LOGE(TAG, "TAB5X_BOOT_MP3_ISOLATION_DONE result=%s", esp_err_to_name(play_result));
        }

#if CONFIG_TAB5X_BOOT_MP3_REPEAT
        ESP_LOGI(TAG, "TAB5X_BOOT_MP3_REPEAT_WAIT delay_ms=1000");
        vTaskDelay(kRepeatDelay);
#endif
    } while (kRepeatPlayback);

    // app_main intentionally remains alive without starting the normal app.
    while (true) {
        vTaskDelay(kIdleDelay);
    }
}
