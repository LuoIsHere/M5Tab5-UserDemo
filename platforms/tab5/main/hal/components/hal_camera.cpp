/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <inttypes.h>
#include <mutex>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/errno.h>
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/ppa.h"

#define CAMERA_WIDTH  1280
#define CAMERA_HEIGHT 720

static const char* TAG = "camera";

#define EXAMPLE_VIDEO_BUFFER_COUNT 2
#define MEMORY_TYPE                V4L2_MEMORY_MMAP
#define CAM_DEV_PATH               ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAMERA_TASK_STACK_SIZE     (8 * 1024)
#define CAMERA_TASK_PRIORITY       5
#define CAMERA_STOP_TIMEOUT_MS     2000
#define CAMERA_STACK_LOG_MS        5000
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

typedef struct cam {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t* buffer[EXAMPLE_VIDEO_BUFFER_COUNT];
    size_t buffer_length[EXAMPLE_VIDEO_BUFFER_COUNT];
    uint32_t buffer_count;
    bool buffers_requested;
    bool streaming;
} cam_t;

typedef enum {
    EXAMPLE_VIDEO_FMT_RAW8   = V4L2_PIX_FMT_SBGGR8,
    EXAMPLE_VIDEO_FMT_RAW10  = V4L2_PIX_FMT_SBGGR10,
    EXAMPLE_VIDEO_FMT_GREY   = V4L2_PIX_FMT_GREY,
    EXAMPLE_VIDEO_FMT_RGB565 = V4L2_PIX_FMT_RGB565,
    EXAMPLE_VIDEO_FMT_RGB888 = V4L2_PIX_FMT_RGB24,
    EXAMPLE_VIDEO_FMT_YUV422 = V4L2_PIX_FMT_YUV422P,
    EXAMPLE_VIDEO_FMT_YUV420 = V4L2_PIX_FMT_YUV420,
} example_fmt_t;

enum class CameraState {
    CLOSED,
    STARTING,
    STREAMING,
    STOPPING,
    ERROR,
};

struct CameraContext {
    std::mutex mutex;
    CameraState state = CameraState::CLOSED;
    TaskHandle_t task_handle = nullptr;
    SemaphoreHandle_t task_exited = nullptr;
    lv_obj_t* canvas = nullptr;
    esp_err_t last_error = ESP_OK;
    bool video_initialized = false;
    bool canvas_uses_display_buffer = false;
    cam_t* camera = nullptr;
    ppa_client_handle_t ppa_handle = nullptr;
    uint8_t* display_buffer = nullptr;
};

static CameraContext camera_ctx;
static uint16_t camera_canvas_placeholder;

static const char* camera_state_name(CameraState state)
{
    switch (state) {
        case CameraState::CLOSED:
            return "CLOSED";
        case CameraState::STARTING:
            return "STARTING";
        case CameraState::STREAMING:
            return "STREAMING";
        case CameraState::STOPPING:
            return "STOPPING";
        case CameraState::ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

static bool camera_stop_requested()
{
    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    return camera_ctx.state == CameraState::STOPPING;
}

static bool camera_mark_streaming()
{
    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    if (camera_ctx.state == CameraState::STOPPING) {
        return false;
    }
    if (camera_ctx.state != CameraState::STARTING) {
        camera_ctx.last_error = ESP_ERR_INVALID_STATE;
        camera_ctx.state = CameraState::ERROR;
        return false;
    }
    camera_ctx.state = CameraState::STREAMING;
    return true;
}

static bool camera_canvas_attach(lv_obj_t* canvas, uint8_t* display_buffer)
{
    if (canvas == nullptr || display_buffer == nullptr) {
        return false;
    }
    if (!bsp_display_lock(100)) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL lock while publishing camera frame");
        return false;
    }

    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.state == CameraState::STREAMING && camera_ctx.canvas == canvas) {
            lv_canvas_set_buffer(canvas, display_buffer, CAMERA_WIDTH, CAMERA_HEIGHT, LV_COLOR_FORMAT_RGB565);
            camera_ctx.canvas_uses_display_buffer = true;
            attached = true;
        }
    }
    bsp_display_unlock();
    return attached;
}

