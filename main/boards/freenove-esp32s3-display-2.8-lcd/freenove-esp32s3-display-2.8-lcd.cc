#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <wifi_station.h>
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "adc_battery_monitor.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>

#include "alarm_manager.h"
#include "clock_home.h"
#include "music_ui.h"
#include "zing_music_player.h"
#include "led/single_led.h"
#include "system_reset.h"
#include "esp_lcd_ili9341.h"
#include "board.h"
#include "network_interface.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "display/lvgl_display/lvgl_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define TAG "FreenoveESP32S3Display"

// DEBUG: set to 1 to auto-play a fixed song a few seconds after boot (network
// up) so the Zing music path can be exercised over serial with no user input.
// Leave at 0 in normal builds; music is triggered by the self.music.play tool.
#define ZING_MUSIC_BOOT_TEST 0
#define ZING_MUSIC_BOOT_TEST_QUERY "Lạc trôi Sơn Tùng"

// DEBUG: set to 1 to auto-ring the alarm a few seconds after boot (verifies the
// ring tone/overlay over serial without waiting for a real clock match).
#define ALARM_BOOT_TEST 0

struct YoutubeTestVideo {
    int index;
    const char* title;
    const char* url;
};

static constexpr YoutubeTestVideo kYoutubeTestVideos[] = {
    {1, "Big Buck Bunny sample", "https://www.youtube.com/watch?v=aqz-KE-bpKQ"},
    {2, "Sintel trailer sample", "https://www.youtube.com/watch?v=eRsGyueVLvQ"},
    {3, "Elephants Dream sample", "https://www.youtube.com/watch?v=TLkA0RELQ1g"},
};

static void DelayAtLeastOneTick(int ms) {
    vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(ms)));
}

// Persistent worker task with its stack in PSRAM: internal RAM on this board
// is fully booked (AFE + ~30 task stacks), so plain xTaskCreate can fail and
// internal-stack tasks would starve the wake word engine of boot RAM. The
// stack/TCB allocations are intentionally never freed (board lifetime).
static TaskHandle_t CreatePsramTask(TaskFunction_t entry, const char* name, size_t stack_size,
        void* arg, UBaseType_t priority, BaseType_t core) {
    auto* stack = static_cast<StackType_t*>(heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
    auto* tcb = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        ESP_LOGE(TAG, "No memory for persistent task %s", name);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        return nullptr;
    }
    return xTaskCreateStaticPinnedToCore(entry, name, stack_size, arg, priority, stack, tcb, core);
}

class VideoStreamPlayer {
public:
    explicit VideoStreamPlayer(LcdDisplay* display) : display_(display) {
        // Create the worker tasks once at boot with PSRAM stacks. Creating
        // them per stream start fails when TTS playback or network buffers
        // happen to hold internal RAM at that moment.
        task_ = CreatePsramTask(TaskEntry, "video_stream", 12288, this, 4, 0);
        if (task_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create persistent video reader task");
        }
        render_task_ = CreatePsramTask(RenderTaskEntry, "video_render", 8192, this, 4, 1);
        if (render_task_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create persistent video render task");
        }
    }

    bool Start(const std::string& url, int max_fps) {
        if (task_ == nullptr) {
            return false;
        }
        Stop();
        url_ = url;
        min_frame_interval_ms_ = 1000 / std::max(1, std::min(max_fps, 15));
        raw_rgb332_ = url_.find(".rgb332") != std::string::npos;
        raw_rgb565_ = url_.find(".rgb565") != std::string::npos;
        current_raw_frame_size_ = raw_rgb565_ ? kRgb565FrameSize : kRgb332FrameSize;
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (latest_rgb332_frame_.size() != current_raw_frame_size_) {
                latest_rgb332_frame_.assign(current_raw_frame_size_, 0);
            } else {
                std::fill(latest_rgb332_frame_.begin(), latest_rgb332_frame_.end(), 0);
            }
            latest_frame_sequence_ = 0;
            rendered_frame_sequence_ = 0;
        }
        stop_requested_ = false;

