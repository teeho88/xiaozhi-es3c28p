#include "zing_music_player.h"

#include "zing_client.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display/display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "http.h"
#include "network_interface.h"

#include <esp_audio_dec.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mp3_dec.h>

#include <algorithm>
#include <cstring>

#define TAG "ZingMusic"

ZingMusicPlayer::ZingMusicPlayer(Display* display) : display_(display) {
    constexpr size_t kStack = 12288;
    task_stack_ = static_cast<StackType_t*>(heap_caps_malloc(kStack, MALLOC_CAP_SPIRAM));
    task_tcb_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (task_stack_ == nullptr || task_tcb_ == nullptr) {
        ESP_LOGE(TAG, "No memory for music task");
        return;
    }
    // Pinned to core 1 so it never starves the wake-word/AFE work on core 0.
    task_ = xTaskCreateStaticPinnedToCore(TaskEntry, "zing_music", kStack, this, 4,
                                          task_stack_, task_tcb_, 1);
}

bool ZingMusicPlayer::Start(const std::string& query) {
    if (task_ == nullptr) {
        return false;
    }
    Stop();
    {
        std::lock_guard<std::mutex> lock(query_mutex_);
        query_ = query;
    }
    stop_requested_ = false;
    xTaskNotifyGive(task_);
    return true;
}

void ZingMusicPlayer::Stop() {
    stop_requested_ = true;
    for (int i = 0; i < 60 && streaming_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ZingMusicPlayer::TaskEntry(void* arg) {
    auto* self = static_cast<ZingMusicPlayer*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (self->stop_requested_) {
            continue;
        }
        self->streaming_ = true;
        self->TaskLoop();
        self->streaming_ = false;
    }
}

void ZingMusicPlayer::TaskLoop() {
    std::string query;
    {
        std::lock_guard<std::mutex> lock(query_mutex_);
        query = query_;
    }

    // Reset now-playing state -> the UI shows a "searching" card immediately.
    {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        np_title_.clear();
        np_artists_.clear();
        if (art_pending_ != nullptr) {
            heap_caps_free(art_pending_);
            art_pending_ = nullptr;
        }
    }

    ZingSong song;
    if (!zing::SearchTopSong(query, song)) {
        if (display_ != nullptr) {
            display_->ShowNotification("Không tìm thấy bài hát", 4000);
        }
        return;
    }
    if (stop_requested_) {
        return;
    }

    std::string url;
    if (!zing::GetStreamUrl(song.id, url)) {
        if (display_ != nullptr) {
            display_->ShowNotification("Không phát được (bài VIP?)", 4000);
        }
        return;
    }
    if (stop_requested_) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ui_mutex_);
        np_title_ = song.title;
        np_artists_ = song.artists;
    }
    ESP_LOGI(TAG, "Playing: %s - %s [%s]", song.title.c_str(), song.artists.c_str(), song.id.c_str());

    // Fetch the album art (best-effort; playback proceeds regardless).
    if (!song.thumbnail.empty() && !stop_requested_) {
        uint16_t* art = DownloadAlbumArt(song.thumbnail);
        if (art != nullptr) {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            if (art_pending_ != nullptr) {
                heap_caps_free(art_pending_);
            }
            art_pending_ = art;  // picked up by TakeAlbumArt()
        }
    }

    PlayStream(url);

    // Clear now-playing state so the UI returns to the clock home.
    std::lock_guard<std::mutex> lock(ui_mutex_);
    np_title_.clear();
    np_artists_.clear();
    if (art_pending_ != nullptr) {
        heap_caps_free(art_pending_);
        art_pending_ = nullptr;
    }
}

void ZingMusicPlayer::GetNowPlaying(std::string& title, std::string& artists) {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    title = np_title_;
    artists = np_artists_;
}

uint16_t* ZingMusicPlayer::TakeAlbumArt(int* w, int* h) {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    if (art_pending_ == nullptr) {
        return nullptr;
    }
    uint16_t* out = art_pending_;
    art_pending_ = nullptr;
    if (w != nullptr) *w = kArtSize;
    if (h != nullptr) *h = kArtSize;
    return out;
}

