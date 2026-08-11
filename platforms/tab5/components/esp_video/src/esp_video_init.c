/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <inttypes.h>
#include <sys/lock.h>
#include "hal/gpio_ll.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_sccb_i2c.h"
#include "esp_cam_sensor_detect.h"
#include "esp_cam_sensor.h"

#include "esp_video_init.h"
#include "esp_video_device_internal.h"
#include "esp_private/esp_cam_dvp.h"
#include "esp_video_pipeline_isp.h"

#define SCCB_NUM_MAX I2C_NUM_MAX

#define INTF_PORT_NAME(port) (((port) == ESP_CAM_SENSOR_DVP) ? "DVP" : "CSI")

/**
 * @brief SCCB initialization mark
 */
typedef struct sccb_mark {
    i2c_master_bus_handle_t handle;             /*!< I2C master handle */
    const esp_video_init_sccb_config_t *config; /*!< SCCB initialization config pointer */
    uint16_t dev_addr;                          /*!< Slave device address */
    esp_cam_sensor_port_t port;                 /*!< Slave device data interface */
} esp_video_init_sccb_mark_t;

static const char *TAG = "esp_video_init";
static _lock_t s_init_lock;
static bool s_video_initialized;

typedef struct esp_video_init_context {
    esp_video_init_sccb_mark_t sccb_mark[SCCB_NUM_MAX];
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    esp_sccb_io_handle_t csi_sccb;
    esp_cam_sensor_device_t *csi_cam_dev;
    struct esp_video *csi_video;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    bool dvp_ctlr_initialized;
    esp_sccb_io_handle_t dvp_sccb;
    esp_cam_sensor_device_t *dvp_cam_dev;
    struct esp_video *dvp_video;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE
    struct esp_video *h264_video;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_HW_JPEG_VIDEO_DEVICE
    struct esp_video *jpeg_video;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
    struct esp_video *isp_video;
#endif
} esp_video_init_context_t;

static void log_cleanup_error(const char *resource, esp_err_t ret)
{
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to release %s: %s", resource, esp_err_to_name(ret));
    }
}

static void rollback_video_init(esp_video_init_context_t *ctx)
{
#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
    log_cleanup_error("ISP video device", esp_video_destroy_isp_video_device(ctx->isp_video));
    ctx->isp_video = NULL;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_HW_JPEG_VIDEO_DEVICE
    log_cleanup_error("JPEG video device", esp_video_destroy_jpeg_video_device(ctx->jpeg_video));
    ctx->jpeg_video = NULL;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE
    log_cleanup_error("H.264 video device", esp_video_destroy_h264_video_device(ctx->h264_video));
    ctx->h264_video = NULL;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    log_cleanup_error("DVP video device", esp_video_destroy_dvp_video_device(ctx->dvp_video));
    ctx->dvp_video = NULL;
#endif
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    log_cleanup_error("MIPI-CSI video device", esp_video_destroy_csi_video_device(ctx->csi_video));
    ctx->csi_video = NULL;
#endif

#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    if (ctx->dvp_cam_dev) {
        log_cleanup_error("DVP camera sensor", esp_cam_sensor_del_dev(ctx->dvp_cam_dev));
        ctx->dvp_cam_dev = NULL;
    }
    if (ctx->dvp_sccb) {
        log_cleanup_error("DVP SCCB device", esp_sccb_del_i2c_io(ctx->dvp_sccb));
        ctx->dvp_sccb = NULL;
    }
    if (ctx->dvp_ctlr_initialized) {
        log_cleanup_error("DVP controller", esp_cam_ctlr_dvp_deinit(0));
        ctx->dvp_ctlr_initialized = false;
    }
#endif
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
    if (ctx->csi_cam_dev) {
        log_cleanup_error("MIPI-CSI camera sensor", esp_cam_sensor_del_dev(ctx->csi_cam_dev));
        ctx->csi_cam_dev = NULL;
    }
    if (ctx->csi_sccb) {
        log_cleanup_error("MIPI-CSI SCCB device", esp_sccb_del_i2c_io(ctx->csi_sccb));
        ctx->csi_sccb = NULL;
    }
#endif

#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE || CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
    for (int i = 0; i < SCCB_NUM_MAX; ++i) {
        if (ctx->sccb_mark[i].handle) {
            log_cleanup_error("internally owned I2C bus", i2c_del_master_bus(ctx->sccb_mark[i].handle));
            ctx->sccb_mark[i].handle = NULL;
        }
    }
#endif
}

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
static const char *s_default_ipa_names[] = {
#if CONFIG_ESP_IPA_AWB_GRAY_WORLD
    "awb.gray",
#endif
#if CONFIG_ESP_IPA_AGC_THRESHOLD
    "agc.threshold",
#endif
#if CONFIG_ESP_IPA_DENOISING_GAIN_FEEDBACK
    "denoising.gf",
#endif
#if CONFIG_ESP_IPA_SHARPEN_FREQUENCY_FEEDBACK
    "sharpen.ff",
#endif
#if CONFIG_ESP_IPA_GAMMA_LUMA
    "gamma.lf",
#endif
#if CONFIG_ESP_IPA_CC_LINEAR
    "cc.linear",
#endif
};