        if ((raw_rgb332_ || raw_rgb565_) && render_task_ != nullptr) {
            xTaskNotifyGive(render_task_);
        }
        xTaskNotifyGive(task_);
        return true;
    }

    void Stop() {
        stop_requested_ = true;
        if (!reader_running_ && !render_running_) {
            return;
        }
        for (int i = 0; i < 40 && (reader_running_ || render_running_); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        display_->SetVideoFrame(nullptr);
    }

    bool IsRunning() const {
        return reader_running_ || render_running_;
    }

private:
    static constexpr size_t kReadChunkSize = 16384;
    static constexpr size_t kMaxJpegSize = 180 * 1024;
    static constexpr size_t kMaxStreamBufferSize = 220 * 1024;
    static constexpr int kRgb332Width = 320;
    static constexpr int kRgb332Height = 180;
    static constexpr size_t kRgb332FrameSize = kRgb332Width * kRgb332Height;
    static constexpr size_t kRgb565FrameSize = kRgb332Width * kRgb332Height * sizeof(uint16_t);

    LcdDisplay* display_;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t render_task_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reader_running_{false};
    std::atomic<bool> render_running_{false};
    std::string url_;
    int min_frame_interval_ms_ = 200;
    bool raw_rgb332_ = false;
    bool raw_rgb565_ = false;
    size_t current_raw_frame_size_ = kRgb332FrameSize;
    std::mutex frame_mutex_;
    std::mutex stats_mutex_;
    std::vector<uint8_t> latest_rgb332_frame_;
    uint32_t latest_frame_sequence_ = 0;
    uint32_t rendered_frame_sequence_ = 0;
    size_t total_bytes_read_ = 0;
    size_t frames_seen_ = 0;
    size_t frames_decoded_ = 0;
    size_t stats_bytes_read_ = 0;
    size_t stats_frames_seen_ = 0;
    size_t stats_frames_decoded_ = 0;
    size_t stats_decode_samples_ = 0;
    size_t last_jpeg_size_ = 0;
    size_t last_image_width_ = 0;
    size_t last_image_height_ = 0;
    int64_t stats_decode_ms_ = 0;
    int64_t stats_draw_ms_ = 0;
    int64_t last_decode_ms_ = 0;
    int64_t last_draw_ms_ = 0;
    int64_t stats_start_ms_ = 0;
    int64_t last_incomplete_rgb332_log_ms_ = 0;
    int consecutive_render_failures_ = 0;

    static void TaskEntry(void* arg) {
        auto* self = static_cast<VideoStreamPlayer*>(arg);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (self->stop_requested_) {
                continue;  // Stopped before the task woke up
            }
            self->reader_running_ = true;
            self->TaskLoop();
            self->reader_running_ = false;
        }
    }

    static void RenderTaskEntry(void* arg) {
        auto* self = static_cast<VideoStreamPlayer*>(arg);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (self->stop_requested_) {
                continue;
            }
            self->render_running_ = true;
            self->RenderLoop();
            self->render_running_ = false;
        }
    }

    void TaskLoop() {
        ESP_LOGI(TAG, "Opening video stream: %s", url_.c_str());
        total_bytes_read_ = 0;
        frames_seen_ = 0;
        frames_decoded_ = 0;
        stats_bytes_read_ = 0;
        stats_frames_seen_ = 0;
        stats_frames_decoded_ = 0;
        stats_decode_samples_ = 0;
        last_jpeg_size_ = 0;
        last_image_width_ = 0;
        last_image_height_ = 0;
        stats_decode_ms_ = 0;
        stats_draw_ms_ = 0;
        last_decode_ms_ = 0;
        last_draw_ms_ = 0;
        stats_start_ms_ = esp_timer_get_time() / 1000;
        last_incomplete_rgb332_log_ms_ = stats_start_ms_;

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(30000);
        http->SetKeepAlive(true);

        int64_t open_start_ms = esp_timer_get_time() / 1000;
        if (!http->Open("GET", url_)) {
            ESP_LOGE(TAG, "Failed to open video stream");
            stop_requested_ = true;
            return;
        }
        ESP_LOGI(TAG, "Video stream HTTP open completed in %d ms",
            static_cast<int>((esp_timer_get_time() / 1000) - open_start_ms));

        int status = http->GetStatusCode();
        ESP_LOGI(TAG, "Video stream HTTP status: %d, body length: %u",
            status, static_cast<unsigned>(http->GetBodyLength()));
        if (status != 200) {
            ESP_LOGE(TAG, "Unexpected video stream status: %d", status);
            http->Close();
            stop_requested_ = true;
            return;
        }

        std::vector<uint8_t> buffer;
        buffer.reserve(std::max(kRgb332FrameSize, kRgb565FrameSize) * 3);
        std::vector<char> chunk(kReadChunkSize);
        int64_t last_frame_ms = 0;
        if (raw_rgb332_ || raw_rgb565_) {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (latest_rgb332_frame_.size() != current_raw_frame_size_) {
                latest_rgb332_frame_.assign(current_raw_frame_size_, 0);
            }
        }

        while (!stop_requested_) {
            int read = http->Read(chunk.data(), chunk.size());
            if (read < 0) {
                ESP_LOGE(TAG, "Video stream read failed");
                break;
            }
            if (read == 0) {
                DelayAtLeastOneTick(5);
                continue;
            }

            total_bytes_read_ += read;
            stats_bytes_read_ += read;
            // Hot path: serial logging at 115200 baud blocks ~9 ms per line,
            // so keep progress logs rare to avoid adding network jitter
            if (total_bytes_read_ <= read || (total_bytes_read_ % (2048 * 1024)) < static_cast<size_t>(read)) {
                ESP_LOGI(TAG, "Video stream read %u bytes total, buffer=%u",
                    static_cast<unsigned>(total_bytes_read_), static_cast<unsigned>(buffer.size()));
            }
            MaybeLogStats();

            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + read);
            if (buffer.size() > kMaxStreamBufferSize) {
                ESP_LOGW(TAG, "Video stream buffer overflow, dropping old data");
                if (raw_rgb332_ || raw_rgb565_) {
                    size_t complete_frames = buffer.size() / current_raw_frame_size_;
                    if (complete_frames > 1) {
                        buffer.erase(buffer.begin(), buffer.begin() + (complete_frames - 1) * current_raw_frame_size_);
                    }
                } else {
                    buffer.erase(buffer.begin(), buffer.begin() + buffer.size() / 2);
                }
            }

            if (raw_rgb332_ || raw_rgb565_) {
                size_t complete_frames = buffer.size() / current_raw_frame_size_;
                if (complete_frames == 0) {
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (buffer.size() > 0 && now_ms - last_incomplete_rgb332_log_ms_ >= 2000) {
                        ESP_LOGW(TAG, "Waiting for full raw frame: buffer=%u/%u bytes, total=%u bytes",
                            static_cast<unsigned>(buffer.size()),
                            static_cast<unsigned>(current_raw_frame_size_),
                            static_cast<unsigned>(total_bytes_read_));
                        last_incomplete_rgb332_log_ms_ = now_ms;
                    }
                    continue;
                }

                frames_seen_ += complete_frames;
                last_jpeg_size_ = current_raw_frame_size_;
                last_image_width_ = kRgb332Width;
                last_image_height_ = kRgb332Height;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_frames_seen_ += complete_frames;
                }

                size_t latest_frame_offset = (complete_frames - 1) * current_raw_frame_size_;
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    memcpy(latest_rgb332_frame_.data(), buffer.data() + latest_frame_offset, current_raw_frame_size_);
                    latest_frame_sequence_++;
                }
                if (complete_frames > 1 && (frames_seen_ <= 3 || frames_seen_ % 150 == 0)) {
                    ESP_LOGI(TAG, "Raw reader dropping %u stale frame(s)",
                        static_cast<unsigned>(complete_frames - 1));
                }

                buffer.erase(buffer.begin(), buffer.begin() + complete_frames * current_raw_frame_size_);
                continue;
            }

            while (!stop_requested_) {
                auto soi = FindMarker(buffer, 0, 0xFF, 0xD8);
                if (soi == buffer.end()) {
                    if (buffer.size() > 2) {
                        buffer.erase(buffer.begin(), buffer.end() - 2);
                    }
                    break;
                }
                if (soi != buffer.begin()) {
                    buffer.erase(buffer.begin(), soi);
                }

                auto eoi = FindMarker(buffer, 2, 0xFF, 0xD9);
                if (eoi == buffer.end()) {
                    if (buffer.size() > kMaxJpegSize) {
                        ESP_LOGW(TAG, "Dropping oversized incomplete JPEG");
                        buffer.clear();
                    }
                    break;
                }

                size_t frame_size = (eoi - buffer.begin()) + 2;
                frames_seen_++;
                stats_frames_seen_++;
                last_jpeg_size_ = frame_size;
                if (frames_seen_ <= 3 || frames_seen_ % 30 == 0) {
                    ESP_LOGI(TAG, "Video stream JPEG frame %u found, size=%u",
                        static_cast<unsigned>(frames_seen_), static_cast<unsigned>(frame_size));
                }
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (now_ms - last_frame_ms >= min_frame_interval_ms_) {
                    DecodeAndDisplay(buffer.data(), frame_size);
                    last_frame_ms = now_ms;
                }
                buffer.erase(buffer.begin(), buffer.begin() + frame_size);
            }
        }

        http->Close();
        stop_requested_ = true;
        ESP_LOGI(TAG, "Video stream stopped, bytes=%u, frames=%u, decoded=%u",
            static_cast<unsigned>(total_bytes_read_), static_cast<unsigned>(frames_seen_),
            static_cast<unsigned>(frames_decoded_));
    }

    void RenderLoop() {
        ESP_LOGI(TAG, "RGB332 render task started");
        int64_t last_render_start_ms = 0;
        std::vector<uint8_t> render_frame;
        render_frame.resize(current_raw_frame_size_);

        while (!stop_requested_) {
            bool has_new_frame = false;
            uint32_t frame_sequence = 0;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                if (stop_requested_) {
                    break;
                }
                if (latest_frame_sequence_ != rendered_frame_sequence_ &&
                    latest_rgb332_frame_.size() >= current_raw_frame_size_) {
                    if (render_frame.size() != current_raw_frame_size_) {
                        render_frame.resize(current_raw_frame_size_);
                    }
                    memcpy(render_frame.data(), latest_rgb332_frame_.data(), current_raw_frame_size_);
                    frame_sequence = latest_frame_sequence_;
                    has_new_frame = true;
                }
            }

            if (!has_new_frame) {
                DelayAtLeastOneTick(5);
                continue;
            }

            // No pacing here: the server already paces frames in real time.
            // Pacing at the same rate as the source beats against network
            // jitter and the 10 ms FreeRTOS tick, causing periodic stutter.
            int64_t render_start_ms = esp_timer_get_time() / 1000;
            if (last_render_start_ms > 0 && render_start_ms - last_render_start_ms > 300) {
                uint32_t latest_sequence = 0;
                uint32_t rendered_sequence = 0;
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    latest_sequence = latest_frame_sequence_;
                    rendered_sequence = rendered_frame_sequence_;
                }
                ESP_LOGW(TAG, "Render gap: %d ms, latest=%u rendered=%u",
                    static_cast<int>(render_start_ms - last_render_start_ms),
                    static_cast<unsigned>(latest_sequence),
                    static_cast<unsigned>(rendered_sequence));
            }
            last_render_start_ms = render_start_ms;
            if (stop_requested_) {
                break;
            }
            bool displayed = raw_rgb565_
                ? DisplayRgb565(render_frame.data(), render_frame.size())
                : DisplayRgb332(render_frame.data(), render_frame.size());
            if (displayed) {
                {
                    std::lock_guard<std::mutex> lock(frame_mutex_);
                    if (rendered_frame_sequence_ < frame_sequence) {
                        rendered_frame_sequence_ = frame_sequence;
                    }
                }
                consecutive_render_failures_ = 0;
            } else {
                consecutive_render_failures_++;
                if (consecutive_render_failures_ >= 3) {
                    ESP_LOGW(TAG, "RGB332 render failed %d times, resetting dirty cache",
                        consecutive_render_failures_);
                    display_->ResetVideoDirtyCache();
                    consecutive_render_failures_ = 0;
                }
            }
            int64_t render_end_ms = esp_timer_get_time() / 1000;
            if (render_end_ms - render_start_ms > 200) {
                ESP_LOGW(TAG, "Slow draw: %d ms",
                    static_cast<int>(render_end_ms - render_start_ms));
                display_->ResetVideoDirtyCache();
            }
        }

        ESP_LOGI(TAG, "RGB332 render task stopped");
    }

    void MaybeLogStats() {
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t elapsed_ms = now_ms - stats_start_ms_;
        if (elapsed_ms < 5000) {
            return;
        }

        std::lock_guard<std::mutex> lock(stats_mutex_);
        elapsed_ms = now_ms - stats_start_ms_;
        if (elapsed_ms < 5000) {
            return;
        }

        float seconds = elapsed_ms / 1000.0f;
        float input_fps = stats_frames_seen_ / seconds;
        float render_fps = stats_frames_decoded_ / seconds;
        float kbps = (stats_bytes_read_ * 8.0f) / seconds / 1000.0f;
        float avg_decode_ms = stats_decode_samples_ > 0 ? stats_decode_ms_ * 1.0f / stats_decode_samples_ : 0.0f;
        float avg_draw_ms = stats_frames_decoded_ > 0 ? stats_draw_ms_ * 1.0f / stats_frames_decoded_ : 0.0f;
        ESP_LOGI(TAG,
            "Video render stats: input=%.2f fps, render=%.2f fps, bitrate=%.1f kbps, last_jpeg=%u, last_image=%ux%u, decode=%.1f/%d ms, draw=%.1f/%d ms, heap=%u",
            input_fps, render_fps, kbps, static_cast<unsigned>(last_jpeg_size_),
            static_cast<unsigned>(last_image_width_), static_cast<unsigned>(last_image_height_),
            avg_decode_ms, static_cast<int>(last_decode_ms_),
            avg_draw_ms, static_cast<int>(last_draw_ms_),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));

        stats_bytes_read_ = 0;
        stats_frames_seen_ = 0;
        stats_frames_decoded_ = 0;
        stats_decode_samples_ = 0;
        stats_decode_ms_ = 0;
        stats_draw_ms_ = 0;
        stats_start_ms_ = now_ms;
    }

    static std::vector<uint8_t>::iterator FindMarker(std::vector<uint8_t>& buffer, size_t offset, uint8_t a, uint8_t b) {
        if (buffer.size() < offset + 2) {
            return buffer.end();
        }
        for (auto it = buffer.begin() + offset; it + 1 != buffer.end(); ++it) {
            if (*it == a && *(it + 1) == b) {
                return it;
            }
        }
        return buffer.end();
    }

    void DecodeAndDisplay(const uint8_t* jpeg_data, size_t jpeg_len) {
        uint8_t* image_data = nullptr;
        size_t image_len = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;

        int64_t decode_start_ms = esp_timer_get_time() / 1000;
        esp_err_t ret = jpeg_to_image(jpeg_data, jpeg_len, &image_data, &image_len, &width, &height, &stride);
        last_decode_ms_ = (esp_timer_get_time() / 1000) - decode_start_ms;
        stats_decode_ms_ += last_decode_ms_;
        stats_decode_samples_++;
        if (ret != ESP_OK || image_data == nullptr) {
            ESP_LOGW(TAG, "Failed to decode JPEG frame: %s", esp_err_to_name(ret));
            return;
        }

        last_image_width_ = width;
        last_image_height_ = height;

        bool displayed = false;
        int64_t draw_start_ms = esp_timer_get_time() / 1000;
        if (width == 320 && height == 240 && stride == width * 2) {
            displayed = display_->DrawRgb565FrameDirectFast(image_data, static_cast<int>(width),
                static_cast<int>(height), static_cast<int>(stride));
            heap_caps_free(image_data);
        } else {
            auto image = std::make_unique<LvglAllocatedImage>(
                image_data, image_len, static_cast<int>(width), static_cast<int>(height),
                static_cast<int>(stride), LV_COLOR_FORMAT_RGB565);
            display_->SetVideoFrame(std::move(image));
            displayed = true;
        }
        last_draw_ms_ = (esp_timer_get_time() / 1000) - draw_start_ms;
        if (displayed) {
            frames_decoded_++;
            stats_frames_decoded_++;
            stats_draw_ms_ += last_draw_ms_;
            if (frames_decoded_ <= 3 || frames_decoded_ % 30 == 0) {
                ESP_LOGI(TAG, "Video stream displayed frame %u: %ux%u stride=%u len=%u",
                    static_cast<unsigned>(frames_decoded_), static_cast<unsigned>(width),
                    static_cast<unsigned>(height), static_cast<unsigned>(stride),
                    static_cast<unsigned>(image_len));
            }
        }
    }

    bool DisplayRgb332(const uint8_t* frame_data, size_t frame_len) {
        if (frame_data == nullptr || frame_len < kRgb332FrameSize) {
            return false;
        }

        bool displayed = false;
        int64_t draw_start_ms = esp_timer_get_time() / 1000;
        displayed = display_->DrawRgb332FrameDirectFast(frame_data, kRgb332Width, kRgb332Height, kRgb332Width);
        last_draw_ms_ = (esp_timer_get_time() / 1000) - draw_start_ms;
        last_decode_ms_ = 0;
        if (displayed) {
            frames_decoded_++;
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_frames_decoded_++;
                stats_draw_ms_ += last_draw_ms_;
            }
            if (frames_decoded_ <= 3 || frames_decoded_ % 300 == 0) {
                ESP_LOGI(TAG, "Video stream displayed RGB332 frame %u: %ux%u len=%u",
                    static_cast<unsigned>(frames_decoded_), kRgb332Width, kRgb332Height,
                    static_cast<unsigned>(frame_len));
            }
        }
        return displayed;
    }

    bool DisplayRgb565(const uint8_t* frame_data, size_t frame_len) {
        if (frame_data == nullptr || frame_len < kRgb565FrameSize) {
            return false;
        }

        bool displayed = false;
        int64_t draw_start_ms = esp_timer_get_time() / 1000;
        displayed = display_->DrawRgb565FrameDirectFast(frame_data, kRgb332Width, kRgb332Height,
            kRgb332Width * sizeof(uint16_t));
        last_draw_ms_ = (esp_timer_get_time() / 1000) - draw_start_ms;
        last_decode_ms_ = 0;
        if (displayed) {
            frames_decoded_++;
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_frames_decoded_++;
                stats_draw_ms_ += last_draw_ms_;
            }
            if (frames_decoded_ <= 3 || frames_decoded_ % 300 == 0) {
                ESP_LOGI(TAG, "Video stream displayed RGB565 frame %u: %ux%u len=%u",
                    static_cast<unsigned>(frames_decoded_), kRgb332Width, kRgb332Height,
                    static_cast<unsigned>(frame_len));
            }
        }
        return displayed;
    }
};

class PcmAudioStreamPlayer {
public:
    explicit PcmAudioStreamPlayer(AudioService* audio_service) : audio_service_(audio_service) {
        // Create the worker task once at boot with a PSRAM stack. Spawning a
        // task per toggle fails while video streaming has internal RAM
        // exhausted.
        task_ = CreatePsramTask(TaskEntry, "pcm_audio_stream", 8192, this, 4, 0);
        if (task_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create persistent PCM audio task");
        }
    }

