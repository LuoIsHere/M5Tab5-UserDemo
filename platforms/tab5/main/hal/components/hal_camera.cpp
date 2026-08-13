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
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/ppa.h"
#include "tab5x_lvgl_diagnostics.h"

#define CAMERA_WIDTH  1280
#define CAMERA_HEIGHT 720

static const char* TAG = "camera";

#define EXAMPLE_VIDEO_BUFFER_COUNT 2
#define CAMERA_DISPLAY_BUFFER_COUNT 2
#define MEMORY_TYPE                V4L2_MEMORY_MMAP
#define CAM_DEV_PATH               ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#define CAMERA_TASK_STACK_SIZE     (8 * 1024)
#define CAMERA_TASK_PRIORITY       5
#define CAMERA_STACK_LOG_MS        5000
#define CAMERA_REFRESH_TIMEOUT_MS   1000
#define CAMERA_INVALID_LOG_INTERVAL 30
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

typedef struct cam {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t bytes_per_line;
    uint32_t size_image;
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
    SemaphoreHandle_t refresh_done = nullptr;
    lv_obj_t* canvas = nullptr;
    lv_display_t* display = nullptr;
    esp_err_t last_error = ESP_OK;
    bool video_initialized = false;
    bool canvas_uses_display_buffer = false;
    bool refresh_callback_registered = false;
    bool refresh_pending = false;
    cam_t* camera = nullptr;
    ppa_client_handle_t ppa_handle = nullptr;
    uint8_t* display_buffers[CAMERA_DISPLAY_BUFFER_COUNT] = {};
    uint8_t* front_buffer = nullptr;
    uint8_t* back_buffer = nullptr;
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

static void camera_display_refresh_ready_cb(lv_event_t* event)
{
    auto* context = static_cast<CameraContext*>(lv_event_get_user_data(event));
    if (context == nullptr || lv_event_get_code(event) != LV_EVENT_REFR_READY) {
        return;
    }

    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->refresh_callback_registered && context->refresh_pending) {
            context->refresh_pending = false;
            refresh_done = context->refresh_done;
        }
    }
    if (refresh_done != nullptr) {
        xSemaphoreGive(refresh_done);
    }
}

static esp_err_t camera_register_refresh_callback(lv_obj_t* canvas)
{
    if (canvas == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_lock(100)) {
        ESP_LOGE(TAG, "Timed out waiting for LVGL lock while registering refresh callback");
        return ESP_ERR_TIMEOUT;
    }

    lv_display_t* display = lv_obj_get_display(canvas);
    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        refresh_done = camera_ctx.refresh_done;
    }
    if (display == nullptr || refresh_done == nullptr) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(refresh_done, 0) == pdTRUE) {
    }
    lv_display_add_event_cb(display, camera_display_refresh_ready_cb, LV_EVENT_REFR_READY, &camera_ctx);
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        camera_ctx.display = display;
        camera_ctx.refresh_callback_registered = true;
        camera_ctx.refresh_pending = false;
    }
    bsp_display_unlock();
    return ESP_OK;
}

static void camera_unregister_refresh_callback()
{
    lv_display_t* display = nullptr;
    bool registered = false;
    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        display = camera_ctx.display;
        registered = camera_ctx.refresh_callback_registered;
        refresh_done = camera_ctx.refresh_done;
    }

    if (registered && display != nullptr && bsp_display_lock(0)) {
        lv_display_remove_event_cb_with_user_data(display, camera_display_refresh_ready_cb, &camera_ctx);
        bsp_display_unlock();
    }

    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        camera_ctx.refresh_callback_registered = false;
        camera_ctx.refresh_pending = false;
        camera_ctx.display = nullptr;
    }
    if (refresh_done != nullptr) {
        xSemaphoreGive(refresh_done);
    }
}

