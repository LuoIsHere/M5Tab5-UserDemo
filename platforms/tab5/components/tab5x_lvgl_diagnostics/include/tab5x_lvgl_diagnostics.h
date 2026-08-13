/*
 * Project-local LVGL freeze diagnostics.
 *
 * All checkpoints are controlled by CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS:
 * A:  before acquiring lvgl_mux
 * B:  after acquiring lvgl_mux
 * C:  before acquiring timer_mux
 * D:  after acquiring timer_mux
 * E:  before the explicit lv_indev_read() call in the LVGL port task
 * F:  after the explicit lv_indev_read() call in the LVGL port task
 * G:  before lv_timer_handler()
 * H:  after lv_timer_handler()
 * I:  after releasing lvgl_mux
 * J:  before each LVGL timer callback
 * K:  after each LVGL timer callback
 * L:  at entry to the display refresh timer callback
 * M:  when leaving the display refresh timer callback
 * N:  before rotate_copy_pixel()
 * O:  after rotate_copy_pixel()
 * P:  before the blocking display ppa_do_scale_rotate_mirror() call
 * Q:  after the blocking display ppa_do_scale_rotate_mirror() call
 * R:  before esp_lcd_panel_draw_bitmap()
 * S:  after esp_lcd_panel_draw_bitmap(); last_draw_result records its return value
 * T:  after esp_lcd_panel_draw_bitmap() successfully accepts a display transfer
 * U:  when a display transfer completion callback is entered
 * V:  before lv_disp_flush_ready()
 * W:  after lv_disp_flush_ready()
 * X:  when entering wait_for_flushing()
 * Y:  after wait_for_flushing() completes
 * Z:  before the esp_lvgl_port/ST712x touch read callback body
 * AA: after the esp_lvgl_port/ST712x touch read callback body
 * AB: before the BSP GT911 touch read callback body
 * AC: after the BSP GT911 touch read callback body
 * AD: before the USB mouse read callback body
 * AE: after the USB mouse read callback body
 * AF: before the camera's blocking ppa_do_scale_rotate_mirror() call
 * AG: after the camera's blocking ppa_do_scale_rotate_mirror() call
 *
 * P/Q encompass both the PPA engine-semaphore wait and transaction-completion
 * wait inside ESP-IDF. Those waits cannot be counted separately without
 * modifying the external ESP-IDF ppa_core.c, which this project does not do.
 */
#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TAB5X_LVGL_DIAG_A = 0,
    TAB5X_LVGL_DIAG_B,
    TAB5X_LVGL_DIAG_C,
    TAB5X_LVGL_DIAG_D,
    TAB5X_LVGL_DIAG_E,
    TAB5X_LVGL_DIAG_F,
    TAB5X_LVGL_DIAG_G,
    TAB5X_LVGL_DIAG_H,
    TAB5X_LVGL_DIAG_I,
    TAB5X_LVGL_DIAG_J,
    TAB5X_LVGL_DIAG_K,
    TAB5X_LVGL_DIAG_L,
    TAB5X_LVGL_DIAG_M,
    TAB5X_LVGL_DIAG_N,
    TAB5X_LVGL_DIAG_O,
    TAB5X_LVGL_DIAG_P,
    TAB5X_LVGL_DIAG_Q,
    TAB5X_LVGL_DIAG_R,
    TAB5X_LVGL_DIAG_S,
    TAB5X_LVGL_DIAG_T,
    TAB5X_LVGL_DIAG_U,
    TAB5X_LVGL_DIAG_V,
    TAB5X_LVGL_DIAG_W,
    TAB5X_LVGL_DIAG_X,
    TAB5X_LVGL_DIAG_Y,
    TAB5X_LVGL_DIAG_Z,
    TAB5X_LVGL_DIAG_AA,
    TAB5X_LVGL_DIAG_AB,
    TAB5X_LVGL_DIAG_AC,
    TAB5X_LVGL_DIAG_AD,
    TAB5X_LVGL_DIAG_AE,
    TAB5X_LVGL_DIAG_AF,
    TAB5X_LVGL_DIAG_AG,
    TAB5X_LVGL_DIAG_COUNT,
} tab5x_lvgl_diag_checkpoint_t;

#if defined(CONFIG_TAB5X_LVGL_TASK_DIAGNOSTICS)
void tab5x_lvgl_diagnostics_init(void);
void tab5x_lvgl_diagnostics_deinit(void);
void tab5x_lvgl_diagnostics_hit(tab5x_lvgl_diag_checkpoint_t checkpoint);
void tab5x_lvgl_diagnostics_set_last_timer(const void *timer, const void *callback);
void tab5x_lvgl_diagnostics_set_last_draw_result(int32_t result);
#else
static inline void tab5x_lvgl_diagnostics_init(void) {}
static inline void tab5x_lvgl_diagnostics_deinit(void) {}
static inline void tab5x_lvgl_diagnostics_hit(tab5x_lvgl_diag_checkpoint_t checkpoint)
{
    (void)checkpoint;
}
static inline void tab5x_lvgl_diagnostics_set_last_timer(const void *timer, const void *callback)
{
    (void)timer;
    (void)callback;
}
static inline void tab5x_lvgl_diagnostics_set_last_draw_result(int32_t result)
{
    (void)result;
}
#endif

#define TAB5X_LVGL_DIAG_HIT(checkpoint) tab5x_lvgl_diagnostics_hit(TAB5X_LVGL_DIAG_##checkpoint)

#ifdef __cplusplus
}
#endif
