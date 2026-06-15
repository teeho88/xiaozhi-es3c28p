#ifndef CLOCK_HOME_H
#define CLOCK_HOME_H

#include <lvgl.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class LcdDisplay;

// Full-screen clock "home" overlay shown while the assistant idles, plus a
// small LAN HTTP server (port 80) serving a mobile-friendly config page for
// its appearance (12/24h, seconds, date, brightness, accent color).
class ClockHome {
public:
    explicit ClockHome(LcdDisplay* display);

    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }

    // Reload settings from NVS and refresh the UI/backlight
    void ApplySettings();
    // Start the LAN config HTTP server (call once the network is up)
    void StartConfigServer();
    bool IsConfigServerRunning() const { return server_ != nullptr; }
    // The httpd handle (httpd_handle_t), so other components (e.g. AlarmManager)
    // can register extra routes on the same server. nullptr until started.
    void* GetHttpServer() const { return server_; }

    // Decode a JPEG (already resized ~320x240 by the phone), store it as the
    // wallpaper, and refresh the UI. Returns false on decode/write failure.
    bool SaveWallpaper(const uint8_t* jpeg, size_t len);

private:
    void LoadSettings();
    void CreateUi();
    void UpdateClock();
    void UpdateWeatherLabel();
    uint8_t* LoadWallpaperPixels();  // PSRAM buffer (caller-owned via wallpaper_pixels_), or nullptr
    void FreeWallpaperPixels();
    static void ClockTimerCb(lv_timer_t* timer);

    // Weather worker (own task, PSRAM stack): geolocates by IP then fetches
    // current conditions, refreshing every ~30 min. Runs regardless of whether
    // the clock is visible so data is fresh when it appears.
    void StartWeatherTask();
    void WeatherLoop();
    bool FetchWeatherOnce();
    static void WeatherTaskEntry(void* arg);

    LcdDisplay* display_;
    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* wallpaper_img_ = nullptr;
    lv_obj_t* time_shadow_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* solar_label_ = nullptr;
    lv_obj_t* lunar_label_ = nullptr;
    lv_obj_t* weather_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::atomic<bool> visible_{false};
    void* server_ = nullptr;  // httpd_handle_t
    uint8_t* wallpaper_pixels_ = nullptr;  // PSRAM, RGB565 320x240, valid while shown
    lv_img_dsc_t wallpaper_dsc_ = {};

    // Weather state (guarded by weather_mutex_)
    TaskHandle_t weather_task_ = nullptr;
    StackType_t* weather_task_stack_ = nullptr;
    StaticTask_t* weather_task_tcb_ = nullptr;
    std::mutex weather_mutex_;
    std::string weather_text_;  // e.g. "28C  May rai rac"
    std::string weather_city_;
    bool weather_valid_ = false;

    // Cached settings (persisted in NVS namespace "clock")
    bool use_24h_ = true;
    bool show_seconds_ = true;
    bool show_date_ = true;
    int brightness_ = 80;
    uint32_t accent_color_ = 0x58A6FF;
};

#endif  // CLOCK_HOME_H