static void camera_canvas_detach(lv_obj_t* canvas)
{
    if (canvas == nullptr) {
        return;
    }

    // timeout_ms == 0 means portMAX_DELAY. stopCameraCapture() invokes this before
    // waiting, so cleanup never waits on a LVGL lock held by the stopping caller.
    if (!bsp_display_lock(0)) {
        return;
    }
    lv_canvas_set_buffer(canvas, &camera_canvas_placeholder, 1, 1, LV_COLOR_FORMAT_RGB565);
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.canvas == canvas) {
            camera_ctx.canvas_uses_display_buffer = false;
        }
    }
    bsp_display_unlock();
}

static bool camera_canvas_needs_detach()
{
    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    return camera_ctx.canvas_uses_display_buffer;
}

static int app_video_open(const char* dev, example_fmt_t init_fmt)
{
    struct v4l2_format default_format = {};
    struct v4l2_capability capability = {};
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open %s failed: errno=%d (%s)", dev, errno, strerror(errno));
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0) {
        ESP_LOGE(TAG, "Failed to get capability: errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability.version >> 16),
             (uint8_t)(capability.version >> 8), (uint8_t)capability.version);
    ESP_LOGI(TAG, "driver:  %s", capability.driver);
    ESP_LOGI(TAG, "card:    %s", capability.card);
    ESP_LOGI(TAG, "bus:     %s", capability.bus_info);

    default_format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &default_format) != 0) {
        ESP_LOGE(TAG, "Failed to get format: errno=%d (%s)", errno, strerror(errno));
        goto fail;
    }

    ESP_LOGI(TAG, "width=%" PRIu32 " height=%" PRIu32, default_format.fmt.pix.width,
             default_format.fmt.pix.height);

    if (default_format.fmt.pix.pixelformat != init_fmt) {
        struct v4l2_format format = {};
        format.type = type;
        format.fmt.pix.width = default_format.fmt.pix.width;
        format.fmt.pix.height = default_format.fmt.pix.height;
        format.fmt.pix.pixelformat = init_fmt;

        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG, "Failed to set format: errno=%d (%s)", errno, strerror(errno));
            goto fail;
        }
    }

    return fd;

fail:
    close(fd);
    return -1;
}

static esp_err_t camera_stream_off(cam_t* camera)
{
    if (camera == nullptr || !camera->streaming) {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMOFF failed: errno=%d (%s)", errno, strerror(errno));
        camera->streaming = false;
        return ESP_FAIL;
    }
    camera->streaming = false;
    return ESP_OK;
}

static esp_err_t delete_cam(cam_t** camera_ptr)
{
    if (camera_ptr == nullptr || *camera_ptr == nullptr) {
        return ESP_OK;
    }

    cam_t* camera = *camera_ptr;
    esp_err_t result = camera_stream_off(camera);

    for (uint32_t i = 0; i < camera->buffer_count && i < ARRAY_SIZE(camera->buffer); ++i) {
        if (camera->buffer[i] != nullptr) {
            if (munmap(camera->buffer[i], camera->buffer_length[i]) != 0) {
                ESP_LOGE(TAG, "munmap buffer %" PRIu32 " failed: errno=%d (%s)", i, errno, strerror(errno));
                result = ESP_FAIL;
            }
            camera->buffer[i] = nullptr;
            camera->buffer_length[i] = 0;
        }
    }

    if (camera->buffers_requested && camera->fd >= 0) {
        struct v4l2_requestbuffers req = {};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = MEMORY_TYPE;
        if (ioctl(camera->fd, VIDIOC_REQBUFS, &req) != 0) {
            ESP_LOGE(TAG, "VIDIOC_REQBUFS(count=0) failed: errno=%d (%s)", errno, strerror(errno));
            result = ESP_FAIL;
        }
        camera->buffers_requested = false;
    }

    if (camera->fd >= 0) {
        if (close(camera->fd) != 0) {
            ESP_LOGE(TAG, "Close camera fd failed: errno=%d (%s)", errno, strerror(errno));
            result = ESP_FAIL;
        }
        camera->fd = -1;
    }

    free(camera);
    *camera_ptr = nullptr;
    return result;
}

