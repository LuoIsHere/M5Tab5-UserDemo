/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <memory>
#include <string.h>
#include <inttypes.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <thread>
#include <mutex>
#include <audio_player.h>
#include <esp_err.h>
#include <esp_log.h>

static const char* TAG = "audio";

static uint8_t _current_speaker_volume = 60;
static std::mutex _audio_tx_mutex;

void HalEsp32::setSpeakerVolume(uint8_t volume)
{
    _current_speaker_volume = std::clamp((int)volume, 0, 100);
    mclog::tagInfo(TAG, "set speaker volume: {}%", _current_speaker_volume);
}

uint8_t HalEsp32::getSpeakerVolume()
{
    return _current_speaker_volume;
}

void HalEsp32::audioRecord(std::vector<int16_t>& data, uint16_t durationMs, float gain)
{
    data.resize(48000 * 4 * durationMs / 1000);

    // ESP_LOGI(TAG, "start record");
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    codec_handle->set_in_gain(gain);
    size_t bytes_read = 0;
    codec_handle->i2s_read((char*)data.data(), (48000 * 4 * durationMs / 1000) * sizeof(uint16_t), &bytes_read,
                           portMAX_DELAY);
    // ESP_LOGI(TAG, "record done, %d bytes", bytes_read);
}

struct AudioTaskData_t {
    std::mutex mutex;
    bool is_task_running  = false;
    bool is_audio_ready   = false;
    bool is_audio_playing = false;
    std::vector<int16_t> audio_data;
};
static AudioTaskData_t _audio_task_data;