    bool Start(const std::string& url) {
        if (task_ == nullptr) {
            return false;
        }
        Stop();
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            url_ = url;
        }
        stop_requested_ = false;
        xTaskNotifyGive(task_);
        return true;
    }

    void Stop() {
        stop_requested_ = true;
        for (int i = 0; i < 40 && streaming_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    bool IsRunning() const {
        return streaming_;
    }

private:
    static constexpr size_t kReadChunkSize = 2048;

    AudioService* audio_service_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> streaming_{false};
    std::mutex url_mutex_;
    std::string url_;

    static void TaskEntry(void* arg) {
        auto* self = static_cast<PcmAudioStreamPlayer*>(arg);
        while (true) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (self->stop_requested_) {
                continue;  // Stopped before the task woke up
            }
            self->streaming_ = true;
            self->TaskLoop();
            self->streaming_ = false;
        }
    }

    void TaskLoop() {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            url = url_;
        }
        ESP_LOGI(TAG, "Opening PCM audio stream: %s", url.c_str());

        // Internal RAM is at its tightest right when the video pipeline spins
        // up, so the first HTTP open can fail transiently — retry with backoff
        std::unique_ptr<Http> http;
        int status = -1;
        for (int attempt = 0; attempt < 4 && !stop_requested_; ++attempt) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "PCM audio open retry %d, last status %d", attempt, status);
                DelayAtLeastOneTick(1000 * attempt);
                if (stop_requested_) {
                    return;
                }
            }
            http = Board::GetInstance().GetNetwork()->CreateHttp(4);
            http->SetTimeout(15000);
            http->SetKeepAlive(true);
            if (!http->Open("GET", url)) {
                ESP_LOGE(TAG, "Failed to open PCM audio stream");
                http.reset();
                continue;
            }
            status = http->GetStatusCode();
            ESP_LOGI(TAG, "PCM audio HTTP status: %d", status);
            if (status == 200) {
                break;
            }
            http->Close();
            http.reset();
        }
        if (!http || status != 200) {
            return;
        }

        std::vector<char> chunk(kReadChunkSize);
        std::vector<int16_t> samples;
        samples.reserve(kReadChunkSize / 2);
        uint8_t pending = 0;
        bool has_pending = false;
        size_t total_bytes = 0;

        while (!stop_requested_) {
            int read = http->Read(chunk.data(), chunk.size());
            if (read < 0) {
                ESP_LOGE(TAG, "PCM audio stream read failed");
                break;
            }
            if (read == 0) {
                DelayAtLeastOneTick(5);
                continue;
            }

            total_bytes += read;
            samples.clear();
            int offset = 0;
            if (has_pending) {
                uint16_t value = static_cast<uint16_t>(pending) |
                    (static_cast<uint16_t>(static_cast<uint8_t>(chunk[0])) << 8);
                samples.push_back(static_cast<int16_t>(value));
                offset = 1;
                has_pending = false;
            }

            for (int i = offset; i + 1 < read; i += 2) {
                uint16_t value = static_cast<uint16_t>(static_cast<uint8_t>(chunk[i])) |
                    (static_cast<uint16_t>(static_cast<uint8_t>(chunk[i + 1])) << 8);
                samples.push_back(static_cast<int16_t>(value));
            }

            if (((read - offset) & 1) != 0) {
                pending = static_cast<uint8_t>(chunk[read - 1]);
                has_pending = true;
            }

            if (!samples.empty()) {
                // Write straight to the codec: the AudioService is stopped in
                // exclusive video mode (its opus task stack is reclaimed for
                // the video pipeline), and OutputData() blocking on I2S DMA
                // paces this loop to real time naturally.
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec != nullptr) {
                    if (!codec->output_enabled()) {
                        codec->EnableOutput(true);
                    }
                    codec->OutputData(samples);
                }
                samples.clear();
                samples.reserve(kReadChunkSize / 2);
            }

            if (total_bytes <= static_cast<size_t>(read) || (total_bytes % (48 * 1024)) < static_cast<size_t>(read)) {
                ESP_LOGI(TAG, "PCM audio stream read %u bytes total", static_cast<unsigned>(total_bytes));
            }
        }

        http->Close();
        ESP_LOGI(TAG, "PCM audio stream stopped, bytes=%u", static_cast<unsigned>(total_bytes));
    }
};

class YoutubeStoryboardPlayer {
public:
    explicit YoutubeStoryboardPlayer(LcdDisplay* display) : display_(display) {}

    bool Start(const std::string& url_or_id, int level, int frame_delay_ms) {
        Stop();
        video_id_ = ExtractVideoId(url_or_id);
        if (video_id_.empty()) {
            ESP_LOGE(TAG, "Invalid YouTube URL or video id: %s", url_or_id.c_str());
            return false;
        }

        level_ = std::max(1, std::min(level, 5));
        frame_delay_ms_ = std::max(100, std::min(frame_delay_ms, 5000));
        stop_requested_ = false;

        auto ret = xTaskCreatePinnedToCore(TaskEntry, "yt_storyboard", 16384, this, 4, &task_, 1);
        if (ret != pdPASS) {
            task_ = nullptr;
            return false;
        }
        return true;
    }

    void Stop() {
        if (task_ == nullptr) {
            return;
        }

        stop_requested_ = true;
        for (int i = 0; i < 60 && task_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        display_->SetVideoFrame(nullptr);
    }

    bool IsRunning() const {
        return task_ != nullptr;
    }

private:
    struct StoryboardLevel {
        int level = 0;
        int display_level = 1;
        int width = 0;
        int height = 0;
        int count = 0;
        int cols = 0;
        int rows = 0;
        std::string name;
        std::string signature;
    };

    struct PrefetchJob {
        YoutubeStoryboardPlayer* player = nullptr;
        std::string url;
        std::vector<uint8_t> jpeg;
        bool success = false;
        bool done = false;
        TaskHandle_t task = nullptr;
    };

    LcdDisplay* display_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::string video_id_;
    int level_ = 2;
    int frame_delay_ms_ = 250;

    static void TaskEntry(void* arg) {
        auto* self = static_cast<YoutubeStoryboardPlayer*>(arg);
        self->TaskLoop();
        self->task_ = nullptr;
        vTaskDelete(nullptr);
    }

    static void PrefetchTaskEntry(void* arg) {
        auto* job = static_cast<PrefetchJob*>(arg);
        job->success = job->player->DownloadBinary(job->url, job->jpeg);
        job->done = true;
        job->task = nullptr;
        vTaskDelete(nullptr);
    }

    void StartPrefetch(PrefetchJob& job, const std::string& url) {
        job.player = this;
        job.url = url;
        job.jpeg.clear();
        job.success = false;
        job.done = false;
        job.task = nullptr;

        auto ret = xTaskCreatePinnedToCore(PrefetchTaskEntry, "yt_sb_prefetch", 8192, &job, 3, &job.task, 0);
        if (ret != pdPASS) {
            ESP_LOGW(TAG, "Failed to start storyboard prefetch task");
            job.done = true;
        }
    }

    void WaitPrefetch(PrefetchJob& job) {
        while (job.task != nullptr && !job.done && !stop_requested_) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        for (int i = 0; i < 50 && job.task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void TaskLoop() {
        ESP_LOGI(TAG, "Opening YouTube storyboard for video id: %s", video_id_.c_str());

        std::string spec;
        if (!FetchStoryboardSpec(spec)) {
            ESP_LOGE(TAG, "Failed to fetch YouTube storyboard spec");
            return;
        }
        if (spec.empty()) {
            ESP_LOGE(TAG, "Storyboard spec not found");
            return;
        }

        std::string template_url;
        std::vector<StoryboardLevel> levels;
        if (!ParseStoryboardSpec(spec, template_url, levels) || levels.empty()) {
            ESP_LOGE(TAG, "Failed to parse storyboard spec");
            return;
        }

        const StoryboardLevel* selected = SelectLevel(levels, level_);
        if (selected == nullptr || selected->cols <= 0 || selected->rows <= 0 || selected->count <= 0) {
            ESP_LOGE(TAG, "No usable storyboard level");
            return;
        }

        int tiles_per_sheet = selected->cols * selected->rows;
        int sheet_count = (selected->count + tiles_per_sheet - 1) / tiles_per_sheet;
        ESP_LOGI(TAG, "Storyboard L%d: %dx%d, count=%d, grid=%dx%d, sheets=%d",
            selected->level, selected->width, selected->height, selected->count,
            selected->cols, selected->rows, sheet_count);

        std::vector<uint8_t> jpeg;
        PrefetchJob prefetch;
        bool has_prefetch = false;

        for (int sheet = 0; sheet < sheet_count && !stop_requested_; ++sheet) {
            std::string sheet_url = BuildSheetUrl(template_url, *selected, sheet);

            if (has_prefetch) {
                WaitPrefetch(prefetch);
                if (!prefetch.success) {
                    ESP_LOGW(TAG, "Failed to prefetch storyboard sheet %d", sheet);
                    has_prefetch = false;
                    continue;
                }
                jpeg = std::move(prefetch.jpeg);
                has_prefetch = false;
            } else {
                ESP_LOGI(TAG, "Download storyboard sheet: %s", sheet_url.c_str());
                if (!DownloadBinary(sheet_url, jpeg)) {
                    ESP_LOGW(TAG, "Failed to download storyboard sheet %d", sheet);
                    continue;
                }
            }

            uint8_t* sheet_data = nullptr;
            size_t sheet_len = 0;
            size_t sheet_w = 0;
            size_t sheet_h = 0;
            size_t stride = 0;
            esp_err_t ret = jpeg_to_image(jpeg.data(), jpeg.size(), &sheet_data, &sheet_len, &sheet_w, &sheet_h, &stride);
            jpeg.clear();
            jpeg.shrink_to_fit();

            if (ret != ESP_OK || sheet_data == nullptr) {
                ESP_LOGW(TAG, "Failed to decode storyboard sheet: %s", esp_err_to_name(ret));
                continue;
            }

            if (sheet + 1 < sheet_count && !stop_requested_) {
                std::string next_url = BuildSheetUrl(template_url, *selected, sheet + 1);
                ESP_LOGI(TAG, "Prefetch storyboard sheet %d: %s", sheet + 1, next_url.c_str());
                StartPrefetch(prefetch, next_url);
                has_prefetch = true;
            }

            int frames_in_sheet = std::min(tiles_per_sheet, selected->count - sheet * tiles_per_sheet);
            for (int i = 0; i < frames_in_sheet && !stop_requested_; ++i) {
                int tile_x = (i % selected->cols) * selected->width;
                int tile_y = (i / selected->cols) * selected->height;
                DisplayTile(sheet_data, stride, sheet_w, sheet_h, tile_x, tile_y, selected->width, selected->height);
                vTaskDelay(pdMS_TO_TICKS(frame_delay_ms_));
            }

            heap_caps_free(sheet_data);
        }

        if (has_prefetch) {
            WaitPrefetch(prefetch);
        }

        display_->SetVideoFrame(nullptr);
        ESP_LOGI(TAG, "YouTube storyboard stopped");
    }

    bool FetchStoryboardSpec(std::string& spec) {
        static const char* kInnertubeKey = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
        std::string url = std::string("https://www.youtube.com/youtubei/v1/player?key=") + kInnertubeKey;
        std::string body = "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20240101.00.00\",\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\""
            + video_id_ + "\",\"contentCheckOk\":true,\"racyCheckOk\":true}";

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(10000);
        http->SetHeader("User-Agent", "Mozilla/5.0");
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));

        if (!http->Open("POST", url)) {
            return false;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGW(TAG, "HTTP status %d for %s", http->GetStatusCode(), url.c_str());
            http->Close();
            return false;
        }

        std::string response;
        response.reserve(64 * 1024);
        char buffer[2048];
        while (!stop_requested_) {
            int ret = http->Read(buffer, sizeof(buffer));
            if (ret < 0) {
                http->Close();
                return false;
            }
            if (ret == 0) {
                break;
            }
            response.append(buffer, ret);
            if (response.size() > 384 * 1024) {
                ESP_LOGW(TAG, "Innertube response too large");
                http->Close();
                return false;
            }
        }
        http->Close();

        spec = ExtractStoryboardSpec(response);
        if (!spec.empty()) {
            ESP_LOGI(TAG, "Storyboard spec found from Innertube");
            return true;
        }

        ESP_LOGW(TAG, "Innertube response has no storyboard spec, falling back to watch HTML");
        return FetchStoryboardSpecFromWatchHtml(spec);
    }

    bool FetchStoryboardSpecFromWatchHtml(std::string& spec) {
        std::string url = "https://www.youtube.com/watch?v=" + video_id_ + "&hl=en&bpctr=9999999999";
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(10000);
        http->SetHeader("User-Agent", "Mozilla/5.0");
        http->SetHeader("Accept-Language", "en-US,en;q=0.9");

        if (!http->Open("GET", url)) {
            return false;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGW(TAG, "HTTP status %d for %s", http->GetStatusCode(), url.c_str());
            http->Close();
            return false;
        }

        static const char* kMarker = "\"storyboards\":{\"playerStoryboardSpecRenderer\":{\"spec\"";
        std::string window;
        window.reserve(64 * 1024);
        bool marker_seen = false;
        size_t total_read = 0;
        char buffer[2048];

        while (!stop_requested_) {
            int ret = http->Read(buffer, sizeof(buffer));
            if (ret < 0) {
                http->Close();
                return false;
            }
            if (ret == 0) {
                break;
            }

            total_read += ret;
            if (total_read > 1536 * 1024) {
                ESP_LOGW(TAG, "Watch HTML scan exceeded limit");
                break;
            }

            window.append(buffer, ret);
            if (!marker_seen) {
                auto marker = window.find(kMarker);
                if (marker != std::string::npos) {
                    window.erase(0, marker);
                    marker_seen = true;
                } else if (window.size() > 4096) {
                    window.erase(0, window.size() - 4096);
                }
                continue;
            }

            spec = ExtractStoryboardSpec(window);
            if (!spec.empty()) {
                ESP_LOGI(TAG, "Storyboard spec found from watch HTML after %u bytes", static_cast<unsigned>(total_read));
                http->Close();
                return true;
            }

            if (window.size() > 64 * 1024) {
                ESP_LOGW(TAG, "Storyboard marker found but spec parse exceeded window");
                break;
            }
        }

        http->Close();
        return false;
    }

    bool DownloadBinary(const std::string& url, std::vector<uint8_t>& out) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(10000);
        http->SetHeader("User-Agent", "Mozilla/5.0");

        if (!http->Open("GET", url)) {
            return false;
        }
        if (http->GetStatusCode() != 200) {
            ESP_LOGW(TAG, "HTTP status %d for %s", http->GetStatusCode(), url.c_str());
            http->Close();
            return false;
        }

        size_t content_length = http->GetBodyLength();
        if (content_length > 0 && content_length < 512 * 1024) {
            out.reserve(content_length);
        }

        char buffer[2048];
        while (!stop_requested_) {
            int ret = http->Read(buffer, sizeof(buffer));
            if (ret < 0) {
                http->Close();
                return false;
            }
            if (ret == 0) {
                break;
            }
            out.insert(out.end(), buffer, buffer + ret);
            if (out.size() > 512 * 1024) {
                ESP_LOGW(TAG, "Storyboard JPEG too large");
                http->Close();
                return false;
            }
        }

        http->Close();
        return !out.empty();
    }