static esp_err_t new_cam(int cam_fd, cam_t** ret_camera)
{
    if (cam_fd < 0 || ret_camera == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_camera = nullptr;

    struct v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "Failed to get camera format: errno=%d (%s)", errno, strerror(errno));
        close(cam_fd);
        return ESP_FAIL;
    }

    cam_t* camera = static_cast<cam_t*>(calloc(1, sizeof(cam_t)));
    if (camera == nullptr) {
        close(cam_fd);
        return ESP_ERR_NO_MEM;
    }

    camera->fd = cam_fd;
    camera->width = format.fmt.pix.width;
    camera->height = format.fmt.pix.height;
    camera->pixel_format = format.fmt.pix.pixelformat;

    struct v4l2_requestbuffers req = {};
    req.count = ARRAY_SIZE(camera->buffer);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = MEMORY_TYPE;
    if (ioctl(camera->fd, VIDIOC_REQBUFS, &req) != 0 || req.count == 0) {
        ESP_LOGE(TAG, "Failed to request camera buffers: errno=%d (%s)", errno, strerror(errno));
        delete_cam(&camera);
        return ESP_FAIL;
    }
    camera->buffers_requested = true;
    camera->buffer_count = req.count < ARRAY_SIZE(camera->buffer) ? req.count : ARRAY_SIZE(camera->buffer);

    for (uint32_t i = 0; i < camera->buffer_count; ++i) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = MEMORY_TYPE;
        buf.index = i;
        if (ioctl(camera->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Failed to query camera buffer %" PRIu32 ": errno=%d (%s)", i, errno,
                     strerror(errno));
            delete_cam(&camera);
            return ESP_FAIL;
        }

        void* mapped = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buf.m.offset);
        if (mapped == nullptr) {
            ESP_LOGE(TAG, "Failed to map camera buffer %" PRIu32 ": errno=%d (%s)", i, errno,
                     strerror(errno));
            delete_cam(&camera);
            return ESP_FAIL;
        }
        camera->buffer[i] = static_cast<uint8_t*>(mapped);
        camera->buffer_length[i] = buf.length;

        if (ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "Failed to queue camera buffer %" PRIu32 ": errno=%d (%s)", i, errno,
                     strerror(errno));
            delete_cam(&camera);
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "Failed to start camera stream: errno=%d (%s)", errno, strerror(errno));
        delete_cam(&camera);
        return ESP_FAIL;
    }
    camera->streaming = true;

    *ret_camera = camera;
    return ESP_OK;
}

static esp_err_t ensure_video_initialized()
{
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.video_initialized) {
            return ESP_OK;
        }
    }

    static esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = nullptr,
            .freq = 400000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    csi_config.sccb_config.i2c_handle = bsp_i2c_get_handle();

    esp_video_init_config_t camera_config = {
        .csi = &csi_config,
        .dvp = nullptr,
        .jpeg = nullptr,
        .isp = nullptr,
    };

    ESP_LOGI(TAG, "Initializing esp_video");
    esp_err_t ret = esp_video_init(&camera_config);
    if (ret == ESP_OK) {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        camera_ctx.video_initialized = true;
    }
    return ret;
}