void _audio_play_task(void* param)
{
    while (true) {
        _audio_task_data.mutex.lock();

        if (_audio_task_data.is_audio_ready) {
            _audio_task_data.is_audio_playing = true;
            _audio_task_data.mutex.unlock();

            // UI tones are best-effort. Never let them reconfigure/write TX while MP3 owns it.
            std::unique_lock<std::mutex> tx_lock(_audio_tx_mutex, std::try_to_lock);
            if (tx_lock.owns_lock()) {
                bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
                size_t bytes_written = 0;
                esp_err_t ret = codec_handle->set_volume(_current_speaker_volume);
                if (ret == ESP_OK) {
                    ret = codec_handle->i2s_reconfig_clk_fn(48000, 16, I2S_SLOT_MODE_STEREO);
                }
                if (ret == ESP_OK) {
                    const size_t bytes_to_write = _audio_task_data.audio_data.size() * sizeof(uint16_t);
                    ret = codec_handle->i2s_write(_audio_task_data.audio_data.data(), bytes_to_write,
                                                  &bytes_written, portMAX_DELAY);
                    if (ret == ESP_OK && bytes_written != bytes_to_write) {
                        ret = ESP_FAIL;
                    }
                }
                if (ret != ESP_OK) {
                    mclog::tagError(TAG, "async audio output failed: {}", esp_err_to_name(ret));
                }
            } else {
                mclog::tagWarn(TAG, "drop async audio while music owns TX");
            }

            _audio_task_data.mutex.lock();
            _audio_task_data.is_audio_playing = false;
            _audio_task_data.is_audio_ready   = false;
            _audio_task_data.mutex.unlock();

            continue;
        }

        _audio_task_data.mutex.unlock();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void HalEsp32::audioPlay(std::vector<int16_t>& data, bool async)
{
    if (async) {
        std::lock_guard<std::mutex> lock(_audio_task_data.mutex);

        if (_audio_task_data.is_audio_playing) {
            mclog::tagWarn(TAG, "audio is playing");
            return;
        }

        if (!_audio_task_data.is_task_running) {
            _audio_task_data.is_task_running = true;
            BaseType_t task_ret = xTaskCreate(_audio_play_task, "audio", 4096, nullptr, 5, nullptr);
            if (task_ret != pdPASS) {
                _audio_task_data.is_task_running = false;
                mclog::tagError(TAG, "failed to create async audio task");
                return;
            }
        }

        _audio_task_data.audio_data     = data;
        _audio_task_data.is_audio_ready = true;
    } else {
        std::lock_guard<std::mutex> tx_lock(_audio_tx_mutex);
        bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
        size_t bytes_written = 0;
        esp_err_t ret = codec_handle->set_volume(_current_speaker_volume);
        if (ret == ESP_OK) {
            ret = codec_handle->i2s_reconfig_clk_fn(48000, 16, I2S_SLOT_MODE_STEREO);
        }
        if (ret == ESP_OK) {
            const size_t bytes_to_write = data.size() * sizeof(uint16_t);
            ret = codec_handle->i2s_write(data.data(), bytes_to_write, &bytes_written, portMAX_DELAY);
            if (ret == ESP_OK && bytes_written != bytes_to_write) {
                ret = ESP_FAIL;
            }
        }
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "audio output failed: {}", esp_err_to_name(ret));
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                            Record and play test                            */
/* -------------------------------------------------------------------------- */
struct RecTestData_t {
    std::mutex mutex;
    bool isDualMic                     = true;
    hal::HalBase::MicTestState_t state = hal::HalBase::MIC_TEST_IDLE;
    int16_t* audio_buffer              = nullptr;
    int16_t* read_buffer               = nullptr;
};
static RecTestData_t _rec_test_data;

static void _rec_test_task(void* param)
{
    mclog::tagInfo(TAG, "start record test");

    const size_t read_buffer_size  = 48000 * 4 * 3;
    const size_t audio_buffer_size = 48000 * 2 * 3;

    // Create buffers
    if (_rec_test_data.audio_buffer == nullptr) {
        _rec_test_data.audio_buffer = new int16_t[audio_buffer_size](0);
    }
    if (_rec_test_data.read_buffer == nullptr) {
        _rec_test_data.read_buffer = new int16_t[read_buffer_size](0);
    }

    const size_t sample_rate   = 48000;
    const size_t total_samples = sample_rate * 4 * 3;  // 4通道，3秒
    const size_t chunk_samples = 4096 * 4;             // 4通道一帧
    const size_t chunk_bytes   = chunk_samples * sizeof(int16_t);

    size_t total_read_samples = 0;
    size_t total_read_bytes   = 0;
    esp_err_t read_ret        = ESP_OK;

    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    codec_handle->set_in_gain(240);

    int16_t* read_buf = _rec_test_data.read_buffer;
    memset(read_buf, 0, total_samples * sizeof(int16_t));  // 清零

    mclog::tagInfo(TAG, "start record");

    while (total_read_samples < total_samples) {
        size_t bytes_to_read = chunk_bytes;
        if (total_read_samples + chunk_samples > total_samples) {
            bytes_to_read = (total_samples - total_read_samples) * sizeof(int16_t);
        }

        size_t bytes_read = 0;
        read_ret = codec_handle->i2s_read((char*)(read_buf + total_read_samples), bytes_to_read, &bytes_read,
                                          portMAX_DELAY);
        if (read_ret != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "record read failed: ret=%s, bytes=%u", esp_err_to_name(read_ret),
                     static_cast<unsigned>(bytes_read));
            break;
        }

        total_read_samples += bytes_read / sizeof(int16_t);
        total_read_bytes += bytes_read;

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    const size_t expected_read_bytes = total_samples * sizeof(int16_t);
    mclog::tagInfo(TAG, "record done: {}/{} bytes", total_read_bytes, expected_read_bytes);

    if (read_ret != ESP_OK || total_read_bytes != expected_read_bytes) {
        ESP_LOGE(TAG, "TAB5X_AUDIO_DRIVER_TEST_FAIL stage=record ret=%s bytes=%u/%u", esp_err_to_name(read_ret),
                 static_cast<unsigned>(total_read_bytes), static_cast<unsigned>(expected_read_bytes));
        _rec_test_data.mutex.lock();
        _rec_test_data.state = hal::HalBase::MIC_TEST_IDLE;
        _rec_test_data.mutex.unlock();
        vTaskDelete(NULL);
        return;
    }

    // Create audio data  [MIC-L, AEC, MIC-R, MIC-HP]
    int num_frames = audio_buffer_size / 2;  // 每帧一组 stereo 输出
    for (int i = 0; i < num_frames; ++i) {
        int16_t* in  = _rec_test_data.read_buffer;
        int16_t* out = _rec_test_data.audio_buffer;

        if (_rec_test_data.isDualMic) {
            out[i * 2 + 0] = in[i * 4 + 0];  // MIC-L
            out[i * 2 + 1] = in[i * 4 + 2];  // MIC-R
        } else {
            out[i * 2 + 0] = in[i * 4 + 3];  // MIC-HP
            out[i * 2 + 1] = in[i * 4 + 3];  // MIC-HP (duplicate for stereo)
        }
    }

    _rec_test_data.mutex.lock();
    _rec_test_data.state = hal::HalBase::MIC_TEST_PLAYING;
    _rec_test_data.mutex.unlock();

    size_t bytes_written = 0;
    codec_handle->set_volume(_current_speaker_volume);
    esp_err_t clk_ret = codec_handle->i2s_reconfig_clk_fn(48000, 16, I2S_SLOT_MODE_STEREO);

    mclog::tagInfo(TAG, "start playback");
    const size_t expected_write_bytes = audio_buffer_size * sizeof(uint16_t);
    esp_err_t write_ret = codec_handle->i2s_write(_rec_test_data.audio_buffer, expected_write_bytes, &bytes_written,
                                                   portMAX_DELAY);
    mclog::tagInfo(TAG, "playback done: {}/{} bytes", bytes_written, expected_write_bytes);

    if (clk_ret == ESP_OK && write_ret == ESP_OK && bytes_written == expected_write_bytes) {
        ESP_LOGI(TAG, "TAB5X_AUDIO_DRIVER_TEST_PASS read=%u write=%u", static_cast<unsigned>(total_read_bytes),
                 static_cast<unsigned>(bytes_written));
    } else {
        ESP_LOGE(TAG, "TAB5X_AUDIO_DRIVER_TEST_FAIL stage=playback clk=%s write=%s bytes=%u/%u",
                 esp_err_to_name(clk_ret), esp_err_to_name(write_ret), static_cast<unsigned>(bytes_written),
                 static_cast<unsigned>(expected_write_bytes));
    }

    _rec_test_data.mutex.lock();
    _rec_test_data.state = hal::HalBase::MIC_TEST_IDLE;
    _rec_test_data.mutex.unlock();

    vTaskDelete(NULL);
}

static void try_create_rec_test_task(bool isDualMic)
{
    _rec_test_data.mutex.lock();

    if (_rec_test_data.state == hal::HalBase::MIC_TEST_IDLE) {
        _rec_test_data.mutex.unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
        _rec_test_data.mutex.lock();
        if (_rec_test_data.state == hal::HalBase::MIC_TEST_IDLE) {
            _rec_test_data.isDualMic = isDualMic;
            _rec_test_data.state     = hal::HalBase::MIC_TEST_RECORDING;
            xTaskCreate(_rec_test_task, "rec", 4096, nullptr, 5, nullptr);
            _rec_test_data.mutex.unlock();
            return;
        }
    }

    _rec_test_data.mutex.unlock();
    mclog::tagWarn(TAG, "rec test is running");
}

void HalEsp32::startDualMicRecordTest()
{
    try_create_rec_test_task(true);
}

hal::HalBase::MicTestState_t HalEsp32::getDualMicRecordTestState()
{
    std::lock_guard<std::mutex> lock(_rec_test_data.mutex);
    return _rec_test_data.state;
}

void HalEsp32::startHeadphoneMicRecordTest()
{
    try_create_rec_test_task(false);
}

hal::HalBase::MicTestState_t HalEsp32::getHeadphoneMicRecordTestState()
{
    std::lock_guard<std::mutex> lock(_rec_test_data.mutex);
    return _rec_test_data.state;
}

/* -------------------------------------------------------------------------- */
/*                               Music play test                              */
/* -------------------------------------------------------------------------- */
extern const uint8_t canon_in_d_mp3_start[] asm("_binary_canon_in_d_mp3_start");
extern const uint8_t canon_in_d_mp3_end[] asm("_binary_canon_in_d_mp3_end");
extern const uint8_t startup_sfx_mp3_start[] asm("_binary_startup_sfx_mp3_start");
extern const uint8_t startup_sfx_mp3_end[] asm("_binary_startup_sfx_mp3_end");
extern const uint8_t shutdown_sfx_mp3_start[] asm("_binary_shutdown_sfx_mp3_start");
extern const uint8_t shutdown_sfx_mp3_end[] asm("_binary_shutdown_sfx_mp3_end");

enum Mp3PlayTarget_t {
    MP3_PLAY_TARGET_CANON_IN_D,
    MP3_PLAY_TARGET_STARTUP_SFX,
    MP3_PLAY_TARGET_SHUTDOWN_SFX,
};

struct MusicTestData_t {
    std::mutex mutex;
    hal::HalBase::MusicPlayState_t state = hal::HalBase::MUSIC_PLAY_IDLE;
    Mp3PlayTarget_t target               = MP3_PLAY_TARGET_CANON_IN_D;
    TaskHandle_t taskHandle              = nullptr;
    esp_err_t lastError                  = ESP_OK;
};
static MusicTestData_t _music_test_data;

static bool music_stop_requested()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    return _music_test_data.state == hal::HalBase::MUSIC_PLAY_STOPPING;
}

static bool music_playback_was_started()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    return _music_test_data.state == hal::HalBase::MUSIC_PLAY_PLAYING ||
           _music_test_data.state == hal::HalBase::MUSIC_PLAY_STOPPING;
}

static void music_mark_playing()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    if (_music_test_data.state == hal::HalBase::MUSIC_PLAY_STARTING) {
        _music_test_data.state = hal::HalBase::MUSIC_PLAY_PLAYING;
    }
}

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    if (codec_handle == nullptr || codec_handle->set_mute == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_handle->set_mute(setting == AUDIO_PLAYER_MUTE);
}