const esp_video_init_isp_config_t s_ipa_config = {
    .ipa_nums = sizeof(s_default_ipa_names) / sizeof(s_default_ipa_names[0]), .ipa_names = s_default_ipa_names};
#endif

#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE || CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
/**
 * @brief Create I2C master handle
 *
 * @param mark SCCB initialization make array
 * @param port Slave device data interface
 * @param init_sccb_config SCCB initialization configuration
 * @param dev_addr device address
 *
 * @return
 *      - I2C master handle on success
 *      - NULL if failed
 */
static i2c_master_bus_handle_t create_i2c_master_bus(esp_video_init_sccb_mark_t *mark, esp_cam_sensor_port_t port,
                                                     const esp_video_init_sccb_config_t *init_sccb_config,
                                                     uint16_t dev_addr)
{
    esp_err_t ret;
    i2c_master_bus_handle_t bus_handle = NULL;
    int i2c_port                       = init_sccb_config->i2c_config.port;

    if (i2c_port < 0 || i2c_port >= SCCB_NUM_MAX) {
        return NULL;
    }

    if (mark[i2c_port].handle != NULL) {
        if (init_sccb_config->i2c_config.scl_pin != mark[i2c_port].config->i2c_config.scl_pin) {
            ESP_LOGE(TAG, "Interface %s and %s: I2C port %d SCL pin is mismatched", INTF_PORT_NAME(i2c_port),
                     INTF_PORT_NAME(mark[i2c_port].port), i2c_port);
            return NULL;
        }

        if (init_sccb_config->i2c_config.sda_pin != mark[i2c_port].config->i2c_config.sda_pin) {
            ESP_LOGE(TAG, "Interface %s and %s: I2C port %d SDA pin is mismatched", INTF_PORT_NAME(i2c_port),
                     INTF_PORT_NAME(mark[i2c_port].port), i2c_port);
            return NULL;
        }

        if (dev_addr == mark[i2c_port].dev_addr) {
            ESP_LOGE(TAG, "Interface %s and %s: use same SCCB device address %d", INTF_PORT_NAME(i2c_port),
                     INTF_PORT_NAME(mark[i2c_port].port), dev_addr);
            return NULL;
        }

        bus_handle = mark[i2c_port].handle;
    } else {
        i2c_master_bus_config_t i2c_bus_config = {0};

        i2c_bus_config.clk_source                   = I2C_CLK_SRC_DEFAULT;
        i2c_bus_config.i2c_port                     = init_sccb_config->i2c_config.port;
        i2c_bus_config.scl_io_num                   = init_sccb_config->i2c_config.scl_pin;
        i2c_bus_config.sda_io_num                   = init_sccb_config->i2c_config.sda_pin;
        i2c_bus_config.glitch_ignore_cnt            = 7;
        i2c_bus_config.flags.enable_internal_pullup = true,

        ret = i2c_new_master_bus(&i2c_bus_config, &bus_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to initialize I2C master bus port %d", init_sccb_config->i2c_config.port);
            return NULL;
        }

        mark[i2c_port].handle   = bus_handle;
        mark[i2c_port].config   = init_sccb_config;
        mark[i2c_port].dev_addr = dev_addr;
        mark[i2c_port].port     = port;
    }

    return bus_handle;
}

/**
 * @brief Create SCCB device
 *
 * @param mark SCCB initialization make array
 * @param port Slave device data interface
 * @param init_sccb_config SCCB initialization configuration
 * @param dev_addr device address
 *
 * @return
 *      - SCCB handle on success
 *      - NULL if failed
 */
static esp_sccb_io_handle_t create_sccb_device(esp_video_init_sccb_mark_t *mark, esp_cam_sensor_port_t port,
                                               const esp_video_init_sccb_config_t *init_sccb_config, uint16_t dev_addr)
{
    esp_err_t ret;
    esp_sccb_io_handle_t sccb_io;
    sccb_i2c_config_t sccb_config = {0};
    i2c_master_bus_handle_t bus_handle;

    if (init_sccb_config->init_sccb) {
        bus_handle = create_i2c_master_bus(mark, port, init_sccb_config, dev_addr);
    } else {
        bus_handle = init_sccb_config->i2c_handle;
    }

    if (!bus_handle) {
        return NULL;
    }

    sccb_config.dev_addr_length = I2C_ADDR_BIT_LEN_7, sccb_config.device_address = dev_addr,
    sccb_config.scl_speed_hz = init_sccb_config->freq, ret = sccb_new_i2c_io(bus_handle, &sccb_config, &sccb_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize SCCB");
        return NULL;
    }

    return sccb_io;
}
#endif

/**
 * @brief Initialize video hardware and software, including I2C, MIPI CSI and so on.
 *
 * @param config video hardware configuration
 *
 * @return
 *      - ESP_OK on success
 *      - Others if failed
 */