static void camera_task_finish(esp_err_t result)
{
    SemaphoreHandle_t task_exited = nullptr;
    CameraState final_state = CameraState::CLOSED;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        final_state = (result == ESP_OK) ? CameraState::CLOSED : CameraState::ERROR;
        camera_ctx.last_error = result;
        camera_ctx.camera = nullptr;
        camera_ctx.ppa_handle = nullptr;
        camera_ctx.display_buffer = nullptr;
        camera_ctx.canvas_uses_display_buffer = false;
        camera_ctx.canvas = nullptr;
        camera_ctx.task_handle = nullptr;
        camera_ctx.state = final_state;
        task_exited = camera_ctx.task_exited;
    }

    ESP_LOGI(TAG, "Camera task finished: state=%s error=%s", camera_state_name(final_state),
             esp_err_to_name(result));
    if (task_exited != nullptr) {
        xSemaphoreGive(task_exited);
    }
    vTaskDelete(nullptr);
}

static void app_camera_display(void* arg)
{
    (void)arg;
    esp_err_t result = ESP_OK;
    esp_err_t cleanup_result = ESP_OK;
    cam_t* camera = nullptr;
    ppa_client_handle_t ppa_handle = nullptr;
    uint8_t* display_buffer = nullptr;
    lv_obj_t* canvas = nullptr;
    uint32_t frame_count = 0;
    TickType_t last_stack_log_tick = xTaskGetTickCount();

    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        canvas = camera_ctx.canvas;
    }

    ESP_LOGI(TAG, "Camera task started: priority=%u core=%d stack_hwm=%u",
             static_cast<unsigned>(uxTaskPriorityGet(nullptr)), xPortGetCoreID(),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    do {
        result = ensure_video_initialized();
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "TAB5X_CAMERA_DRIVER_TEST_FAIL init=%s", esp_err_to_name(result));
            break;
        }
        if (camera_stop_requested()) {
            break;
        }

        int camera_fd = app_video_open(CAM_DEV_PATH, EXAMPLE_VIDEO_FMT_RGB565);
        if (camera_fd < 0) {
            result = ESP_FAIL;
            ESP_LOGE(TAG, "TAB5X_CAMERA_DRIVER_TEST_FAIL open=%s", CAM_DEV_PATH);
            break;
        }

        result = new_cam(camera_fd, &camera);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "TAB5X_CAMERA_DRIVER_TEST_FAIL setup=%s", esp_err_to_name(result));
            break;
        }
        {
            std::lock_guard<std::mutex> lock(camera_ctx.mutex);
            camera_ctx.camera = camera;
        }

        if (camera->width != CAMERA_WIDTH || camera->height != CAMERA_HEIGHT ||
            camera->pixel_format != V4L2_PIX_FMT_RGB565) {
            ESP_LOGE(TAG, "Unexpected camera format: width=%" PRIu32 " height=%" PRIu32 " format=0x%08" PRIx32,
                     camera->width, camera->height, camera->pixel_format);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (camera_stop_requested()) {
            break;
        }

        const size_t display_buffer_size = CAMERA_WIDTH * CAMERA_HEIGHT * sizeof(uint16_t);
        display_buffer = static_cast<uint8_t*>(
            heap_caps_calloc(display_buffer_size, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
        if (display_buffer == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate camera display buffer (%u bytes)",
                     static_cast<unsigned>(display_buffer_size));
            result = ESP_ERR_NO_MEM;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(camera_ctx.mutex);
            camera_ctx.display_buffer = display_buffer;
        }

        ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
            .flags = {},
        };
        result = ppa_register_client(&ppa_config, &ppa_handle);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register PPA client: %s", esp_err_to_name(result));
            break;
        }
        {
            std::lock_guard<std::mutex> lock(camera_ctx.mutex);
            camera_ctx.ppa_handle = ppa_handle;
        }

        if (!camera_mark_streaming()) {
            if (!camera_stop_requested()) {
                result = ESP_ERR_INVALID_STATE;
            }
            break;
        }

        while (!camera_stop_requested()) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = MEMORY_TYPE;

            if (ioctl(camera->fd, VIDIOC_DQBUF, &buf) != 0) {
                const int dq_errno = errno;
                if (dq_errno == ETIMEDOUT || dq_errno == EAGAIN || dq_errno == EINTR) {
                    TickType_t now = xTaskGetTickCount();
                    if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(CAMERA_STACK_LOG_MS)) {
                        ESP_LOGI(TAG, "Camera task stack_hwm=%u state=%s frames=%u",
                                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                                 camera_state_name(CameraState::STREAMING), static_cast<unsigned>(frame_count));
                        last_stack_log_tick = now;
                    }
                    continue;
                }
                ESP_LOGE(TAG, "VIDIOC_DQBUF failed: errno=%d (%s)", dq_errno, strerror(dq_errno));
                result = ESP_FAIL;
                break;
            }

            if (buf.index >= camera->buffer_count || camera->buffer[buf.index] == nullptr) {
                ESP_LOGE(TAG, "Driver returned invalid camera buffer index=%u count=%u",
                         static_cast<unsigned>(buf.index), static_cast<unsigned>(camera->buffer_count));
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }

            bool requeue_buffer = true;
            if (!camera_stop_requested()) {
                ppa_srm_oper_config_t srm_config = {
                    .in = {
                        .buffer = camera->buffer[buf.index],
                        .pic_w = CAMERA_WIDTH,
                        .pic_h = CAMERA_HEIGHT,
                        .block_w = CAMERA_WIDTH,
                        .block_h = CAMERA_HEIGHT,
                        .block_offset_x = 0,
                        .block_offset_y = 0,
                        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                        .yuv_range = PPA_COLOR_RANGE_FULL,
                        .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                    },
                    .out = {
                        .buffer = display_buffer,
                        .buffer_size = CAMERA_WIDTH * CAMERA_HEIGHT * sizeof(uint16_t),
                        .pic_w = CAMERA_WIDTH,
                        .pic_h = CAMERA_HEIGHT,
                        .block_offset_x = 0,
                        .block_offset_y = 0,
                        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                        .yuv_range = PPA_COLOR_RANGE_FULL,
                        .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                    },
                    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
                    .scale_x = 1,
                    .scale_y = 1,
                    .mirror_x = true,
                    .mirror_y = false,
                    .rgb_swap = false,
                    .byte_swap = false,
                    .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
                    .alpha_fix_val = 0,
                    .mode = PPA_TRANS_MODE_BLOCKING,
                    .user_data = nullptr,
                };

                result = ppa_do_scale_rotate_mirror(ppa_handle, &srm_config);
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "PPA processing failed: %s", esp_err_to_name(result));
                } else {
                    camera_canvas_attach(canvas, display_buffer);
                }
            }

            if (requeue_buffer && ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF failed: errno=%d (%s)", errno, strerror(errno));
                if (result == ESP_OK) {
                    result = ESP_FAIL;
                }
                break;
            }
            if (result != ESP_OK) {
                break;
            }

            ++frame_count;
            if (frame_count == 1) {
                ESP_LOGI(TAG, "TAB5X_CAMERA_DRIVER_FRAME_OK index=%u bytes=%u",
                         static_cast<unsigned>(buf.index), static_cast<unsigned>(buf.bytesused));
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(CAMERA_STACK_LOG_MS)) {
                ESP_LOGI(TAG, "Camera task stack_hwm=%u state=%s frames=%u",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                         camera_state_name(CameraState::STREAMING), static_cast<unsigned>(frame_count));
                last_stack_log_tick = now;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } while (false);

    if (camera != nullptr) {
        cleanup_result = camera_stream_off(camera);
        if (result == ESP_OK && cleanup_result != ESP_OK) {
            result = cleanup_result;
        }
    }

    if (camera_canvas_needs_detach()) {
        camera_canvas_detach(canvas);
    }

    if (ppa_handle != nullptr) {
        cleanup_result = ppa_unregister_client(ppa_handle);
        if (cleanup_result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unregister PPA client: %s", esp_err_to_name(cleanup_result));
            if (result == ESP_OK) {
                result = cleanup_result;
            }
        }
        ppa_handle = nullptr;
    }

    if (display_buffer != nullptr) {
        heap_caps_free(display_buffer);
        display_buffer = nullptr;
    }

    cleanup_result = delete_cam(&camera);
    if (result == ESP_OK && cleanup_result != ESP_OK) {
        result = cleanup_result;
    }

    ESP_LOGI(TAG, "TAB5X_CAMERA_DRIVER_TEST_%s frames=%u",
             (frame_count > 0 && result == ESP_OK) ? "PASS" : "FAIL", static_cast<unsigned>(frame_count));
    camera_task_finish(result);
}