static esp_err_t audio_clock_function(uint32_t rate, uint32_t bits_per_sample, i2s_slot_mode_t channel)
{
    // RX/AEC remains at 48 kHz. Reconfiguring the shared I2S peripheral to 44.1 kHz
    // breaks record/playback and previously drove Audio Task into a retry/busy loop.
    if (rate != 48000) {
        ESP_LOGE(TAG, "reject non-48kHz audio stream: %" PRIu32 " Hz", rate);
        return ESP_ERR_NOT_SUPPORTED;
    }
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    if (codec_handle == nullptr || codec_handle->i2s_reconfig_clk_fn == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec_handle->i2s_reconfig_clk_fn(rate, bits_per_sample, channel);
}

static void audio_player_callback(audio_player_cb_ctx_t* ctx)
{
    mclog::tagInfo(TAG, "audio event: {}", (int)ctx->audio_event);
    audio_player_state_t state = audio_player_get_state();
    mclog::tagInfo(TAG, "audio state: {}", (int)state);
    if (state == AUDIO_PLAYER_STATE_PLAYING) {
        music_mark_playing();
    }
}

static FILE* open_embedded_mp3(Mp3PlayTarget_t target)
{
    const uint8_t* start = nullptr;
    const uint8_t* end   = nullptr;
    switch (target) {
        case MP3_PLAY_TARGET_CANON_IN_D:
            start = canon_in_d_mp3_start;
            end   = canon_in_d_mp3_end;
            break;
        case MP3_PLAY_TARGET_STARTUP_SFX:
            start = startup_sfx_mp3_start;
            end   = startup_sfx_mp3_end;
            break;
        case MP3_PLAY_TARGET_SHUTDOWN_SFX:
            start = shutdown_sfx_mp3_start;
            end   = shutdown_sfx_mp3_end;
            break;
        default:
            return nullptr;
    }
    const size_t mp3_size = (end - start) - 1;
    return fmemopen((void*)start, mp3_size, "rb");
}

static esp_err_t run_music_session(Mp3PlayTarget_t target)
{
    // One owner for codec reconfiguration and all TX writes for the whole MP3 session.
    std::unique_lock<std::mutex> tx_lock(_audio_tx_mutex);
    bsp_codec_config_t* codec_handle = bsp_get_codec_handle();
    if (codec_handle == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    bool player_created = false;
    FILE* fp = nullptr;

    do {
        if (music_stop_requested()) {
            break;
        }

        ret = codec_handle->set_volume(_current_speaker_volume);
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "set music volume failed: {}", esp_err_to_name(ret));
            break;
        }
        ret = audio_clock_function(48000, 16, I2S_SLOT_MODE_STEREO);
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "configure 48kHz output failed: {}", esp_err_to_name(ret));
            break;
        }

        audio_player_config_t config = {
            .mute_fn      = audio_mute_function,
            .clk_set_fn   = audio_clock_function,
            .write_fn     = codec_handle->i2s_write,
            .priority     = 7,
            .coreID       = 1,
            .force_stereo = false,
            .write_fn2    = nullptr,
            .write_ctx    = nullptr,
        };
        ret = audio_player_new(config);
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "audio player create failed: {}", esp_err_to_name(ret));
            break;
        }
        player_created = true;

        ret = audio_player_callback_register(audio_player_callback, nullptr);
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "audio callback register failed: {}", esp_err_to_name(ret));
            break;
        }

        fp = open_embedded_mp3(target);
        if (fp == nullptr) {
            ret = ESP_FAIL;
            mclog::tagError(TAG, "open embedded MP3 failed");
            break;
        }
        if (music_stop_requested()) {
            break;
        }

        ret = audio_player_play(fp);
        if (ret != ESP_OK) {
            mclog::tagError(TAG, "audio play request failed: {}", esp_err_to_name(ret));
            break;
        }
        // Ownership transfers to audio_player only after the play event is queued successfully.
        fp = nullptr;

        bool stop_sent = false;
        const TickType_t start_tick = xTaskGetTickCount();
        while (true) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            const bool stopping = music_stop_requested();
            if (stopping && !stop_sent) {
                esp_err_t stop_ret = audio_player_stop();
                if (stop_ret == ESP_OK) {
                    stop_sent = true;
                } else {
                    mclog::tagWarn(TAG, "queue music stop failed, retrying: {}", esp_err_to_name(stop_ret));
                }
            }

            const audio_player_state_t player_state = audio_player_get_state();
            if (player_state == AUDIO_PLAYER_STATE_PLAYING) {
                music_mark_playing();
            } else if (player_state == AUDIO_PLAYER_STATE_IDLE) {
                if (music_playback_was_started() || stopping) {
                    ret = audio_player_get_last_error();
                    if (ret != ESP_OK) {
                        mclog::tagError(TAG, "audio playback failed: {}", esp_err_to_name(ret));
                    }
                    break;
                }
                if ((xTaskGetTickCount() - start_tick) > pdMS_TO_TICKS(2000)) {
                    ret = ESP_ERR_TIMEOUT;
                    mclog::tagError(TAG, "audio player did not start within timeout");
                    break;
                }
            } else if (player_state == AUDIO_PLAYER_STATE_SHUTDOWN) {
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
        }
    } while (false);

    if (fp != nullptr) {
        fclose(fp);
    }
    if (player_created) {
        esp_err_t delete_ret = audio_player_delete();
        if (delete_ret != ESP_OK) {
            mclog::tagError(TAG, "audio player delete failed: {}", esp_err_to_name(delete_ret));
            ret = delete_ret;
        }
    }
    return ret;
}

