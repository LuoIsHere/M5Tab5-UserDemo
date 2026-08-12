#include <string.h>
#include "audio_log.h"
#include "audio_mp3.h"

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
#include <inttypes.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#endif

static const char *TAG = "mp3";

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
#define MP3_DIAG_REPORT_INTERVAL_US (5LL * 1000 * 1000)
#define MP3_DIAG_LATE_CALL_GAP_US   (30LL * 1000)

static uint64_t mp3_diag_average(uint64_t total, uint64_t count)
{
    return count == 0 ? 0 : total / count;
}

static const char *mp3_diag_memory_region(const void *ptr)
{
    if (ptr == NULL) {
        return "null";
    }
    if (esp_ptr_external_ram(ptr)) {
        return "PSRAM";
    }
    if (esp_ptr_internal(ptr)) {
        return "internal";
    }
    return "other";
}

static void mp3_decode_diag_log_heap(const char *phase)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    const uint32_t spiram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    /*
     * Log field meanings:
     *   free    = bytes currently available for allocation.
     *   largest = largest contiguous allocation currently possible.
     *   low     = lowest free-memory watermark since boot, not only this MP3 session.
     * DMA is a subset of internal memory and is shown separately because I2S depends on it.
     */
    ESP_LOGI(TAG,
             "MP3 decode heap (%s): internal free/largest/low=%u/%u/%u, "
             "DMA free/largest/low=%u/%u/%u, PSRAM free/largest/low=%u/%u/%u",
             phase,
             (unsigned)heap_caps_get_free_size(internal_caps),
             (unsigned)heap_caps_get_largest_free_block(internal_caps),
             (unsigned)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned)heap_caps_get_free_size(dma_caps),
             (unsigned)heap_caps_get_largest_free_block(dma_caps),
             (unsigned)heap_caps_get_minimum_free_size(dma_caps),
             (unsigned)heap_caps_get_free_size(spiram_caps),
             (unsigned)heap_caps_get_largest_free_block(spiram_caps),
             (unsigned)heap_caps_get_minimum_free_size(spiram_caps));
}

static void mp3_decode_diag_report(mp3_instance *pInstance, const char *phase, int64_t now_us)
{
    mp3_decode_diag_t *diag = &pInstance->diag;

    /*
     * Counter meanings:
     *   calls/frames/pcm = decode_mp3 calls, successfully decoded MP3 frames, and produced PCM bytes.
     *   no_data = calls asking the player to retry without PCM output.
     *   refill/refill_bytes = input-buffer fread operations and bytes read.
     *   sync_miss = calls where no MP3 sync word was found; underflow = Helix requested more main data.
     *   decode_err = other Helix decode errors; late = decoder call-start gaps >=30 ms.
     */
    ESP_LOGI(TAG,
             "MP3 decode (%s): calls=%" PRIu64 " frames=%" PRIu64 " pcm=%" PRIu64
             " no_data=%" PRIu64 " refill=%" PRIu64 " refill_bytes=%" PRIu64
             " sync_miss=%" PRIu64 " underflow=%" PRIu64 " decode_err=%" PRIu64
             " late=%" PRIu64,
             phase, diag->calls, diag->decoded_frames, diag->pcm_bytes,
             diag->no_data_calls, diag->refill_calls, diag->refill_bytes,
             diag->sync_misses, diag->underflows, diag->decode_errors,
             diag->late_call_gaps);

    /*
     * Timing values are cumulative average/maximum microseconds:
     *   gap    = time between starts of adjacent decode_mp3 calls; large values indicate task starvation or I2S wait.
     *   move   = compacting unread compressed data with memmove before refill.
     *   read   = fread time while refilling the compressed input buffer.
     *   sync   = MP3FindSyncWord time.
     *   decode = Helix MP3Decode time only.
     *   total  = complete decode_mp3 call, excluding this diagnostic report itself.
     */
    ESP_LOGI(TAG,
             "MP3 decode timing (%s) avg/max us: gap=%" PRIu64 "/%" PRId64
             " move=%" PRIu64 "/%" PRId64 " read=%" PRIu64 "/%" PRId64
             " sync=%" PRIu64 "/%" PRId64 " decode=%" PRIu64 "/%" PRId64
             " total=%" PRIu64 "/%" PRId64,
             phase,
             mp3_diag_average(diag->call_gap_total_us, diag->call_gap_count), diag->max_call_gap_us,
             mp3_diag_average(diag->move_total_us, diag->refill_calls), diag->max_move_us,
             mp3_diag_average(diag->read_total_us, diag->refill_calls), diag->max_read_us,
             mp3_diag_average(diag->sync_total_us, diag->sync_calls), diag->max_sync_us,
             mp3_diag_average(diag->decode_total_us, diag->decode_calls), diag->max_decode_us,
             mp3_diag_average(diag->call_total_us, diag->calls), diag->max_call_us);

    mp3_decode_diag_log_heap(phase);
    diag->last_report_us = now_us;
}