static esp_err_t camera_wait_for_refresh_ready()
{
    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        refresh_done = camera_ctx.refresh_done;
    }
    if (refresh_done == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const TickType_t start_tick = xTaskGetTickCount();
    while (!camera_stop_requested()) {
        if (xSemaphoreTake(refresh_done, pdMS_TO_TICKS(20)) == pdTRUE) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(CAMERA_REFRESH_TIMEOUT_MS)) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

static bool camera_canvas_attach(lv_obj_t* canvas, uint8_t* front_buffer, uint8_t* next_back_buffer)
{
    if (canvas == nullptr || front_buffer == nullptr || next_back_buffer == nullptr) {
        return false;
    }
    if (camera_stop_requested()) {
        return false;
    }
    if (!bsp_display_lock(100)) {
        ESP_LOGW(TAG, "Timed out waiting for LVGL lock while publishing camera frame");
        return false;
    }

    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        refresh_done = camera_ctx.refresh_done;
    }
    if (refresh_done != nullptr) {
        while (xSemaphoreTake(refresh_done, 0) == pdTRUE) {
        }
    }

    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.state == CameraState::STREAMING && camera_ctx.canvas == canvas &&
            camera_ctx.refresh_callback_registered) {
            lv_canvas_set_buffer(canvas, front_buffer, CAMERA_WIDTH, CAMERA_HEIGHT, LV_COLOR_FORMAT_RGB565);
            lv_obj_invalidate(canvas);
            camera_ctx.canvas_uses_display_buffer = true;
            camera_ctx.refresh_pending = true;
            camera_ctx.front_buffer = front_buffer;
            camera_ctx.back_buffer = next_back_buffer;
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

    // timeout_ms == 0 means portMAX_DELAY. When stop is requested from an LVGL
    // callback, the port's recursive mutex lets the callback detach before returning.
    if (!bsp_display_lock(0)) {
        return;
    }
    lv_canvas_set_buffer(canvas, &camera_canvas_placeholder, 1, 1, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(canvas);

    SemaphoreHandle_t refresh_done = nullptr;
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.canvas == canvas) {
            camera_ctx.canvas_uses_display_buffer = false;
            camera_ctx.refresh_pending = false;
            camera_ctx.front_buffer = nullptr;
            camera_ctx.back_buffer = nullptr;
            refresh_done = camera_ctx.refresh_done;
        }
    }
    bsp_display_unlock();

    if (refresh_done != nullptr) {
        xSemaphoreGive(refresh_done);
    }
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
    camera->bytes_per_line = format.fmt.pix.bytesperline;
    camera->size_image = format.fmt.pix.sizeimage;
    ESP_LOGI(TAG,
             "Camera format: width=%" PRIu32 " height=%" PRIu32 " format=0x%08" PRIx32
             " bytesperline=%" PRIu32 " sizeimage=%" PRIu32,
             camera->width, camera->height, camera->pixel_format, camera->bytes_per_line, camera->size_image);

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
        if (mapped == MAP_FAILED) {
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
        .reset_pin = GPIO_NUM_NC,
        .pwdn_pin = GPIO_NUM_NC,
        .dont_init_ldo = true,
    };
    csi_config.sccb_config.i2c_handle = bsp_i2c_get_handle();

    esp_video_init_config_t camera_config = {};
    camera_config.csi = &csi_config;

    ESP_LOGI(TAG, "Initializing esp_video");
    esp_err_t ret = esp_video_init_with_flags(
        &camera_config, ESP_VIDEO_INIT_FLAGS_ISP | ESP_VIDEO_INIT_FLAGS_MIPI_CSI);
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
        for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
            camera_ctx.display_buffers[i] = nullptr;
        }
        camera_ctx.front_buffer = nullptr;
        camera_ctx.back_buffer = nullptr;
        camera_ctx.display = nullptr;
        camera_ctx.refresh_callback_registered = false;
        camera_ctx.refresh_pending = false;
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