static void _music_play_task(void* param)
{
    Mp3PlayTarget_t target;
    {
        std::lock_guard<std::mutex> lock(_music_test_data.mutex);
        target = _music_test_data.target;
    }

    esp_err_t ret = run_music_session(target);

    {
        std::lock_guard<std::mutex> lock(_music_test_data.mutex);
        _music_test_data.lastError = ret;
        _music_test_data.taskHandle = nullptr;
        _music_test_data.state = (ret == ESP_OK) ? hal::HalBase::MUSIC_PLAY_IDLE
                                                  : hal::HalBase::MUSIC_PLAY_ERROR;
    }
    vTaskDelete(nullptr);
}

static void try_create_music_play_task(Mp3PlayTarget_t target)
{
    if (_music_test_data.state != hal::HalBase::MUSIC_PLAY_IDLE &&
        _music_test_data.state != hal::HalBase::MUSIC_PLAY_ERROR) {
        mclog::tagWarn(TAG, "music start ignored in state {}", (int)_music_test_data.state);
        return;
    }

    _music_test_data.state = hal::HalBase::MUSIC_PLAY_STARTING;
    _music_test_data.target = target;
    _music_test_data.lastError = ESP_OK;
    BaseType_t task_ret = xTaskCreate(_music_play_task, "music", 4096, nullptr, 5,
                                      &_music_test_data.taskHandle);
    if (task_ret != pdPASS) {
        _music_test_data.taskHandle = nullptr;
        _music_test_data.lastError = ESP_ERR_NO_MEM;
        _music_test_data.state = hal::HalBase::MUSIC_PLAY_ERROR;
        mclog::tagError(TAG, "failed to create music task");
    }
}

void HalEsp32::startPlayMusicTest()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    try_create_music_play_task(MP3_PLAY_TARGET_CANON_IN_D);
}

hal::HalBase::MusicPlayState_t HalEsp32::getMusicPlayTestState()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    return _music_test_data.state;
}

void HalEsp32::stopPlayMusicTest()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    if (_music_test_data.state == hal::HalBase::MUSIC_PLAY_STARTING ||
        _music_test_data.state == hal::HalBase::MUSIC_PLAY_PLAYING) {
        _music_test_data.state = hal::HalBase::MUSIC_PLAY_STOPPING;
        if (_music_test_data.taskHandle != nullptr) {
            xTaskNotifyGive(_music_test_data.taskHandle);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                                     SFX                                    */
/* -------------------------------------------------------------------------- */
void HalEsp32::playStartupSfx()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    try_create_music_play_task(MP3_PLAY_TARGET_STARTUP_SFX);
}

void HalEsp32::playShutdownSfx()
{
    std::lock_guard<std::mutex> lock(_music_test_data.mutex);
    try_create_music_play_task(MP3_PLAY_TARGET_SHUTDOWN_SFX);
}