void mp3_decode_diag_session_start(HMP3Decoder mp3_decoder, const decode_data *pData, mp3_instance *pInstance)
{
    const int64_t now_us = esp_timer_get_time();
    memset(&pInstance->diag, 0, sizeof(pInstance->diag));
    pInstance->diag.last_report_us = now_us;
    pInstance->diag.first_frame_report_pending = true;

    /*
     * Memory-location log meanings:
     *   input = compressed MP3 refill buffer; pcm = decoded output buffer.
     *   decoder identifies the Helix top-level handle location. Helix may own additional internal allocations.
     * A buffer in PSRAM can save scarce internal heap but can also change decode/cache performance.
     */
    ESP_LOGI(TAG,
             "MP3 decode memory (session-start): input=%p(%s,%uB) pcm=%p(%s,%uB) decoder=%p(%s)",
             pInstance->data_buf, mp3_diag_memory_region(pInstance->data_buf),
             (unsigned)pInstance->data_buf_size,
             pData->samples, mp3_diag_memory_region(pData->samples),
             (unsigned)pData->samples_capacity_max,
             mp3_decoder, mp3_diag_memory_region(mp3_decoder));
    mp3_decode_diag_log_heap("session-start");
}

void mp3_decode_diag_session_end(mp3_instance *pInstance)
{
    if (pInstance->diag.calls != 0) {
        mp3_decode_diag_report(pInstance, "session-end", esp_timer_get_time());
    }
}
#endif

bool is_mp3(FILE *fp) {
    bool is_mp3_file = false;

    fseek(fp, 0, SEEK_SET);

    // see https://en.wikipedia.org/wiki/List_of_file_signatures
    uint8_t magic[3];
    if(sizeof(magic) == fread(magic, 1, sizeof(magic), fp)) {
        if((magic[0] == 0xFF) &&
            (magic[1] == 0xFB))
        {
            is_mp3_file = true;
        } else if((magic[0] == 0xFF) &&
                  (magic[1] == 0xF3))
        {
            is_mp3_file = true;
        } else if((magic[0] == 0xFF) &&
                  (magic[1] == 0xF2))
        {
            is_mp3_file = true;
        } else if((magic[0] == 0x49) &&
                  (magic[1] == 0x44) &&
                  (magic[2] == 0x33)) /* 'ID3' */
        {
            fseek(fp, 0, SEEK_SET);

            /* Get ID3 head */
            mp3_id3_header_v2_t tag;
            if (sizeof(mp3_id3_header_v2_t) == fread(&tag, 1, sizeof(mp3_id3_header_v2_t), fp)) {
                if (memcmp("ID3", (const void *) &tag, sizeof(tag.header)) == 0) {
                    is_mp3_file = true;
                }
            }
        }
    }

    // seek back to the start of the file to avoid
    // missing frames upon decode
    fseek(fp, 0, SEEK_SET);

    return is_mp3_file;
}

/**
 * @return true if data remains, false on error or end of file
 */