    static std::string ExtractVideoId(const std::string& url_or_id) {
        if (url_or_id.size() == 11 && IsVideoId(url_or_id)) {
            return url_or_id;
        }

        const char* keys[] = {"v=", "youtu.be/", "/shorts/", "/embed/"};
        for (auto key : keys) {
            auto pos = url_or_id.find(key);
            if (pos == std::string::npos) {
                continue;
            }
            pos += std::strlen(key);
            if (pos + 11 <= url_or_id.size()) {
                std::string id = url_or_id.substr(pos, 11);
                if (IsVideoId(id)) {
                    return id;
                }
            }
        }

        return "";
    }

    static bool IsVideoId(const std::string& text) {
        if (text.size() != 11) {
            return false;
        }
        for (char c : text) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
                return false;
            }
        }
        return true;
    }

    static std::string ExtractStoryboardSpec(const std::string& html) {
        auto marker = html.find("\"playerStoryboardSpecRenderer\"");
        if (marker == std::string::npos) {
            marker = html.find("playerStoryboardSpecRenderer");
        }
        if (marker == std::string::npos) {
            return "";
        }

        auto spec_key = html.find("\"spec\"", marker);
        if (spec_key == std::string::npos) {
            return "";
        }

        auto colon = html.find(':', spec_key);
        if (colon == std::string::npos) {
            return "";
        }

        auto quote = html.find('"', colon + 1);
        if (quote == std::string::npos) {
            return "";
        }

        std::string escaped;
        bool slash = false;
        for (size_t i = quote + 1; i < html.size(); ++i) {
            char c = html[i];
            if (slash) {
                escaped.push_back('\\');
                escaped.push_back(c);
                slash = false;
                continue;
            }
            if (c == '\\') {
                slash = true;
                continue;
            }
            if (c == '"') {
                break;
            }
            escaped.push_back(c);
        }

        return JsonUnescape(escaped);
    }

    static std::string JsonUnescape(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c != '\\' || i + 1 >= text.size()) {
                out.push_back(c);
                continue;
            }

            char n = text[++i];
            switch (n) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (i + 4 < text.size()) {
                        int h1 = HexValue(text[i + 1]);
                        int h2 = HexValue(text[i + 2]);
                        int h3 = HexValue(text[i + 3]);
                        int h4 = HexValue(text[i + 4]);
                        if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                            int value = (h1 << 12) | (h2 << 8) | (h3 << 4) | h4;
                            if (value > 0x7F) {
                                out.push_back('u');
                                break;
                            }
                            out.push_back(static_cast<char>(value));
                            i += 4;
                            break;
                        }
                    }
                    out.push_back('u');
                    break;
                default: out.push_back(n); break;
            }
        }
        return out;
    }

    static int HexValue(char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    static bool ParseStoryboardSpec(const std::string& spec, std::string& template_url, std::vector<StoryboardLevel>& levels) {
        auto parts = Split(spec, '|');
        if (parts.size() < 2) {
            return false;
        }

        template_url = parts[0];
        for (size_t i = 1; i < parts.size(); ++i) {
            auto fields = Split(parts[i], '#');
            if (fields.size() < 7) {
                continue;
            }

            StoryboardLevel level;
            level.level = static_cast<int>(i) - 1;
            level.display_level = static_cast<int>(i);
            level.width = ToInt(fields[0]);
            level.height = ToInt(fields[1]);
            level.count = ToInt(fields[2]);
            level.cols = ToInt(fields[3]);
            level.rows = ToInt(fields[4]);
            level.name = fields[6];
            level.signature = fields[7];

            if (level.width > 0 && level.height > 0 && level.count > 0 && level.cols > 0 && level.rows > 0 && !level.name.empty()) {
                levels.push_back(level);
            }
        }
        return !template_url.empty() && !levels.empty();
    }

    static const StoryboardLevel* SelectLevel(const std::vector<StoryboardLevel>& levels, int requested) {
        const StoryboardLevel* best = &levels.front();
        for (const auto& level : levels) {
            if (level.display_level <= requested) {
                best = &level;
            }
        }
        return best;
    }

    static std::string BuildSheetUrl(std::string template_url, const StoryboardLevel& level, int sheet) {
        ReplaceAll(template_url, "$L", std::to_string(level.level));
        ReplaceAll(template_url, "$N", level.name);
        ReplaceAll(template_url, "$M", std::to_string(sheet));
        if (!level.signature.empty()) {
            template_url += template_url.find('?') == std::string::npos ? "?sigh=" : "&sigh=";
            template_url += level.signature;
        }
        return template_url;
    }

    void DisplayTile(const uint8_t* sheet_data, size_t sheet_stride, size_t sheet_w, size_t sheet_h,
            int tile_x, int tile_y, int tile_w, int tile_h) {
        if (tile_x < 0 || tile_y < 0 || tile_x + tile_w > static_cast<int>(sheet_w) || tile_y + tile_h > static_cast<int>(sheet_h)) {
            return;
        }

        int target_w = DISPLAY_WIDTH;
        int target_h = tile_h * target_w / tile_w;
        if (target_h > DISPLAY_HEIGHT) {
            target_h = DISPLAY_HEIGHT;
            target_w = tile_w * target_h / tile_h;
        }

        size_t target_stride = target_w * 2;
        size_t target_len = target_stride * target_h;
        uint8_t* target = static_cast<uint8_t*>(heap_caps_malloc(target_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (target == nullptr) {
            target = static_cast<uint8_t*>(heap_caps_malloc(target_len, MALLOC_CAP_8BIT));
        }
        if (target == nullptr) {
            ESP_LOGW(TAG, "No memory for scaled storyboard tile");
            return;
        }

        for (int y = 0; y < target_h; ++y) {
            int src_y = tile_y + y * tile_h / target_h;
            const uint16_t* src_line = reinterpret_cast<const uint16_t*>(sheet_data + src_y * sheet_stride + tile_x * 2);
            uint16_t* dst_line = reinterpret_cast<uint16_t*>(target + y * target_stride);
            for (int x = 0; x < target_w; ++x) {
                int src_x = x * tile_w / target_w;
                dst_line[x] = src_line[src_x];
            }
        }

        auto image = std::make_unique<LvglAllocatedImage>(
            target, target_len, target_w, target_h, static_cast<int>(target_stride), LV_COLOR_FORMAT_RGB565);
        display_->SetVideoFrame(std::move(image));
    }

    static std::vector<std::string> Split(const std::string& text, char delimiter) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= text.size()) {
            auto end = text.find(delimiter, start);
            if (end == std::string::npos) {
                out.push_back(text.substr(start));
                break;
            }
            out.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    static int ToInt(const std::string& text) {
        return std::atoi(text.c_str());
    }

    static void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string UrlEncode(const std::string& text) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0F]);
            }
        }
        return out;
    }
};

class TouchDriver {
public:
    TouchDriver() : dev_(nullptr) {}

    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .device_address = addr,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool Read(bool &touched, uint16_t &x, uint16_t &y) {
        touched = false;
        x = y = 0;
        if (!dev_) return false;

        uint8_t reg = 0x02;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) return false;

        uint8_t points = buf[0] & 0x0F;
        if (points == 0) return true;

        touched = true;
        x = ((buf[1] & 0x0F) << 8) | buf[2];
        y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }

private:
    i2c_master_dev_handle_t dev_;
};

class FreenoveESP32S3Display : public WifiBoard {
private:
    static constexpr const char* kPcStreamUrl = "http://192.168.0.174:8088/stream.rgb332";
    static constexpr const char* kPcAudioUrl = "http://192.168.0.174:8088/audio.pcm";
    static constexpr int kPcDiscoveryPort = 8089;
    static constexpr const char* kPcDiscoveryRequest = "XIAOZHI_STREAM_DISCOVER";
    static constexpr const char* kPcDiscoveryResponse = "XIAOZHI_STREAM_SERVER";
    static constexpr int kPcStreamDefaultFps = 12;
    static constexpr int kStreamButtonW = 150;
    static constexpr int kStreamButtonH = 52;
    static constexpr int kStreamButtonMargin = 6;