void HalEsp32::startCameraCapture(lv_obj_t* imgCanvas)
{
    if (imgCanvas == nullptr) {
        ESP_LOGE(TAG, "Camera start rejected: canvas is null");
        return;
    }

    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    if (camera_ctx.task_handle != nullptr || camera_ctx.state == CameraState::STARTING ||
        camera_ctx.state == CameraState::STREAMING || camera_ctx.state == CameraState::STOPPING) {
        ESP_LOGW(TAG, "Camera start ignored in state %s", camera_state_name(camera_ctx.state));
        return;
    }

    if (camera_ctx.task_exited == nullptr) {
        camera_ctx.task_exited = xSemaphoreCreateBinary();
        if (camera_ctx.task_exited == nullptr) {
            camera_ctx.last_error = ESP_ERR_NO_MEM;
            camera_ctx.state = CameraState::ERROR;
            ESP_LOGE(TAG, "Failed to create camera completion semaphore");
            return;
        }
    }
    while (xSemaphoreTake(camera_ctx.task_exited, 0) == pdTRUE) {
    }

    camera_ctx.canvas = imgCanvas;
    camera_ctx.last_error = ESP_OK;
    camera_ctx.canvas_uses_display_buffer = false;
    camera_ctx.camera = nullptr;
    camera_ctx.ppa_handle = nullptr;
    camera_ctx.display_buffer = nullptr;
    camera_ctx.state = CameraState::STARTING;

    TaskHandle_t task_handle = nullptr;
    BaseType_t task_result = xTaskCreatePinnedToCore(app_camera_display, "cam", CAMERA_TASK_STACK_SIZE, nullptr,
                                                     CAMERA_TASK_PRIORITY, &task_handle, 0);
    if (task_result != pdPASS) {
        camera_ctx.canvas = nullptr;
        camera_ctx.last_error = ESP_ERR_NO_MEM;
        camera_ctx.state = CameraState::ERROR;
        ESP_LOGE(TAG, "Failed to create camera task");
        return;
    }
    camera_ctx.task_handle = task_handle;
    ESP_LOGI(TAG, "Camera start requested");
}

void HalEsp32::stopCameraCapture()
{
    TaskHandle_t task_handle = nullptr;
    SemaphoreHandle_t task_exited = nullptr;
    lv_obj_t* canvas = nullptr;

    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.task_handle == nullptr || camera_ctx.state == CameraState::CLOSED ||
            camera_ctx.state == CameraState::ERROR) {
            return;
        }
        camera_ctx.state = CameraState::STOPPING;
        task_handle = camera_ctx.task_handle;
        task_exited = camera_ctx.task_exited;
        canvas = camera_ctx.canvas;
    }

    ESP_LOGI(TAG, "Camera stop requested");
    camera_canvas_detach(canvas);
    xTaskNotifyGive(task_handle);

    if (task_exited == nullptr ||
        xSemaphoreTake(task_exited, pdMS_TO_TICKS(CAMERA_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Camera stop timed out after %u ms", CAMERA_STOP_TIMEOUT_MS);
        return;
    }
    ESP_LOGI(TAG, "Camera stop completed");
}

bool HalEsp32::isCameraCapturing()
{
    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    return camera_ctx.state == CameraState::STARTING || camera_ctx.state == CameraState::STREAMING ||
           camera_ctx.state == CameraState::STOPPING;
}