esp_err_t esp_video_init(const esp_video_init_config_t *config)
{
    esp_err_t ret = ESP_OK;
    esp_video_init_context_t ctx = {0};

    if (config == NULL) {
        ESP_LOGW(TAG, "Please validate camera config");
        return ESP_ERR_INVALID_ARG;
    }

    _lock_acquire(&s_init_lock);
    if (s_video_initialized) {
        ESP_LOGD(TAG, "video system has already been initialized");
        _lock_release(&s_init_lock);
        return ESP_OK;
    }

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
    const esp_video_init_isp_config_t *ipa_config;

    if (config->isp == NULL || !config->isp->ipa_nums || !config->isp->ipa_names) {
        ESP_LOGW(TAG, "ISP config is null and use default IPA config");
        ipa_config = &s_ipa_config;
    } else {
        ipa_config = config->isp;
    }
#endif

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
#if CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE
        if (p->port == ESP_CAM_SENSOR_MIPI_CSI && config->csi != NULL && ctx.csi_video == NULL) {
            esp_cam_sensor_config_t cfg = {0};

            ctx.csi_sccb =
                create_sccb_device(ctx.sccb_mark, ESP_CAM_SENSOR_MIPI_CSI, &config->csi->sccb_config, p->sccb_addr);
            if (!ctx.csi_sccb) {
                ret = ESP_FAIL;
                goto fail;
            }

            cfg.sccb_handle = ctx.csi_sccb;
            cfg.reset_pin   = config->csi->reset_pin;
            cfg.pwdn_pin    = config->csi->pwdn_pin;
            ctx.csi_cam_dev = (*(p->detect))((void *)&cfg);
            if (!ctx.csi_cam_dev) {
                ESP_LOGE(TAG, "failed to detect MIPI-CSI camera");
                ret = ESP_FAIL;
                goto fail;
            }

            ret = esp_video_create_csi_video_device(ctx.csi_cam_dev, &ctx.csi_video);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "failed to create MIPI-CSI video device");
                goto fail;
            }
        }
#endif

#if CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE
        if (p->port == ESP_CAM_SENSOR_DVP && config->dvp != NULL && ctx.dvp_video == NULL) {
            int dvp_ctlr_id = 0;
            esp_cam_sensor_config_t cfg = {0};

            ret = esp_cam_ctlr_dvp_init(dvp_ctlr_id, CAM_CLK_SRC_DEFAULT, &config->dvp->dvp_pin);
            if (ret != ESP_OK) {
                goto fail;
            }
            ctx.dvp_ctlr_initialized = true;

            if (config->dvp->dvp_pin.xclk_io >= 0 && config->dvp->xclk_freq > 0) {
                ret = esp_cam_ctlr_dvp_output_clock(dvp_ctlr_id, CAM_CLK_SRC_DEFAULT, config->dvp->xclk_freq);
                if (ret != ESP_OK) {
                    goto fail;
                }
            }

            ctx.dvp_sccb =
                create_sccb_device(ctx.sccb_mark, ESP_CAM_SENSOR_DVP, &config->dvp->sccb_config, p->sccb_addr);
            if (!ctx.dvp_sccb) {
                ret = ESP_FAIL;
                goto fail;
            }

            cfg.sccb_handle = ctx.dvp_sccb;
            cfg.reset_pin   = config->dvp->reset_pin;
            cfg.pwdn_pin    = config->dvp->pwdn_pin;
            ctx.dvp_cam_dev = (*(p->detect))((void *)&cfg);
            if (!ctx.dvp_cam_dev) {
                ESP_LOGE(TAG, "failed to detect DVP camera");
                ret = ESP_FAIL;
                goto fail;
            }

            ret = esp_video_create_dvp_video_device(ctx.dvp_cam_dev, &ctx.dvp_video);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "failed to create DVP video device");
                goto fail;
            }
        }
#endif
    }

#if CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE
    ret = esp_video_create_h264_video_device(true, &ctx.h264_video);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create hardware H.264 video device");
        goto fail;
    }
#endif

#if CONFIG_ESP_VIDEO_ENABLE_HW_JPEG_VIDEO_DEVICE
    jpeg_encoder_handle_t handle = NULL;

    if (config->jpeg) {
        handle = config->jpeg->enc_handle;
    }

    ret = esp_video_create_jpeg_video_device(handle, &ctx.jpeg_video);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create hardware JPEG video device");
        goto fail;
    }
#endif

#if CONFIG_ESP_VIDEO_ENABLE_ISP_VIDEO_DEVICE
    ret = esp_video_create_isp_video_device(&ctx.isp_video);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create hardware ISP video device");
        goto fail;
    }
#endif

#if CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
    esp_video_isp_config_t isp_config = {
        .cam_dev   = ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
        .isp_dev   = ESP_VIDEO_ISP1_DEVICE_NAME,
        .ipa_nums  = ipa_config->ipa_nums,
        .ipa_names = ipa_config->ipa_names,
    };

    ret = esp_video_isp_pipeline_init(&isp_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create ISP system");
        goto fail;
    }
#endif

    s_video_initialized = true;
    _lock_release(&s_init_lock);
    return ESP_OK;

fail:
    rollback_video_init(&ctx);
    _lock_release(&s_init_lock);
    return ret;
}
