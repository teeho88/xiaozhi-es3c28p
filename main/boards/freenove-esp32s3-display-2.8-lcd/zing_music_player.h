#ifndef ZING_MUSIC_PLAYER_H
#define ZING_MUSIC_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Display;

// Plays a song from Zing MP3 by name: searches, resolves a 128 kbps MP3 URL,
// streams + decodes it (esp_audio_codec MP3), downmixes to mono and resamples
// to the codec's 16 kHz, writing PCM straight to the audio codec output. Runs on
// a dedicated PSRAM-stack task created once at construction (internal RAM is
// near-exhausted on this board). The song title is shown on the display.
class ZingMusicPlayer {
public:
    explicit ZingMusicPlayer(Display* display);

    // Search + play the best match for `query`. Returns false only if the task
    // could not be created; actual search/playback happens asynchronously.
    bool Start(const std::string& query);

    // Stop playback and block briefly until the worker is idle.
    void Stop();

    // Ask the worker to stop without blocking (safe from the touch/UI loop).
    void RequestStop() { stop_requested_ = true; }

    bool IsPlaying() const { return streaming_; }

    // Current song's title/artists for the now-playing UI (thread-safe).
    void GetNowPlaying(std::string& title, std::string& artists);
    // Hands a freshly decoded album-art buffer (kArtSize*kArtSize RGB565,
    // heap_caps-allocated) to the caller, transferring ownership; nullptr if none
    // pending. *w/*h are set to kArtSize. Caller frees with heap_caps_free.
    uint16_t* TakeAlbumArt(int* w, int* h);

private:
    static constexpr int kStreamConnectId = 7;
    static constexpr int kArtConnectId = 8;
    static constexpr int kArtSize = 180;  // matches MusicUi art box
    static constexpr int kOutputSampleRate = 16000;

    // Download + decode the album-art JPEG, scaled to kArtSize*kArtSize RGB565 in
    // PSRAM. Returns the buffer (ownership to caller) or nullptr.
    uint16_t* DownloadAlbumArt(const std::string& url);

    Display* display_;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> streaming_{false};
    std::mutex query_mutex_;
    std::string query_;

    // Now-playing state for the UI (guarded by ui_mutex_).
    std::mutex ui_mutex_;
    std::string np_title_;
    std::string np_artists_;
    uint16_t* art_pending_ = nullptr;  // decoded art awaiting the UI; owned here

    // Static-task backing store (PSRAM stack + internal TCB).
    StackType_t* task_stack_ = nullptr;
    StaticTask_t* task_tcb_ = nullptr;

    // Streaming resampler state (mono, src_rate -> 16 kHz, linear interpolation).
    std::vector<int16_t> mono_acc_;
    double resample_frac_ = 0.0;
    uint32_t src_rate_ = 0;

    static void TaskEntry(void* arg);
    void TaskLoop();
    void PlayStream(const std::string& url);
    void PushPcm(const int16_t* interleaved, size_t frames, uint32_t sample_rate, uint8_t channels);
    void ResetResampler() { mono_acc_.clear(); resample_frac_ = 0.0; src_rate_ = 0; }
};

#endif  // ZING_MUSIC_PLAYER_H
