#pragma once

#include <stdio.h>
#include "sdkconfig.h"
#include "audio_decode_types.h"
#include "mp3dec.h"

typedef struct {
    char header[3];     /*!< Always "TAG" */
    char title[30];     /*!< Audio title */
    char artist[30];    /*!< Audio artist */
    char album[30];     /*!< Album name */
    char year[4];       /*!< Char array of year */
    char comment[30];   /*!< Extra comment */
    char genre;         /*!< See "https://en.wikipedia.org/wiki/ID3" */
} __attribute__((packed)) mp3_id3_header_v1_t;

typedef struct {
    char header[3];     /*!< Always "ID3" */
    char ver;           /*!< Version, equals to3 if ID3V2.3 */
    char revision;      /*!< Revision, should be 0 */
    char flag;          /*!< Flag byte, use Bit[7..5] only */
    char size[4];       /*!< TAG size */
} __attribute__((packed)) mp3_id3_header_v2_t;

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
typedef struct {
    uint64_t calls;
    uint64_t decoded_frames;
    uint64_t pcm_bytes;
    uint64_t no_data_calls;
    uint64_t refill_calls;
    uint64_t refill_bytes;
    uint64_t sync_calls;
    uint64_t sync_misses;
    uint64_t decode_calls;
    uint64_t underflows;
    uint64_t decode_errors;
    uint64_t late_call_gaps;
    uint64_t call_gap_count;
    uint64_t call_gap_total_us;
    uint64_t move_total_us;
    uint64_t read_total_us;
    uint64_t sync_total_us;
    uint64_t decode_total_us;
    uint64_t call_total_us;
    int64_t max_call_gap_us;
    int64_t max_move_us;
    int64_t max_read_us;
    int64_t max_sync_us;
    int64_t max_decode_us;
    int64_t max_call_us;
    int64_t last_call_start_us;
    int64_t last_report_us;
    bool first_frame_report_pending;
} mp3_decode_diag_t;
#endif

typedef struct {
    // Constants below
    uint8_t *data_buf;

    /** number of bytes in data_buf */
    size_t data_buf_size;

    // Values that change at runtime are below

    /**
     * Total bytes in data_buf,
     * not the number of bytes remaining after the read_ptr
     */
    size_t bytes_in_data_buf;

    /** Pointer to read location in data_buf */
    uint8_t *read_ptr;

    // set to true if the end of file has been reached
    bool eof_reached;

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
    // Per-player-instance counters prevent concurrent players from mixing diagnostic sessions.
    mp3_decode_diag_t diag;
#endif
} mp3_instance;

bool is_mp3(FILE *fp);
DECODE_STATUS decode_mp3(HMP3Decoder mp3_decoder, FILE *fp, decode_data *pData, mp3_instance *pInstance);

#if defined(CONFIG_TAB5X_MP3_DECODE_DIAGNOSTICS)
void mp3_decode_diag_session_start(HMP3Decoder mp3_decoder, const decode_data *pData, mp3_instance *pInstance);
void mp3_decode_diag_session_end(mp3_instance *pInstance);
#endif