DECODE_STATUS decode_mp3(HMP3Decoder mp3_decoder, FILE *fp, decode_data *pData, mp3_instance *pInstance) {
    MP3FrameInfo frame_info;

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
    const int64_t call_start_us = esp_timer_get_time();
    int64_t move_us = 0;
    int64_t read_us = 0;
    int64_t sync_us = 0;
    int64_t decode_us = 0;
    size_t decoded_pcm_bytes = 0;
    mp3_decode_diag_t *diag = &pInstance->diag;

    if (diag->last_call_start_us != 0) {
        const int64_t call_gap_us = call_start_us - diag->last_call_start_us;
        diag->call_gap_count++;
        diag->call_gap_total_us += call_gap_us;
        if (call_gap_us > diag->max_call_gap_us) {
            diag->max_call_gap_us = call_gap_us;
        }
        if (call_gap_us >= MP3_DIAG_LATE_CALL_GAP_US) {
            diag->late_call_gaps++;
        }
    }
    diag->last_call_start_us = call_start_us;

    auto finish_decode_call = [&](DECODE_STATUS status) -> DECODE_STATUS {
        const int64_t call_end_us = esp_timer_get_time();
        const int64_t call_us = call_end_us - call_start_us;

        diag->calls++;
        diag->move_total_us += move_us;
        diag->read_total_us += read_us;
        diag->sync_total_us += sync_us;
        diag->decode_total_us += decode_us;
        diag->call_total_us += call_us;
        if (move_us > diag->max_move_us) diag->max_move_us = move_us;
        if (read_us > diag->max_read_us) diag->max_read_us = read_us;
        if (sync_us > diag->max_sync_us) diag->max_sync_us = sync_us;
        if (decode_us > diag->max_decode_us) diag->max_decode_us = decode_us;
        if (call_us > diag->max_call_us) diag->max_call_us = call_us;

        if (decoded_pcm_bytes == 0 && status != DECODE_STATUS_DONE) {
            diag->no_data_calls++;
        }
        if (decoded_pcm_bytes != 0) {
            diag->decoded_frames++;
            diag->pcm_bytes += decoded_pcm_bytes;
        }

        const bool first_frame_report = decoded_pcm_bytes != 0 && diag->first_frame_report_pending;
        if (first_frame_report || (call_end_us - diag->last_report_us) >= MP3_DIAG_REPORT_INTERVAL_US) {
            diag->first_frame_report_pending = false;
            mp3_decode_diag_report(pInstance, first_frame_report ? "first-frame" : "periodic", call_end_us);
        }
        return status;
    };
#endif

    size_t unread_bytes = pInstance->bytes_in_data_buf - (pInstance->read_ptr - pInstance->data_buf);

    /* somewhat arbitrary trigger to refill buffer - should always be enough for a full frame */
    if (unread_bytes < 1.25 * MAINBUF_SIZE && !pInstance->eof_reached) {
        uint8_t *write_ptr = pInstance->data_buf + unread_bytes;
        size_t free_space = pInstance->data_buf_size - unread_bytes;

        /* move last, small chunk from end of buffer to start,
           then fill with new data */
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        const int64_t move_start_us = esp_timer_get_time();
#endif
        memmove(pInstance->data_buf, pInstance->read_ptr, unread_bytes);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        move_us = esp_timer_get_time() - move_start_us;
        const int64_t read_start_us = esp_timer_get_time();
#endif
        size_t nRead = fread(write_ptr, 1, free_space, fp);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        read_us = esp_timer_get_time() - read_start_us;
        diag->refill_calls++;
        diag->refill_bytes += nRead;
#endif

        pInstance->bytes_in_data_buf = unread_bytes + nRead;
        pInstance->read_ptr = pInstance->data_buf;

        if ((nRead == 0) || feof(fp)) {
            pInstance->eof_reached = true;
        }

        LOGI_2("pos %ld, nRead %d, eof %d", ftell(fp), nRead, pInstance->eof_reached);

        unread_bytes = pInstance->bytes_in_data_buf;
    }

    LOGI_3("data_buf 0x%p, read 0x%p", pInstance->data_buf, pInstance->read_ptr);

    if(unread_bytes == 0) {
        LOGI_1("unread_bytes == 0, status done");
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        return finish_decode_call(DECODE_STATUS_DONE);
#else
        return DECODE_STATUS_DONE;
#endif
    }

    /* Find MP3 sync word from read buffer */
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
    const int64_t sync_start_us = esp_timer_get_time();
#endif
    int offset = MP3FindSyncWord(pInstance->read_ptr, unread_bytes);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
    sync_us = esp_timer_get_time() - sync_start_us;
    diag->sync_calls++;
#endif

    LOGI_2("unread %d, total %d, offset 0x%x(%d)",
            unread_bytes, pInstance->bytes_in_data_buf, offset, offset);

    if (offset >= 0) {
        COMPILE_3(int starting_unread_bytes = unread_bytes);
        uint8_t *read_ptr = pInstance->read_ptr + offset; /*!< Data start point */
        unread_bytes -= offset;
        LOGI_3("read 0x%p, unread %d", read_ptr, unread_bytes);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        const int64_t decode_start_us = esp_timer_get_time();
        diag->decode_calls++;
#endif
        int mp3_dec_err = MP3Decode(mp3_decoder, &read_ptr, (int*)&unread_bytes,
                                    reinterpret_cast<int16_t *>(pData->samples), 0);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        decode_us = esp_timer_get_time() - decode_start_us;
#endif

        pInstance->read_ptr = read_ptr;

        if(mp3_dec_err == ERR_MP3_NONE) {
            /* Get MP3 frame info */
            MP3GetLastFrameInfo(mp3_decoder, &frame_info);

            pData->fmt.sample_rate = frame_info.samprate;
            pData->fmt.bits_per_sample = frame_info.bitsPerSample;
            pData->fmt.channels = frame_info.nChans;

            pData->frame_count = (frame_info.outputSamps / frame_info.nChans);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
            decoded_pcm_bytes = pData->frame_count * pData->fmt.channels * (pData->fmt.bits_per_sample / 8);
#endif

            LOGI_3("mp3: channels %d, sr %d, bps %d, frame_count %d, processed %d",
                pData->fmt.channels,
                pData->fmt.sample_rate,
                pData->fmt.bits_per_sample,
                frame_info.outputSamps,
                starting_unread_bytes - unread_bytes);
        } else {
            if (pInstance->eof_reached) {
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                diag->decode_errors++;
#endif
                ESP_LOGE(TAG, "status error %d, but EOF", mp3_dec_err);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                return finish_decode_call(DECODE_STATUS_DONE);
#else
                return DECODE_STATUS_DONE;
#endif
            } else if (mp3_dec_err == ERR_MP3_MAINDATA_UNDERFLOW) {
                // underflow indicates MP3Decode should be called again
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                diag->underflows++;
#endif
                LOGI_1("underflow read ptr is 0x%p", read_ptr);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                return finish_decode_call(DECODE_STATUS_NO_DATA_CONTINUE);
#else
                return DECODE_STATUS_NO_DATA_CONTINUE;
#endif
            } else {
                // NOTE: some mp3 files result in misdetection of mp3 frame headers
                // and during decode these misdetected frames cannot be
                // decoded
                //
                // Rather than give up on the file by returning
                // DECODE_STATUS_ERROR, we ask the caller
                // to continue to call us, by returning DECODE_STATUS_NO_DATA_CONTINUE.
                //
                // The invalid frame data is skipped over as a search for the next frame
                // on the subsequent call to this function will start searching
                // AFTER the misdetected frmame header, dropping the invalid data.
                //
                // We may want to consider a more sophisticated approach here at a later time.
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                diag->decode_errors++;
#endif
                ESP_LOGE(TAG, "status error %d", mp3_dec_err);
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
                return finish_decode_call(DECODE_STATUS_NO_DATA_CONTINUE);
#else
                return DECODE_STATUS_NO_DATA_CONTINUE;
#endif
            }
        }
    } else {
        // if we are dropping data there were no frames decoded
        pData->frame_count = 0;
#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
        diag->sync_misses++;
#endif

        // drop an even count of words
        size_t words_to_drop = unread_bytes / BYTES_IN_WORD;
        size_t bytes_to_drop = words_to_drop * BYTES_IN_WORD;

        // if the unread bytes is less than BYTES_IN_WORD, we should drop any unread bytes
        // to avoid the situation where the file could have a few extra bytes at the end
        // of the file that isn't at least BYTES_IN_WORD and decoding would get stuck
        if(unread_bytes < BYTES_IN_WORD) {
            bytes_to_drop = unread_bytes;
        }

        // shift the read_ptr to drop the bytes in the buffer
        pInstance->read_ptr += bytes_to_drop;

        /* Sync word not found in frame. Drop data that was read until a word boundary */
        ESP_LOGE(TAG, "MP3 sync word not found, dropping %d bytes", bytes_to_drop);
    }

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
    return finish_decode_call(DECODE_STATUS_CONTINUE);
#else
    return DECODE_STATUS_CONTINUE;
#endif
}