    Button boot_button_;
    LcdDisplay *display_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    TouchDriver touch_;
    AdcBatteryMonitor* adc_battery_monitor_;
    std::unique_ptr<VideoStreamPlayer> video_stream_player_;
    std::unique_ptr<PcmAudioStreamPlayer> audio_stream_player_;
    std::unique_ptr<YoutubeStoryboardPlayer> youtube_storyboard_player_;
    lv_obj_t* pc_stream_button_ = nullptr;
    lv_obj_t* chat_input_bar_ = nullptr;
    lv_obj_t* chat_input_label_ = nullptr;
    lv_obj_t* keyboard_overlay_ = nullptr;
    lv_obj_t* keyboard_textarea_ = nullptr;
    lv_obj_t* keyboard_key_feedback_ = nullptr;
    lv_timer_t* keyboard_key_feedback_timer_ = nullptr;
    std::string keyboard_text_;
    std::string pc_stream_audio_url_;
    int saved_volume_ = 70;
    std::unique_ptr<ClockHome> clock_home_;
    std::unique_ptr<ZingMusicPlayer> music_player_;
    std::unique_ptr<MusicUi> music_ui_;
    std::string music_ui_text_;  // last title|artists pushed to music_ui_
    std::unique_ptr<AlarmManager> alarm_manager_;
    bool alarm_http_registered_ = false;
    bool home_boot_shown_ = false;
#if ZING_MUSIC_BOOT_TEST
    bool boot_music_test_done_ = false;
    int64_t boot_music_first_idle_ms_ = 0;
#endif
#if ALARM_BOOT_TEST
    bool alarm_test_done_ = false;
    int64_t alarm_test_first_idle_ms_ = 0;
#endif
    std::atomic<bool> go_home_pending_{false};
    int64_t go_home_request_ms_ = 0;

    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_8, 200000, 200000, GPIO_NUM_NC);
    }

    static void TouchTask(void *arg) {
        auto *self = static_cast<FreenoveESP32S3Display*>(arg);
        auto &app = Application::GetInstance();

        uint32_t last_tap = 0;
        uint32_t down_start = 0;
        uint16_t last_x = 0;
        uint16_t last_y = 0;
        bool down = false;

        while (true) {
            self->EnsureChatInputBar();
            self->PollHomeState();

            bool t;
            uint16_t x, y;
            self->touch_.Read(t, x, y);

            uint32_t now = esp_timer_get_time() / 1000;

            if (t) {
                last_x = x;
                last_y = y;
                if (!down) {
                    down = true;
                    down_start = now;
                }
            }

            if (!t && down) {
                down = false;

                uint32_t press = now - down_start;
                ESP_LOGI(TAG, "Touch released: raw=(%u,%u), press=%u ms",
                    static_cast<unsigned>(last_x), static_cast<unsigned>(last_y),
                    static_cast<unsigned>(press));

                // While video plays there are exactly two gestures (touch
                // rotation is unreliable, so press duration is used instead
                // of coordinates): short tap toggles stream audio, holding
                // (>=800 ms) exits playback. Nothing else, not even the WiFi
                // config long-press.
                // A tap while the alarm is ringing snoozes it.
                if (self->alarm_manager_ && self->alarm_manager_->IsRinging()) {
                    self->alarm_manager_->Snooze();
                    last_tap = 0;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }

                // A tap during music playback stops it and returns to normal.
                if (self->music_player_ && self->music_player_->IsPlaying()) {
                    self->music_player_->RequestStop();
                    last_tap = 0;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }

                if (self->IsVideoPlaybackRunning()) {
                    if (press < 800 && self->HasPcStreamAudioControl()) {
                        self->TogglePcStreamAudio();
                    } else {
                        self->StopVideoPlayback();
                    }
                    last_tap = 0;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }

                // long tap
                if (press > 3000) {
                    self->EnterWifiConfigMode();
                } else {
                    // On the clock home screen touches are ignored: the AI is
                    // entered by wake word or the BOOT button only
                    if (self->clock_home_ && self->clock_home_->IsVisible()) {
                        last_tap = 0;
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }

                    if (self->HandleKeyboardTouch(last_x, last_y)) {
                        last_tap = 0;
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }

                    if (self->IsChatInputTouch(last_x, last_y)) {
                        self->ShowTouchKeyboard();
                        last_tap = 0;
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }

                    if (self->IsMuteIconTouch(last_x, last_y)) {
                        self->ToggleMute();
                        last_tap = 0;
                        vTaskDelay(pdMS_TO_TICKS(50));
                        continue;
                    }

                    // double tap
                    if (now - last_tap < 250) {
                        app.StartListening();
                        last_tap = 0;
                    } else {
                        // single tap
                        app.ToggleChatState();
                        last_tap = now;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeTouch() {
        if (!touch_.Init(codec_i2c_bus_, 0x38)) return;
        // 8 KB: the touch task drives audio toggle and video stop, and the
        // stop path runs a synchronous LVGL refresh (lv_refr_now) which
        // overflows a 4 KB stack
        xTaskCreatePinnedToCore(TouchTask, "touch_task", 8192, this, 5, nullptr, 0);
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MIS0_PIN;
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }

            // Alarm ringing? BOOT dismisses it.
            if (alarm_manager_ && alarm_manager_->IsRinging()) {
                ESP_LOGI(TAG, "BOOT pressed -> dismiss alarm");
                alarm_manager_->Dismiss();
                return;
            }

            // Playing music? BOOT stops it first (like a media stop button).
            if (music_player_ && music_player_->IsPlaying()) {
                music_player_->RequestStop();
                return;
            }

            // BOOT acts as the "home" button: from video or an AI session it
            // returns to the clock screen; from the clock screen it starts a
            // conversation.
            if (StopVideoPlayback()) {
                RequestGoHome();
                return;
            }
            if (clock_home_ && !clock_home_->IsVisible()) {
                RequestGoHome();
                if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
                        state == kDeviceStateSpeaking) {
                    app.ToggleChatState();  // end the session so idle is reached
                }
                return;
            }
            app.ToggleChatState();
        });
    }

    bool StartPcStream(int max_fps = kPcStreamDefaultFps) {
        if (!video_stream_player_) {
            return false;
        }

        PrepareForVideoPlayback();
        youtube_storyboard_player_->Stop();
        if (audio_stream_player_) {
            audio_stream_player_->Stop();
        }

        ESP_LOGI(TAG, "Internal heap before stream: free=%u largest=%u, dma_free=%u dma_largest=%u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));

        std::string base_url = ResolvePcStreamBaseUrl();
        // RGB332: half the bandwidth of RGB565 (57.6 KB/frame, 12 fps at
        // ~5.5 Mbps). RGB565 was measured to cap at ~7 fps on this link
        // (needs ~11 Mbps for 12 fps, link sustains ~7 Mbps).
        std::string video_url = base_url + "/stream.rgb332";
        pc_stream_audio_url_ = base_url + "/audio.pcm";
        if (!video_stream_player_->Start(video_url, max_fps)) {
            RestoreAfterVideoPlayback();
            return false;
        }

        bool audio_started = false;
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr && codec->output_volume() <= 0) {
            ESP_LOGI(TAG, "PC audio stream skipped because output volume is 0");
        } else if (audio_stream_player_ && audio_stream_player_->Start(pc_stream_audio_url_)) {
            audio_started = true;
        } else if (audio_stream_player_) {
            ESP_LOGW(TAG, "PC audio stream failed to start, video continues");
        }
        display_->SetVideoAudioIcon(audio_started ? 1 : 0);
        return true;
    }

    void TogglePcStreamAudio() {
        if (!video_stream_player_ || !video_stream_player_->IsRunning() || !audio_stream_player_) {
            return;
        }

        if (audio_stream_player_->IsRunning()) {
            audio_stream_player_->Stop();
            display_->SetVideoAudioIcon(0);
            ESP_LOGI(TAG, "PC stream audio muted by touch");
            return;
        }

        if (pc_stream_audio_url_.empty()) {
            return;
        }
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr && codec->output_volume() <= 0) {
            codec->SetOutputVolume(saved_volume_ > 0 ? saved_volume_ : 70);
        }
        if (audio_stream_player_->Start(pc_stream_audio_url_)) {
            display_->SetVideoAudioIcon(1);
            ESP_LOGI(TAG, "PC stream audio enabled by touch");
        } else {
            ESP_LOGW(TAG, "PC stream audio failed to start on toggle");
        }
    }

    bool IsVideoPlaybackRunning() const {
        return (video_stream_player_ && video_stream_player_->IsRunning()) ||
               (youtube_storyboard_player_ && youtube_storyboard_player_->IsRunning());
    }

    // Audio toggle only applies to the PC stream (it has a paired audio URL)
    bool HasPcStreamAudioControl() const {
        return video_stream_player_ && video_stream_player_->IsRunning() &&
               audio_stream_player_ && !pc_stream_audio_url_.empty();
    }

    // Status-bar speaker icon (top-right) in the normal UI
    bool IsMuteIconTouch(uint16_t raw_x, uint16_t raw_y) const {
        if (keyboard_overlay_ != nullptr) {
            return false;
        }
        int sx = 0;
        int sy = 0;
        return MapTouchToScreen(raw_x, raw_y, sx, sy) && sx >= DISPLAY_WIDTH - 70 && sy <= 44;
    }

    void ToggleMute() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            return;
        }
        if (codec->output_volume() > 0) {
            saved_volume_ = codec->output_volume();
            codec->SetOutputVolume(0);
            display_->ShowNotification("Da tat tieng", 1500);
            ESP_LOGI(TAG, "Speaker muted by touch");
        } else {
            codec->SetOutputVolume(saved_volume_ > 0 ? saved_volume_ : 70);
            display_->ShowNotification("Da bat tieng", 1500);
            ESP_LOGI(TAG, "Speaker unmuted by touch, volume=%d", codec->output_volume());
        }
    }

    std::string ResolvePcStreamBaseUrl() {
        std::string fallback = "http://192.168.0.174:8088";
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "PC stream discovery socket failed, using fallback base: %s", fallback.c_str());
            return fallback;
        }

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        struct timeval timeout = {};
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(kPcDiscoveryPort);
        dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        ESP_LOGI(TAG, "Discovering PC stream server on UDP port %d", kPcDiscoveryPort);
        for (int attempt = 0; attempt < 3; ++attempt) {
            sendto(sock, kPcDiscoveryRequest, strlen(kPcDiscoveryRequest), 0,
                reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

            char buffer[96] = {};
            sockaddr_in from = {};
            socklen_t from_len = sizeof(from);
            int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                reinterpret_cast<sockaddr*>(&from), &from_len);
            if (len <= 0) {
                continue;
            }
            buffer[len] = '\0';
            if (strncmp(buffer, kPcDiscoveryResponse, strlen(kPcDiscoveryResponse)) != 0) {
                continue;
            }

            int port = 8088;
            sscanf(buffer + strlen(kPcDiscoveryResponse), "%d", &port);
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            close(sock);

            std::string base_url = "http://";
            base_url += ip;
            base_url += ":";
            base_url += std::to_string(port);
            ESP_LOGI(TAG, "PC stream server discovered: %s", base_url.c_str());
            return base_url;
        }

        close(sock);
        ESP_LOGW(TAG, "PC stream discovery timeout, using fallback base: %s", fallback.c_str());
        return fallback;
    }

    void StartPcStreamFromButton() {
        ESP_LOGI(TAG, "PC stream test button pressed");
        if (StartPcStream()) {
            display_->ShowNotification("Opening PC stream...", 1500);
        } else {
            display_->ShowNotification("PC stream failed", 2500);
        }
    }

    bool IsPcStreamButtonTouch(uint16_t raw_x, uint16_t raw_y) const {
        constexpr int x1 = DISPLAY_WIDTH - kStreamButtonW - kStreamButtonMargin;
        constexpr int y1 = DISPLAY_HEIGHT - kStreamButtonH - kStreamButtonMargin;
        constexpr int x2 = DISPLAY_WIDTH - kStreamButtonMargin;
        constexpr int y2 = DISPLAY_HEIGHT - kStreamButtonMargin;

        auto inside = [&](int x, int y) {
            return x >= x1 && x <= x2 && y >= y1 && y <= y2;
        };

        if (inside(raw_x, raw_y)) {
            return true;
        }

        int swap_x = raw_y;
        int swap_y = raw_x;
        if (inside(swap_x, swap_y)) {
            return true;
        }

        if (inside(DISPLAY_WIDTH - 1 - swap_x, swap_y) ||
                inside(swap_x, DISPLAY_HEIGHT - 1 - swap_y) ||
                inside(DISPLAY_WIDTH - 1 - swap_x, DISPLAY_HEIGHT - 1 - swap_y)) {
            return true;
        }

        // FT6336G often reports 240x320 raw coordinates while the LCD is 320x240 landscape.
        // Treat the lower-right raw area as the test button too, so the debug button remains usable
        // even if the touch rotation differs from the LVGL screen rotation.
        if ((raw_x >= 120 && raw_y >= 160) ||
                (raw_x <= 120 && raw_y >= 220) ||
                (raw_x >= 180 && raw_y >= 120)) {
            return true;
        }

        return false;
    }

    static void PcStreamButtonEvent(lv_event_t* event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
            return;
        }

        auto* self = static_cast<FreenoveESP32S3Display*>(lv_event_get_user_data(event));
        if (self != nullptr) {
            self->StartPcStreamFromButton();
        }
    }

    void InitializePcStreamButton() {
        DisplayLockGuard lock(display_);

        pc_stream_button_ = lv_button_create(lv_screen_active());
        lv_obj_set_size(pc_stream_button_, kStreamButtonW, kStreamButtonH);
        lv_obj_align(pc_stream_button_, LV_ALIGN_BOTTOM_RIGHT, -kStreamButtonMargin, -kStreamButtonMargin);
        lv_obj_set_style_radius(pc_stream_button_, 6, 0);
        lv_obj_set_style_bg_color(pc_stream_button_, lv_color_hex(0x1F6FEB), 0);
        lv_obj_set_style_bg_opa(pc_stream_button_, LV_OPA_80, 0);
        lv_obj_set_style_border_width(pc_stream_button_, 1, 0);
        lv_obj_set_style_border_color(pc_stream_button_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_event_cb(pc_stream_button_, PcStreamButtonEvent, LV_EVENT_CLICKED, this);

        auto* label = lv_label_create(pc_stream_button_);
        lv_label_set_text(label, "PC STREAM");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        lv_obj_move_foreground(pc_stream_button_);
    }

    void EnsurePcStreamButton() {
        if (pc_stream_button_ != nullptr || display_ == nullptr || !display_->IsSetupUICalled()) {
            return;
        }

        InitializePcStreamButton();
        ESP_LOGI(TAG, "PC stream test button created");
    }

    void InitializeChatInputBar() {
        DisplayLockGuard lock(display_);
        chat_input_bar_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(chat_input_bar_, DISPLAY_WIDTH - 12, 34);
        lv_obj_align(chat_input_bar_, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_radius(chat_input_bar_, 6, 0);
        lv_obj_set_style_bg_color(chat_input_bar_, lv_color_hex(0x161B22), 0);
        lv_obj_set_style_bg_opa(chat_input_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chat_input_bar_, 1, 0);
        lv_obj_set_style_border_color(chat_input_bar_, lv_color_hex(0x30363D), 0);
        lv_obj_set_style_pad_all(chat_input_bar_, 0, 0);
        lv_obj_set_scrollbar_mode(chat_input_bar_, LV_SCROLLBAR_MODE_OFF);

        chat_input_label_ = lv_label_create(chat_input_bar_);
        lv_label_set_text(chat_input_label_, "Nhap chat...");
        lv_obj_set_style_text_color(chat_input_label_, lv_color_hex(0x8B949E), 0);
        lv_obj_align(chat_input_label_, LV_ALIGN_LEFT_MID, 10, 0);
        lv_obj_move_foreground(chat_input_bar_);
        ESP_LOGI(TAG, "Chat input bar created");
    }

    void EnsureChatInputBar() {
        if (chat_input_bar_ != nullptr || display_ == nullptr || !display_->IsSetupUICalled()) {
            return;
        }
        InitializeChatInputBar();
    }

    // Drives the clock-home lifecycle from the touch task's 50 ms loop:
    // shows it at boot and on go-home requests, hides it when a conversation
    // starts (wake word), and starts the LAN config server once online.
    void PollHomeState() {
        if (!clock_home_ || display_ == nullptr || !display_->IsSetupUICalled()) {
            return;
        }
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        int64_t now_ms = esp_timer_get_time() / 1000;

#if ZING_MUSIC_BOOT_TEST
        // DEBUG: once the device first reaches idle (network + codec ready),
        // wait a few seconds then auto-play a fixed song so the music path can
        // be debugged over serial without any user interaction.
        if (!boot_music_test_done_ && music_player_ && state == kDeviceStateIdle) {
            if (boot_music_first_idle_ms_ == 0) {
                boot_music_first_idle_ms_ = now_ms;
            } else if (now_ms - boot_music_first_idle_ms_ > 4000) {
                boot_music_test_done_ = true;
                ESP_LOGI(TAG, "[BOOT TEST] auto-playing '%s'", ZING_MUSIC_BOOT_TEST_QUERY);
                music_player_->Start(ZING_MUSIC_BOOT_TEST_QUERY);
            }
        }
#endif

#if ALARM_BOOT_TEST
        if (!alarm_test_done_ && alarm_manager_ && state == kDeviceStateIdle) {
            if (alarm_test_first_idle_ms_ == 0) {
                alarm_test_first_idle_ms_ = now_ms;
            } else if (now_ms - alarm_test_first_idle_ms_ > 6000) {
                alarm_test_done_ = true;
                ESP_LOGI(TAG, "[ALARM TEST] ringing style 1 (Chime)");
                alarm_manager_->DebugRing(1);
            }
        }
#endif

        // Wake word (listening) interrupts music so the assistant can be heard.
        bool music_playing = music_player_ && music_player_->IsPlaying();
        if (music_playing && state == kDeviceStateListening) {
            music_player_->RequestStop();
            music_playing = false;
        }

        // Now-playing overlay (album art + title) follows playback state. All UI
        // work happens here on the touch task, so it never races clock_home_.
        if (music_ui_) {
            if (music_playing) {
                std::string t, a;
                music_player_->GetNowPlaying(t, a);
                if (!music_ui_->IsVisible()) {
                    if (clock_home_->IsVisible()) {
                        clock_home_->Hide();
                    }
                    music_ui_->Show(t.c_str(), a.c_str());
                    music_ui_text_ = t + "\n" + a;
                } else {
                    std::string combined = t + "\n" + a;
                    if (combined != music_ui_text_) {
                        music_ui_->UpdateText(t.c_str(), a.c_str());
                        music_ui_text_ = combined;
                    }
                }
                int w = 0, h = 0;
                uint16_t* art = music_player_->TakeAlbumArt(&w, &h);
                if (art != nullptr) {
                    music_ui_->SetAlbumArt(art, w, h);
                }
            } else if (music_ui_->IsVisible()) {
                music_ui_->Hide();
                music_ui_text_.clear();
            }
        }

        if (clock_home_->IsVisible()) {
            if (state == kDeviceStateConnecting || state == kDeviceStateListening) {
                clock_home_->Hide();
            }
        } else {
            // Keep the chat UI (with the song title) up while music plays;
            // the clock home returns automatically once playback ends.
            bool video_running = IsVideoPlaybackRunning() || music_playing;
            if (!home_boot_shown_ && state == kDeviceStateIdle && !video_running) {
                home_boot_shown_ = true;
                clock_home_->Show();
            }
            if (go_home_pending_ && !video_running) {
                if (state == kDeviceStateIdle) {
                    go_home_pending_ = false;
                    clock_home_->Show();
                } else if (state == kDeviceStateListening &&
                           now_ms - go_home_request_ms_ > 6000) {
                    // The assistant re-entered listening after confirming the
                    // go-home command; close the session so idle is reached
                    go_home_request_ms_ = now_ms;
                    app.ToggleChatState();
                }
            }
        }

        if (!clock_home_->IsConfigServerRunning() &&
            state != kDeviceStateStarting &&
            state != kDeviceStateWifiConfiguring &&
            state != kDeviceStateActivating) {
            clock_home_->StartConfigServer();
        }

        // Add the alarm web routes onto the clock's HTTP server once it is up.
        if (!alarm_http_registered_ && alarm_manager_ && clock_home_->IsConfigServerRunning()) {
            alarm_manager_->RegisterHttp(
                static_cast<httpd_handle_t>(clock_home_->GetHttpServer()));
            alarm_http_registered_ = true;
        }
    }

    void RequestGoHome() {
        go_home_request_ms_ = esp_timer_get_time() / 1000;
        go_home_pending_ = true;
    }

    bool IsChatInputPoint(int sx, int sy) const {
        return sx >= 6 && sx < DISPLAY_WIDTH - 6 && sy >= DISPLAY_HEIGHT - 40 && sy < DISPLAY_HEIGHT - 4;
    }

    bool MapTouchToScreen(uint16_t raw_x, uint16_t raw_y, int& sx, int& sy) const {
        sx = static_cast<int>(raw_x);
        sy = static_cast<int>(raw_y);
        return sx >= 0 && sx < DISPLAY_WIDTH && sy >= 0 && sy < DISPLAY_HEIGHT;
    }

    bool IsChatInputTouch(uint16_t raw_x, uint16_t raw_y) const {
        if (chat_input_bar_ == nullptr || keyboard_overlay_ != nullptr) {
            return false;
        }
        int sx = 0;
        int sy = 0;
        return MapTouchToScreen(raw_x, raw_y, sx, sy) && IsChatInputPoint(sx, sy);
    }

    struct KeyboardKeyDef {
        const char* text;
        int x;
        int y;
        int w;
        int h;
    };

    static constexpr KeyboardKeyDef kKeyboardKeys[] = {
        {"A", 0, 50, 34, 38}, {"B", 36, 50, 34, 38}, {"C", 72, 50, 34, 38}, {"D", 108, 50, 34, 38}, {"E", 144, 50, 34, 38}, {"BKSP", 180, 50, 58, 38},
        {"F", 0, 92, 34, 38}, {"G", 36, 92, 34, 38}, {"H", 72, 92, 34, 38}, {"I", 108, 92, 34, 38}, {"J", 144, 92, 34, 38}, {"SPACE", 180, 92, 58, 38},
        {"K", 0, 134, 34, 38}, {"L", 36, 134, 34, 38}, {"M", 72, 134, 34, 38}, {"N", 108, 134, 34, 38}, {"O", 144, 134, 34, 38}, {"OK", 180, 134, 58, 38},
        {"P", 0, 176, 34, 38}, {"Q", 36, 176, 34, 38}, {"R", 72, 176, 34, 38}, {"S", 108, 176, 34, 38}, {"T", 144, 176, 34, 38}, {"ESC", 180, 176, 58, 38},
        {"U", 0, 218, 34, 38}, {"V", 36, 218, 34, 38}, {"W", 72, 218, 34, 38}, {"X", 108, 218, 34, 38}, {"Y", 144, 218, 34, 38}, {"CLR", 180, 218, 58, 38},
        {"Z", 0, 260, 34, 38}, {".", 36, 260, 34, 38}, {",", 72, 260, 34, 38}, {"?", 108, 260, 34, 38}, {"!", 144, 260, 34, 38}, {"'", 180, 260, 58, 38},
    };

    void CreateKeyboardButton(lv_obj_t* parent, const char* text, int x, int y, int w, int h) {
        auto* btn = lv_button_create(parent);
        lv_obj_set_pos(btn, x, y);
        lv_obj_set_size(btn, w, h);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2D333B), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x8B949E), 0);

        auto* label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }

    void UpdateKeyboardText() {
        if (keyboard_textarea_ != nullptr) {
            lv_textarea_set_text(keyboard_textarea_, keyboard_text_.c_str());
        }
    }

    static void KeyboardKeyFeedbackTimer(lv_timer_t* timer) {
        auto* self = static_cast<FreenoveESP32S3Display*>(lv_timer_get_user_data(timer));
        if (self != nullptr) {
            self->ClearKeyboardKeyFeedback();
        }
        lv_timer_del(timer);
    }

    void ClearKeyboardKeyFeedback() {
        keyboard_key_feedback_timer_ = nullptr;
        if (keyboard_key_feedback_ != nullptr) {
            lv_obj_del(keyboard_key_feedback_);
            keyboard_key_feedback_ = nullptr;
        }
    }

    void ShowKeyboardKeyFeedback(const std::string& key, int sx, int sy) {
        if (keyboard_overlay_ == nullptr) {
            return;
        }
        if (keyboard_key_feedback_timer_ != nullptr) {
            lv_timer_del(keyboard_key_feedback_timer_);
            keyboard_key_feedback_timer_ = nullptr;
        }
        if (keyboard_key_feedback_ != nullptr) {
            lv_obj_del(keyboard_key_feedback_);
            keyboard_key_feedback_ = nullptr;
        }

        const int w = std::min(DISPLAY_WIDTH - 12, std::max(42, static_cast<int>(key.size()) * 18 + 20));
        const int h = 46;
        const int x = std::max(4, std::min(DISPLAY_WIDTH - w - 4, sx - w / 2));
        const int y = std::max(38, sy - h - 8);

        keyboard_key_feedback_ = lv_obj_create(keyboard_overlay_);
        lv_obj_set_pos(keyboard_key_feedback_, x, y);
        lv_obj_set_size(keyboard_key_feedback_, w, h);
        lv_obj_set_style_radius(keyboard_key_feedback_, 8, 0);
        lv_obj_set_style_bg_color(keyboard_key_feedback_, lv_color_hex(0xF0F6FC), 0);
        lv_obj_set_style_bg_opa(keyboard_key_feedback_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(keyboard_key_feedback_, 2, 0);
        lv_obj_set_style_border_color(keyboard_key_feedback_, lv_color_hex(0x58A6FF), 0);
        lv_obj_set_style_shadow_width(keyboard_key_feedback_, 10, 0);
        lv_obj_set_style_shadow_opa(keyboard_key_feedback_, LV_OPA_40, 0);
        lv_obj_clear_flag(keyboard_key_feedback_, LV_OBJ_FLAG_SCROLLABLE);

        auto* label = lv_label_create(keyboard_key_feedback_);
        lv_label_set_text(label, key.c_str());
        lv_obj_set_style_text_color(label, lv_color_hex(0x0D1117), 0);
        lv_obj_center(label);
        lv_obj_move_foreground(keyboard_key_feedback_);

        keyboard_key_feedback_timer_ = lv_timer_create(KeyboardKeyFeedbackTimer, 140, this);
    }

    void ShowTouchKeyboard() {
        if (keyboard_overlay_ != nullptr) {
            return;
        }

        DisplayLockGuard lock(display_);
        keyboard_overlay_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(keyboard_overlay_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_pos(keyboard_overlay_, 0, 0);
        lv_obj_set_style_radius(keyboard_overlay_, 0, 0);
        lv_obj_set_style_border_width(keyboard_overlay_, 0, 0);
        lv_obj_set_style_pad_all(keyboard_overlay_, 0, 0);
        lv_obj_set_style_bg_color(keyboard_overlay_, lv_color_hex(0x0D1117), 0);
        lv_obj_set_style_bg_opa(keyboard_overlay_, LV_OPA_COVER, 0);

        keyboard_textarea_ = lv_textarea_create(keyboard_overlay_);
        lv_obj_set_pos(keyboard_textarea_, 4, 4);
        lv_obj_set_size(keyboard_textarea_, DISPLAY_WIDTH - 8, 40);
        lv_textarea_set_one_line(keyboard_textarea_, true);
        lv_textarea_set_placeholder_text(keyboard_textarea_, "Type chat...");
        lv_obj_set_style_radius(keyboard_textarea_, 4, 0);
        lv_obj_set_style_text_color(keyboard_textarea_, lv_color_white(), 0);
        lv_obj_set_style_bg_color(keyboard_textarea_, lv_color_hex(0x161B22), 0);
        lv_obj_set_style_border_color(keyboard_textarea_, lv_color_hex(0x30363D), 0);

        for (const auto& key : kKeyboardKeys) {
            CreateKeyboardButton(keyboard_overlay_, key.text, key.x, key.y, key.w, key.h);
        }
        lv_obj_move_foreground(keyboard_overlay_);
        UpdateKeyboardText();
        ESP_LOGI(TAG, "Touch keyboard shown");
    }

    void HideTouchKeyboard() {
        if (keyboard_overlay_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(display_);
        if (keyboard_key_feedback_timer_ != nullptr) {
            lv_timer_del(keyboard_key_feedback_timer_);
            keyboard_key_feedback_timer_ = nullptr;
        }
        keyboard_key_feedback_ = nullptr;
        lv_obj_del(keyboard_overlay_);
        keyboard_overlay_ = nullptr;
        keyboard_textarea_ = nullptr;
        ESP_LOGI(TAG, "Touch keyboard hidden");
    }

    const char* KeyboardKeyAt(int sx, int sy) const {
        for (const auto& key : kKeyboardKeys) {
            if (sx >= key.x && sx < key.x + key.w && sy >= key.y && sy < key.y + key.h) {
                return key.text;
            }
        }
        return nullptr;
    }

    static size_t Utf8CharLen(unsigned char c) {
        if ((c & 0x80) == 0) return 1;
        if ((c & 0xE0) == 0xC0) return 2;
        if ((c & 0xF0) == 0xE0) return 3;
        if ((c & 0xF8) == 0xF0) return 4;
        return 1;
    }

    static void PopLastUtf8Char(std::string& text) {
        if (text.empty()) {
            return;
        }
        size_t pos = text.size() - 1;
        while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        text.erase(pos);
    }

    static const char* VietnameseVowel(int family, int tone) {
        static const char* kForms[][6] = {
            {"a", "á", "à", "ả", "ã", "ạ"},
            {"ă", "ắ", "ằ", "ẳ", "ẵ", "ặ"},
            {"â", "ấ", "ầ", "ẩ", "ẫ", "ậ"},
            {"e", "é", "è", "ẻ", "ẽ", "ẹ"},
            {"ê", "ế", "ề", "ể", "ễ", "ệ"},
            {"i", "í", "ì", "ỉ", "ĩ", "ị"},
            {"o", "ó", "ò", "ỏ", "õ", "ọ"},
            {"ô", "ố", "ồ", "ổ", "ỗ", "ộ"},
            {"ơ", "ớ", "ờ", "ở", "ỡ", "ợ"},
            {"u", "ú", "ù", "ủ", "ũ", "ụ"},
            {"ư", "ứ", "ừ", "ử", "ữ", "ự"},
            {"y", "ý", "ỳ", "ỷ", "ỹ", "ỵ"},
        };
        if (family < 0 || family >= 12 || tone < 0 || tone >= 6) {
            return nullptr;
        }
        return kForms[family][tone];
    }

    static bool MatchVietnameseVowelAt(const std::string& text, size_t pos, int& family, int& tone, size_t& len) {
        for (int f = 0; f < 12; ++f) {
            for (int t = 0; t < 6; ++t) {
                const char* form = VietnameseVowel(f, t);
                size_t form_len = strlen(form);
                if (pos + form_len <= text.size() && text.compare(pos, form_len, form) == 0) {
                    family = f;
                    tone = t;
                    len = form_len;
                    return true;
                }
            }
        }
        return false;
    }

    static size_t LastWordStart(const std::string& text) {
        size_t pos = text.size();
        while (pos > 0) {
            size_t char_pos = pos - 1;
            while (char_pos > 0 && (static_cast<unsigned char>(text[char_pos]) & 0xC0) == 0x80) {
                --char_pos;
            }
            if (text[char_pos] == ' ') {
                return char_pos + 1;
            }
            pos = char_pos;
        }
        return 0;
    }

    bool FindLastVietnameseVowel(size_t& pos, size_t& len, int& family, int& tone) const {
        size_t start = LastWordStart(keyboard_text_);
        size_t cursor = start;
        bool found = false;
        while (cursor < keyboard_text_.size()) {
            int f = -1;
            int t = 0;
            size_t char_len = Utf8CharLen(static_cast<unsigned char>(keyboard_text_[cursor]));
            size_t vowel_len = 0;
            if (MatchVietnameseVowelAt(keyboard_text_, cursor, f, t, vowel_len)) {
                pos = cursor;
                len = vowel_len;
                family = f;
                tone = t;
                char_len = vowel_len;
                found = true;
            }
            cursor += char_len;
        }
        return found;
    }

    bool ReplaceLastVowelFamily(int from_family, int to_family) {
        size_t pos = 0;
        size_t len = 0;
        int family = -1;
        int tone = 0;
        if (!FindLastVietnameseVowel(pos, len, family, tone) || family != from_family) {
            return false;
        }
        const char* replacement = VietnameseVowel(to_family, tone);
        if (replacement == nullptr) {
            return false;
        }
        keyboard_text_.replace(pos, len, replacement);
        return true;
    }

    bool ApplyTone(int tone) {
        size_t pos = 0;
        size_t len = 0;
        int family = -1;
        int old_tone = 0;
        if (!FindLastVietnameseVowel(pos, len, family, old_tone)) {
            return false;
        }
        const char* replacement = VietnameseVowel(family, old_tone == tone ? 0 : tone);
        if (replacement == nullptr) {
            return false;
        }
        keyboard_text_.replace(pos, len, replacement);
        return true;
    }

    bool ApplyVietnameseTelex(char key) {
        size_t word_start = LastWordStart(keyboard_text_);
        std::string word = keyboard_text_.substr(word_start);
        if (word.empty()) {
            return false;
        }

        if (key == 'd' && word == "d") {
            keyboard_text_.replace(word_start, keyboard_text_.size() - word_start, "đ");
            return true;
        }
        if (key == 'a') return ReplaceLastVowelFamily(0, 2);
        if (key == 'e') return ReplaceLastVowelFamily(3, 4);
        if (key == 'o') return ReplaceLastVowelFamily(6, 7);
        if (key == 'w') {
            return ReplaceLastVowelFamily(0, 1) ||
                   ReplaceLastVowelFamily(6, 8) ||
                   ReplaceLastVowelFamily(9, 10);
        }
        if (key == 's') return ApplyTone(1);
        if (key == 'f') return ApplyTone(2);
        if (key == 'r') return ApplyTone(3);
        if (key == 'x') return ApplyTone(4);
        if (key == 'j') return ApplyTone(5);
        return false;
    }

    bool TryKeyboardPoint(int sx, int sy) {
        if (sx < 0 || sx >= DISPLAY_WIDTH || sy < 0 || sy >= DISPLAY_HEIGHT) {
            return false;
        }
        const char* key = KeyboardKeyAt(sx, sy);
        if (key == nullptr) {
            return false;
        }

        std::string action(key);
        {
            DisplayLockGuard lock(display_);
            ShowKeyboardKeyFeedback(action, sx, sy);
        }

        if (action == "BKSP") {
            PopLastUtf8Char(keyboard_text_);
        } else if (action == "SPACE") {
            keyboard_text_ += " ";
        } else if (action == "CLR") {
            keyboard_text_.clear();
        } else if (action == "CLOSE" || action == "ESC") {
            HideTouchKeyboard();
            return true;
        } else if (action == "SEND" || action == "OK") {
            if (!keyboard_text_.empty()) {
                auto text = keyboard_text_;
                keyboard_text_.clear();
                HideTouchKeyboard();
                Application::GetInstance().SendTextChat(text);
            }
            return true;
        } else {
            char key = static_cast<char>(std::tolower(static_cast<unsigned char>(action[0])));
            if (!ApplyVietnameseTelex(key)) {
                keyboard_text_ += key;
            }
        }

        DisplayLockGuard lock(display_);
        UpdateKeyboardText();
        return true;
    }

    bool HandleKeyboardTouch(uint16_t raw_x, uint16_t raw_y) {
        if (keyboard_overlay_ == nullptr) {
            return false;
        }

        int sx = 0;
        int sy = 0;
        if (MapTouchToScreen(raw_x, raw_y, sx, sy)) {
            TryKeyboardPoint(sx, sy);
        }
        return true;
    }

    void PrepareForVideoPlayback() {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        ESP_LOGI(TAG, "Preparing video playback, app state=%d", static_cast<int>(state));
        if (clock_home_) {
            clock_home_->Hide();  // also restores LVGL rotation to 0
        }
        display_->SetRawPanelGeometry(320, 240, false, false, true);
        // Grab the DMA strip buffers before the audio pipeline (I2S + HTTP)
        // takes its share of internal RAM, otherwise the first frame draw can
        // fail with no DMA memory and the screen stays black
        display_->PreallocateVideoStripBuffers();
        display_->SetVideoOverlayActive(true);
        HideTouchKeyboard();
        if (chat_input_bar_ != nullptr) {
            DisplayLockGuard lock(display_);
            lv_obj_add_flag(chat_input_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        app.EnterExclusiveVideoMode();
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    void RestoreAfterVideoPlayback() {
        display_->SetVideoAudioIcon(-1);
        display_->SetVideoOverlayActive(false);
        display_->SetRawPanelGeometry(DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        Application::GetInstance().ExitExclusiveVideoMode();
        if (chat_input_bar_ != nullptr) {
            DisplayLockGuard lock(display_);
            lv_obj_remove_flag(chat_input_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(chat_input_bar_);
        }
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        ESP_LOGI(TAG, "Install LCD driver ILI9341");
        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTools() {
        video_stream_player_ = std::make_unique<VideoStreamPlayer>(display_);
        audio_stream_player_ = std::make_unique<PcmAudioStreamPlayer>(&Application::GetInstance().GetAudioService());
        youtube_storyboard_player_ = std::make_unique<YoutubeStoryboardPlayer>(display_);
        clock_home_ = std::make_unique<ClockHome>(display_);
        music_player_ = std::make_unique<ZingMusicPlayer>(display_);
        music_ui_ = std::make_unique<MusicUi>(display_);
        alarm_manager_ = std::make_unique<AlarmManager>(display_);
        alarm_manager_->RegisterMcpTools();
        alarm_manager_->Start();
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.screen.go_home",
            "Ve man hinh chinh hien thi dong ho. Dung tool nay khi nguoi dung noi: ve man hinh chinh, ve home, hien dong ho, ket thuc tro chuyen va ve dong ho.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                RequestGoHome();
                return std::string("returning to clock home screen");
            });

        mcp_server.AddTool("self.music.play",
            "Tim va phat nhac tu Zing MP3 theo ten bai hat va/hoac ca si. Dung tool nay khi nguoi dung noi: phat nhac, phat bai ..., mo bai hat ..., nghe nhac ..., bat bai ... cua ...",
            PropertyList({
                Property("query", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto query = properties["query"].value<std::string>();
                if (query.empty()) {
                    throw std::runtime_error("empty query");
                }
                StopVideoPlayback();
                if (!music_player_ || !music_player_->Start(query)) {
                    throw std::runtime_error("Failed to start music player");
                }
                return std::string("Dang tim va phat: ") + query;
            });

        mcp_server.AddTool("self.music.stop",
            "Dung phat nhac Zing MP3. Dung tool nay khi nguoi dung noi: dung nhac, tat nhac, dung phat nhac.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (music_player_) {
                    music_player_->Stop();
                }
                return std::string("da dung nhac");
            });

        mcp_server.AddUserOnlyTool("self.video.play_stream",
            "Play an MJPEG/JPEG-over-HTTP video stream on the TFT. Use a local proxy for YouTube URLs.",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("max_fps", kPropertyTypeInteger, 5, 1, 15)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto max_fps = properties["max_fps"].value<int>();
                StopVideoPlayback();
                PrepareForVideoPlayback();
                youtube_storyboard_player_->Stop();
                if (!video_stream_player_->Start(url, max_fps)) {
                    throw std::runtime_error("Failed to start video stream task");
                }
                return std::string("video stream started");
            });

        mcp_server.AddTool("self.video.play_pc_stream",
            "Mo stream video MJPEG tu PC/phone proxy de test video tren TFT. Dung tool nay khi nguoi dung noi mo stream tu PC, phat video tu may tinh, hoac thu stream video.",
            PropertyList({
                Property("max_fps", kPropertyTypeInteger, 12, 1, 15)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto max_fps = properties["max_fps"].value<int>();
                if (!StartPcStream(max_fps)) {
                    throw std::runtime_error("Failed to start PC MJPEG stream");
                }
                return std::string("pc mjpeg stream started");
            });

        mcp_server.AddUserOnlyTool("self.video.stop",
            "Stop the current video stream and restore the normal XiaoZhi screen.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                return std::string("video stream stopped");
            });

        mcp_server.AddUserOnlyTool("self.video.status",
            "Return whether the video stream player is running.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                if (video_stream_player_->IsRunning()) {
                    if (audio_stream_player_ && audio_stream_player_->IsRunning()) {
                        return std::string("mjpeg+pcm running");
                    }
                    return std::string("mjpeg running");
                }
                if (youtube_storyboard_player_->IsRunning()) {
                    return std::string("youtube storyboard running");
                }
                return std::string("stopped");
            });

        mcp_server.AddTool("self.keyboard.show",
            "Hien ban phim cam ung tren man hinh TFT de nguoi dung go chat bang tay.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                ShowTouchKeyboard();
                return std::string("touch keyboard shown");
            });

        mcp_server.AddUserOnlyTool("self.keyboard.hide",
            "Hide the touch keyboard from the TFT.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                HideTouchKeyboard();
                return std::string("touch keyboard hidden");
            });

        mcp_server.AddUserOnlyTool("self.keyboard.send_text",
            "Show typed text in the local chat UI. Text AI chat is disabled in this firmware build.",
            PropertyList({
                Property("text", kPropertyTypeString)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto text = properties["text"].value<std::string>();
                if (!Application::GetInstance().SendTextChat(text)) {
                    throw std::runtime_error("Failed to send text chat");
                }
                return std::string("text chat sent");
            });

        mcp_server.AddUserOnlyTool("self.youtube.play_storyboard",
            "Play YouTube storyboard images directly from YouTube without a PC or cloud proxy. This is a silent slideshow preview, not real video playback.",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("level", kPropertyTypeInteger, 2, 1, 5),
                Property("frame_delay_ms", kPropertyTypeInteger, 250, 100, 5000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto level = properties["level"].value<int>();
                auto frame_delay_ms = properties["frame_delay_ms"].value<int>();
                StopVideoPlayback();
                PrepareForVideoPlayback();
                if (!youtube_storyboard_player_->Start(url, level, frame_delay_ms)) {
                    throw std::runtime_error("Failed to start YouTube storyboard player");
                }
                return std::string("youtube storyboard started");
            });

        mcp_server.AddUserOnlyTool("self.youtube.stop",
            "Stop YouTube storyboard playback and restore the normal XiaoZhi screen.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                return std::string("youtube storyboard stopped");
            });

        mcp_server.AddTool("self.youtube.play_test_video",
            "Phat video test YouTube storyboard tren man hinh TFT. Bat buoc dung tool nay khi nguoi dung noi mo/phat video test so 1, so 2, hoac so 3. Khong goi tool danh sach neu nguoi dung yeu cau mo/phat video.",
            PropertyList({
                Property("index", kPropertyTypeInteger, 1, 1, 3),
                Property("level", kPropertyTypeInteger, 2, 1, 5),
                Property("frame_delay_ms", kPropertyTypeInteger, 250, 100, 5000)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                int index = properties["index"].value<int>();
                int level = properties["level"].value<int>();
                int frame_delay_ms = properties["frame_delay_ms"].value<int>();

                const YoutubeTestVideo* selected = nullptr;
                for (const auto& video : kYoutubeTestVideos) {
                    if (video.index == index) {
                        selected = &video;
                        break;
                    }
                }
                if (selected == nullptr) {
                    throw std::runtime_error("Invalid test video index");
                }

                StopVideoPlayback();
                PrepareForVideoPlayback();
                if (!youtube_storyboard_player_->Start(selected->url, level, frame_delay_ms)) {
                    throw std::runtime_error("Failed to start YouTube storyboard test video");
                }
                return std::string("playing test video ") + std::to_string(index) + ": " + selected->title;
            });

        mcp_server.AddTool("self.youtube.play_test_video_1",
            "Mo ngay video test so 1 tren man hinh TFT. Dung tool nay khi nguoi dung noi: mo video test so 1, phat video 1, thu video 1.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                PrepareForVideoPlayback();
                if (!youtube_storyboard_player_->Start(kYoutubeTestVideos[0].url, 2, 250)) {
                    throw std::runtime_error("Failed to start YouTube storyboard test video 1");
                }
                return std::string("playing test video 1: ") + kYoutubeTestVideos[0].title;
            });

        mcp_server.AddTool("self.youtube.play_test_video_2",
            "Mo ngay video test so 2 tren man hinh TFT. Dung tool nay khi nguoi dung noi: mo video test so 2, phat video 2, thu video 2.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                PrepareForVideoPlayback();
                if (!youtube_storyboard_player_->Start(kYoutubeTestVideos[1].url, 2, 250)) {
                    throw std::runtime_error("Failed to start YouTube storyboard test video 2");
                }
                return std::string("playing test video 2: ") + kYoutubeTestVideos[1].title;
            });

        mcp_server.AddTool("self.youtube.play_test_video_3",
            "Mo ngay video test so 3 tren man hinh TFT. Dung tool nay khi nguoi dung noi: mo video test so 3, phat video 3, thu video 3.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                StopVideoPlayback();
                PrepareForVideoPlayback();
                if (!youtube_storyboard_player_->Start(kYoutubeTestVideos[2].url, 2, 250)) {
                    throw std::runtime_error("Failed to start YouTube storyboard test video 3");
                }
                return std::string("playing test video 3: ") + kYoutubeTestVideos[2].title;
            });

        mcp_server.AddUserOnlyTool("self.youtube.list_test_videos",
            "List the built-in YouTube storyboard test videos available on this device.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                std::string result;
                for (const auto& video : kYoutubeTestVideos) {
                    result += std::to_string(video.index);
                    result += ". ";
                    result += video.title;
                    result += "\n";
                }
                return result;
            });
    }

    bool StopVideoPlayback() {
        bool stopped = false;
        if (audio_stream_player_ && audio_stream_player_->IsRunning()) {
            audio_stream_player_->Stop();
            stopped = true;
        }
        if (video_stream_player_ && video_stream_player_->IsRunning()) {
            video_stream_player_->Stop();
            stopped = true;
        }
        if (youtube_storyboard_player_ && youtube_storyboard_player_->IsRunning()) {
            youtube_storyboard_player_->Stop();
            stopped = true;
        }
        if (stopped) {
            RestoreAfterVideoPlayback();
        }
        return stopped;
    }

public:
    FreenoveESP32S3Display(): boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, AUDIO_CODEC_I2C_NUM,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override { return display_; }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(FreenoveESP32S3Display);