/*
 * Camera diagnostic fields:
 * - stack_hwm_bytes: minimum free task stack observed since task creation.
 * - spiram_free_bytes: current total free PSRAM heap.
 * - spiram_largest_block_bytes: current largest contiguous free PSRAM block.
 * - frames, invalid, publish_drop: cumulative frame counters for this capture task.
 */
static void app_camera_display(void* arg)
{
    (void)arg;
    esp_err_t result = ESP_OK;
    esp_err_t cleanup_result = ESP_OK;
    cam_t* camera = nullptr;
    ppa_client_handle_t ppa_handle = nullptr;
    uint8_t* display_buffers[CAMERA_DISPLAY_BUFFER_COUNT] = {};
    uint8_t* front_buffer = nullptr;
    uint8_t* back_buffer = nullptr;
    lv_obj_t* canvas = nullptr;
    uint32_t frame_count = 0;
    uint32_t invalid_frame_count = 0;
    uint32_t publish_drop_count = 0;
    bool refresh_wait_pending = false;
    TickType_t last_stack_log_tick = xTaskGetTickCount();
    const size_t expected_line_size = CAMERA_WIDTH * sizeof(uint16_t);
    const size_t expected_frame_size = expected_line_size * CAMERA_HEIGHT;

    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        canvas = camera_ctx.canvas;
    }

    ESP_LOGI(TAG, "Camera task started: priority=%u core=%d stack_hwm_bytes=%u",
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
        if (camera->bytes_per_line != 0 && camera->bytes_per_line != expected_line_size) {
            ESP_LOGE(TAG, "Unsupported camera stride: bytesperline=%" PRIu32 " expected=%u",
                     camera->bytes_per_line, static_cast<unsigned>(expected_line_size));
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (camera->size_image != 0 && camera->size_image < expected_frame_size) {
            ESP_LOGE(TAG, "Camera sizeimage is too small: sizeimage=%" PRIu32 " expected=%u",
                     camera->size_image, static_cast<unsigned>(expected_frame_size));
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        for (uint32_t i = 0; i < camera->buffer_count; ++i) {
            if (camera->buffer_length[i] < expected_frame_size) {
                ESP_LOGE(TAG, "Camera buffer %" PRIu32 " is too small: length=%u expected=%u", i,
                         static_cast<unsigned>(camera->buffer_length[i]),
                         static_cast<unsigned>(expected_frame_size));
                result = ESP_ERR_INVALID_SIZE;
                break;
            }
        }
        if (result != ESP_OK || camera_stop_requested()) {
            break;
        }

        ESP_LOGI(TAG, "Allocating %u camera display buffers, %u bytes each; spiram_free_bytes=%u spiram_largest_block_bytes=%u",
                 CAMERA_DISPLAY_BUFFER_COUNT, static_cast<unsigned>(expected_frame_size),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
            display_buffers[i] = static_cast<uint8_t*>(
                heap_caps_calloc(expected_frame_size, 1, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM));
            if (display_buffers[i] == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate camera display buffer %" PRIu32 " (%u bytes)", i,
                         static_cast<unsigned>(expected_frame_size));
                result = ESP_ERR_NO_MEM;
                break;
            }
        }
        if (result != ESP_OK) {
            break;
        }
        back_buffer = display_buffers[0];
        {
            std::lock_guard<std::mutex> lock(camera_ctx.mutex);
            for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
                camera_ctx.display_buffers[i] = display_buffers[i];
            }
            camera_ctx.front_buffer = nullptr;
            camera_ctx.back_buffer = back_buffer;
        }

        ppa_client_config_t ppa_config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
            .flags = {},
#endif
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

        result = camera_register_refresh_callback(canvas);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register LVGL refresh callback: %s", esp_err_to_name(result));
            break;
        }

        if (!camera_mark_streaming()) {
            if (!camera_stop_requested()) {
                result = ESP_ERR_INVALID_STATE;
            }
            break;
        }

        while (!camera_stop_requested()) {
            if (refresh_wait_pending) {
                esp_err_t wait_result = camera_wait_for_refresh_ready();
                if (wait_result != ESP_OK) {
                    if (camera_stop_requested()) {
                        break;
                    }
                    ESP_LOGE(TAG, "Timed out waiting for LVGL refresh completion");
                    result = wait_result;
                    break;
                }
                refresh_wait_pending = false;
                if (camera_stop_requested()) {
                    break;
                }
            }

            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = MEMORY_TYPE;

            if (ioctl(camera->fd, VIDIOC_DQBUF, &buf) != 0) {
                const int dq_errno = errno;
                if (dq_errno == ETIMEDOUT || dq_errno == EAGAIN || dq_errno == EINTR) {
                    TickType_t now = xTaskGetTickCount();
                    if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(CAMERA_STACK_LOG_MS)) {
                        ESP_LOGI(TAG, "Camera task stack_hwm_bytes=%u state=%s frames=%u invalid=%u publish_drop=%u",
                                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                                 camera_state_name(CameraState::STREAMING), static_cast<unsigned>(frame_count),
                                 static_cast<unsigned>(invalid_frame_count),
                                 static_cast<unsigned>(publish_drop_count));
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

            const size_t mapped_length = camera->buffer_length[buf.index];
            const size_t dequeued_length = buf.length != 0 ? buf.length : mapped_length;
            const bool driver_error = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
            const bool invalid_length = dequeued_length < expected_frame_size || dequeued_length > mapped_length ||
                                        buf.bytesused < expected_frame_size || buf.bytesused > dequeued_length;
            const bool frame_valid = !driver_error && !invalid_length;
            bool frame_published = false;

            if (!frame_valid) {
                ++invalid_frame_count;
                if (invalid_frame_count == 1 ||
                    (invalid_frame_count % CAMERA_INVALID_LOG_INTERVAL) == 0) {
                    ESP_LOGW(TAG,
                             "Dropping invalid camera frame: count=%u index=%u flags=0x%08x bytesused=%u "
                             "expected=%u dequeued=%u mapped=%u",
                             static_cast<unsigned>(invalid_frame_count), static_cast<unsigned>(buf.index),
                             static_cast<unsigned>(buf.flags), static_cast<unsigned>(buf.bytesused),
                             static_cast<unsigned>(expected_frame_size), static_cast<unsigned>(dequeued_length),
                             static_cast<unsigned>(mapped_length));
                }
            } else if (!camera_stop_requested()) {
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
                        .buffer = back_buffer,
                        .buffer_size = expected_frame_size,
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

                /* AF/AG: before/after the camera's blocking PPA transaction. */
                TAB5X_LVGL_DIAG_HIT(AF);
                result = ppa_do_scale_rotate_mirror(ppa_handle, &srm_config);
                TAB5X_LVGL_DIAG_HIT(AG);
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "PPA processing failed: %s", esp_err_to_name(result));
                } else if (!camera_stop_requested()) {
                    uint8_t* previous_front = front_buffer;
                    uint8_t* next_back = previous_front != nullptr ? previous_front : display_buffers[1];
                    if (camera_canvas_attach(canvas, back_buffer, next_back)) {
                        front_buffer = back_buffer;
                        back_buffer = next_back;
                        refresh_wait_pending = true;
                        frame_published = true;
                    } else if (!camera_stop_requested()) {
                        ++publish_drop_count;
                        if (publish_drop_count == 1 ||
                            (publish_drop_count % CAMERA_INVALID_LOG_INTERVAL) == 0) {
                            ESP_LOGW(TAG, "Dropping camera frame because Canvas publish failed: count=%u",
                                     static_cast<unsigned>(publish_drop_count));
                        }
                    }
                }
            }

            if (ioctl(camera->fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF failed: errno=%d (%s)", errno, strerror(errno));
                if (result == ESP_OK) {
                    result = ESP_FAIL;
                }
                break;
            }
            if (result != ESP_OK) {
                break;
            }
            if (!frame_published) {
                continue;
            }

            ++frame_count;
            if (frame_count == 1) {
                ESP_LOGI(TAG, "TAB5X_CAMERA_DRIVER_FRAME_OK index=%u bytes=%u",
                         static_cast<unsigned>(buf.index), static_cast<unsigned>(buf.bytesused));
            }

            TickType_t now = xTaskGetTickCount();
            if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(CAMERA_STACK_LOG_MS)) {
                ESP_LOGI(TAG, "Camera task stack_hwm_bytes=%u state=%s frames=%u invalid=%u publish_drop=%u",
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                         camera_state_name(CameraState::STREAMING), static_cast<unsigned>(frame_count),
                         static_cast<unsigned>(invalid_frame_count), static_cast<unsigned>(publish_drop_count));
                last_stack_log_tick = now;
            }
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
    camera_unregister_refresh_callback();

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

    for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
        if (display_buffers[i] != nullptr) {
            heap_caps_free(display_buffers[i]);
            display_buffers[i] = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
            camera_ctx.display_buffers[i] = nullptr;
        }
        camera_ctx.front_buffer = nullptr;
        camera_ctx.back_buffer = nullptr;
    }

    cleanup_result = delete_cam(&camera);
    if (result == ESP_OK && cleanup_result != ESP_OK) {
        result = cleanup_result;
    }

    ESP_LOGI(TAG, "TAB5X_CAMERA_DRIVER_TEST_%s frames=%u invalid=%u publish_drop=%u",
             (frame_count > 0 && result == ESP_OK) ? "PASS" : "FAIL", static_cast<unsigned>(frame_count),
             static_cast<unsigned>(invalid_frame_count), static_cast<unsigned>(publish_drop_count));
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
    if (camera_ctx.refresh_done == nullptr) {
        camera_ctx.refresh_done = xSemaphoreCreateBinary();
        if (camera_ctx.refresh_done == nullptr) {
            camera_ctx.last_error = ESP_ERR_NO_MEM;
            camera_ctx.state = CameraState::ERROR;
            ESP_LOGE(TAG, "Failed to create camera refresh semaphore");
            return;
        }
    }
    while (xSemaphoreTake(camera_ctx.task_exited, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(camera_ctx.refresh_done, 0) == pdTRUE) {
    }

    camera_ctx.canvas = imgCanvas;
    camera_ctx.display = nullptr;
    camera_ctx.last_error = ESP_OK;
    camera_ctx.canvas_uses_display_buffer = false;
    camera_ctx.refresh_callback_registered = false;
    camera_ctx.refresh_pending = false;
    camera_ctx.camera = nullptr;
    camera_ctx.ppa_handle = nullptr;
    for (uint32_t i = 0; i < CAMERA_DISPLAY_BUFFER_COUNT; ++i) {
        camera_ctx.display_buffers[i] = nullptr;
    }
    camera_ctx.front_buffer = nullptr;
    camera_ctx.back_buffer = nullptr;
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
    lv_obj_t* canvas = nullptr;

    {
        std::lock_guard<std::mutex> lock(camera_ctx.mutex);
        if (camera_ctx.task_handle == nullptr || camera_ctx.state == CameraState::CLOSED ||
            camera_ctx.state == CameraState::ERROR || camera_ctx.state == CameraState::STOPPING) {
            return;
        }
        camera_ctx.state = CameraState::STOPPING;
        canvas = camera_ctx.canvas;
    }

    ESP_LOGI(TAG, "Camera stop requested");
    camera_canvas_detach(canvas);
}

bool HalEsp32::isCameraCapturing()
{
    std::lock_guard<std::mutex> lock(camera_ctx.mutex);
    return camera_ctx.state == CameraState::STARTING || camera_ctx.state == CameraState::STREAMING ||
           camera_ctx.state == CameraState::STOPPING;
}