void ZingMusicPlayer::PlayStream(const std::string& url) {
    ResetResampler();

    // Open the stream, following up to a few redirects to the CDN.
    std::unique_ptr<Http> http;
    std::string current = url;
    int status = -1;
    for (int redirect = 0; redirect < 5 && !stop_requested_; ++redirect) {
        http = Board::GetInstance().GetNetwork()->CreateHttp(kStreamConnectId);
        if (!http) {
            return;
        }
        http->SetTimeout(15000);
        http->SetKeepAlive(true);
        http->SetHeader("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        if (!http->Open("GET", current)) {
            ESP_LOGE(TAG, "Failed to open music stream");
            return;
        }
        status = http->GetStatusCode();
        if (status >= 300 && status < 400) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            http.reset();
            if (location.empty()) {
                ESP_LOGE(TAG, "Redirect %d with no Location", status);
                return;
            }
            current = location;
            continue;
        }
        break;
    }
    if (!http || status != 200) {
        ESP_LOGE(TAG, "Music stream HTTP status %d", status);
        if (http) {
            http->Close();
        }
        return;
    }

    // Open the MP3 decoder (direct API, no registration needed).
    ESP_LOGI(TAG, "free internal before decoder: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    void* dec = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "Failed to open MP3 decoder");
        http->Close();
        return;
    }

    uint32_t out_cap = 8192;
    auto* out_buf = static_cast<uint8_t*>(heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out_buf == nullptr) {
        esp_mp3_dec_close(dec);
        http->Close();
        return;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        if (!codec->output_enabled()) {
            codec->EnableOutput(true);
        }
        if (codec->output_volume() == 0) {
            // SetOutputVolume persists to NVS (a flash write). This task runs on
            // a PSRAM stack, and flash writes disable the cache -> PSRAM stack
            // becomes unreadable -> assert/panic. Do it on the main task instead.
            Application::GetInstance().Schedule([]() {
                auto c = Board::GetInstance().GetAudioCodec();
                if (c != nullptr && c->output_volume() == 0) {
                    c->SetOutputVolume(80);
                }
            });
        }
    }

    std::vector<uint8_t> inbuf;
    inbuf.reserve(16 * 1024);
    char rd[4096];
    size_t total_bytes = 0;
    int empty_reads = 0;
    bool id3_checked = false;   // Zing MP3s open with an ID3v2 tag (cover art)
    size_t id3_skip = 0;        // bytes of that tag still to discard

    while (!stop_requested_) {
        int n = http->Read(rd, sizeof(rd));
        if (n < 0) {
            ESP_LOGE(TAG, "Music stream read error");
            break;
        }
        if (n == 0) {
            if (++empty_reads > 3) {
                break;  // end of stream
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        empty_reads = 0;
        total_bytes += n;
        inbuf.insert(inbuf.end(), rd, rd + n);

        // Skip the leading ID3v2 tag once (Zing serves ~18KB of it incl. cover
        // art); the raw-frame MP3 decoder cannot parse it and fails otherwise.
        if (!id3_checked && inbuf.size() >= 10) {
            id3_checked = true;
            if (inbuf[0] == 'I' && inbuf[1] == 'D' && inbuf[2] == '3') {
                size_t sz = (static_cast<size_t>(inbuf[6] & 0x7f) << 21) |
                            (static_cast<size_t>(inbuf[7] & 0x7f) << 14) |
                            (static_cast<size_t>(inbuf[8] & 0x7f) << 7) |
                            static_cast<size_t>(inbuf[9] & 0x7f);
                id3_skip = 10 + sz;
                ESP_LOGI(TAG, "Skipping ID3v2 tag: %u bytes", (unsigned)id3_skip);
            }
        }
        if (id3_skip > 0) {
            size_t drop = std::min(id3_skip, inbuf.size());
            inbuf.erase(inbuf.begin(), inbuf.begin() + drop);
            id3_skip -= drop;
            if (id3_skip > 0) {
                continue;  // whole buffer was still inside the tag; read more
            }
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = inbuf.data();
        raw.len = inbuf.size();
        size_t total_consumed = 0;

        while (raw.len > 0 && !stop_requested_) {
            esp_audio_dec_out_frame_t frame = {};
            frame.buffer = out_buf;
            frame.len = out_cap;
            esp_audio_dec_info_t info = {};
            esp_audio_err_t e = esp_mp3_dec_decode(dec, &raw, &frame, &info);

            if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                auto* grown = static_cast<uint8_t*>(
                    heap_caps_realloc(out_buf, frame.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (grown == nullptr) {
                    stop_requested_ = true;
                    break;
                }
                out_buf = grown;
                out_cap = frame.needed_size;
                continue;  // retry same input (nothing consumed yet)
            }
            if (e != ESP_AUDIO_ERR_OK) {
                // Likely a partial frame at the tail; read more bytes and retry.
                break;
            }

            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            total_consumed += raw.consumed;

            if (frame.decoded_size > 0 && info.channel > 0) {
                size_t frames = frame.decoded_size / (info.channel * sizeof(int16_t));
                PushPcm(reinterpret_cast<const int16_t*>(out_buf), frames, info.sample_rate, info.channel);
            }
            if (raw.consumed == 0) {
                break;  // no progress; need more data
            }
        }

        if (total_consumed > 0) {
            inbuf.erase(inbuf.begin(), inbuf.begin() + total_consumed);
        } else if (inbuf.size() > 4096) {
            // A full buffer produced no frame: the head is misaligned. Resync to
            // the next MP3 frame header (0xFF 0xEx) so we always move forward.
            size_t i = 1;
            for (; i + 1 < inbuf.size(); ++i) {
                if (static_cast<uint8_t>(inbuf[i]) == 0xFF &&
                    (static_cast<uint8_t>(inbuf[i + 1]) & 0xE0) == 0xE0) {
                    break;
                }
            }
            inbuf.erase(inbuf.begin(), inbuf.begin() + i);
        }
        // Safety valve: if the buffer keeps growing without decoding, drop it.
        if (inbuf.size() > 64 * 1024) {
            ESP_LOGW(TAG, "Input buffer overflow, resyncing");
            inbuf.clear();
        }
    }

    esp_mp3_dec_close(dec);
    heap_caps_free(out_buf);
    http->Close();
    ESP_LOGI(TAG, "Music stream stopped, bytes=%u", (unsigned)total_bytes);
}

void ZingMusicPlayer::PushPcm(const int16_t* interleaved, size_t frames, uint32_t sample_rate,
                              uint8_t channels) {
    if (frames == 0 || sample_rate == 0) {
        return;
    }
    if (src_rate_ != sample_rate) {
        // Sample rate changed mid-stream (rare). Restart the resampler.
        mono_acc_.clear();
        resample_frac_ = 0.0;
        src_rate_ = sample_rate;
    }

    // Downmix to mono and accumulate.
    mono_acc_.reserve(mono_acc_.size() + frames);
    if (channels >= 2) {
        for (size_t f = 0; f < frames; ++f) {
            int32_t l = interleaved[f * channels];
            int32_t r = interleaved[f * channels + 1];
            mono_acc_.push_back(static_cast<int16_t>((l + r) / 2));
        }
    } else {
        for (size_t f = 0; f < frames; ++f) {
            mono_acc_.push_back(interleaved[f]);
        }
    }

    // Resample mono_acc_ (src_rate_) -> 16 kHz with linear interpolation,
    // keeping fractional position + the trailing sample across calls.
    const double step = static_cast<double>(src_rate_) / kOutputSampleRate;
    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(mono_acc_.size() / step) + 4);
    double pos = resample_frac_;
    while (static_cast<size_t>(pos) + 1 < mono_acc_.size()) {
        size_t i = static_cast<size_t>(pos);
        double fr = pos - i;
        int32_t a = mono_acc_[i];
        int32_t b = mono_acc_[i + 1];
        out.push_back(static_cast<int16_t>(a + (b - a) * fr));
        pos += step;
    }
    size_t consumed = static_cast<size_t>(pos);
    if (consumed > mono_acc_.size()) {
        consumed = mono_acc_.size();
    }
    if (consumed > 0) {
        mono_acc_.erase(mono_acc_.begin(), mono_acc_.begin() + consumed);
        pos -= consumed;
    }
    resample_frac_ = pos;

    if (!out.empty()) {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) {
            codec->OutputData(out);  // blocks on I2S DMA -> paces to real time
        }
    }
}

uint16_t* ZingMusicPlayer::DownloadAlbumArt(const std::string& url) {
    // GET the JPEG (following a few redirects to the CDN) into a PSRAM buffer.
    std::unique_ptr<Http> http;
    std::string current = url;
    int status = -1;
    for (int redirect = 0; redirect < 4 && !stop_requested_; ++redirect) {
        http = Board::GetInstance().GetNetwork()->CreateHttp(kArtConnectId);
        if (!http) {
            return nullptr;
        }
        http->SetTimeout(10000);
        http->SetHeader("User-Agent",
                        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
        if (!http->Open("GET", current)) {
            return nullptr;
        }
        status = http->GetStatusCode();
        if (status >= 300 && status < 400) {
            std::string loc = http->GetResponseHeader("Location");
            http->Close();
            http.reset();
            if (loc.empty()) {
                return nullptr;
            }
            current = loc;
            continue;
        }
        break;
    }
    if (!http || status != 200) {
        if (http) {
            http->Close();
        }
        return nullptr;
    }

    size_t cap = 64 * 1024, len = 0;
    auto* jpeg = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg == nullptr) {
        http->Close();
        return nullptr;
    }
    char tmp[2048];
    bool bad = false;
    while (!stop_requested_) {
        int r = http->Read(tmp, sizeof(tmp));
        if (r < 0) {
            bad = true;
            break;
        }
        if (r == 0) {
            break;
        }
        if (len + static_cast<size_t>(r) > cap) {
            size_t ncap = cap * 2;
            while (len + static_cast<size_t>(r) > ncap) {
                ncap *= 2;
            }
            if (ncap > 512 * 1024) {
                bad = true;
                break;
            }
            auto* nb = static_cast<uint8_t*>(
                heap_caps_realloc(jpeg, ncap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (nb == nullptr) {
                bad = true;
                break;
            }
            jpeg = nb;
            cap = ncap;
        }
        memcpy(jpeg + len, tmp, r);
        len += r;
    }
    http->Close();
    if (bad || len == 0 || stop_requested_) {
        heap_caps_free(jpeg);
        return nullptr;
    }

    uint8_t* decoded = nullptr;
    size_t dlen = 0, dw = 0, dh = 0, stride = 0;
    esp_err_t ret = jpeg_to_image(jpeg, len, &decoded, &dlen, &dw, &dh, &stride);
    heap_caps_free(jpeg);
    if (ret != ESP_OK || decoded == nullptr || dw == 0 || dh == 0) {
        if (decoded != nullptr) {
            heap_caps_free(decoded);
        }
        ESP_LOGW(TAG, "Album art JPEG decode failed");
        return nullptr;
    }

    // Nearest-neighbour scale (cover art is square) into kArtSize*kArtSize RGB565.
    auto* art = static_cast<uint16_t*>(heap_caps_malloc(
        static_cast<size_t>(kArtSize) * kArtSize * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (art == nullptr) {
        heap_caps_free(decoded);
        return nullptr;
    }
    for (int y = 0; y < kArtSize; ++y) {
        size_t sy = static_cast<size_t>(y) * dh / kArtSize;
        const auto* src_row = reinterpret_cast<const uint16_t*>(decoded + sy * stride);
        for (int x = 0; x < kArtSize; ++x) {
            size_t sx = static_cast<size_t>(x) * dw / kArtSize;
            art[y * kArtSize + x] = src_row[sx];
        }
    }
    heap_caps_free(decoded);
    ESP_LOGI(TAG, "Album art %ux%u -> %dx%d", (unsigned)dw, (unsigned)dh, kArtSize, kArtSize);
    return art;
}
