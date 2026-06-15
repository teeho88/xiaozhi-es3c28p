#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <esp_http_server.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class LcdDisplay;

// One alarm entry. repeat_mask bit0=Sunday .. bit6=Saturday; 0 means one-shot
// (auto-disables after it fires). ring indexes the built-in ring styles.
struct Alarm {
    int id = 0;
    int hour = 7;
    int minute = 0;
    bool enabled = true;
    uint8_t repeat_mask = 0;
    int ring = 0;
    std::string label;
};

// Multi-alarm clock with weekly repeat + snooze. Alarms persist in NVS. A 2 s
// esp_timer checks the wall clock and, on a match, wakes a PSRAM-stack ring task
// that synthesises an offline tone (no network/assets) through the audio codec
// and shows a full-screen overlay until dismissed/snoozed/timed-out. Config is
// via the self.alarm.* MCP tools (chat) and a /alarms web page (LAN port 80).
class AlarmManager {
public:
    explicit AlarmManager(LcdDisplay* display);
    ~AlarmManager();

    void Start();                              // load alarms + start the checker
    void RegisterHttp(httpd_handle_t server);  // add /alarms + /api/alarms routes
    void RegisterMcpTools();                   // self.alarm.set / list / cancel

    bool IsRinging() const { return ringing_; }
    void Dismiss();  // stop the current ring (clears snooze too)
    void Snooze();   // stop + re-fire after kSnoozeMinutes

    static int RingCount();
    static const char* RingName(int i);  // nullptr if out of range

    // DEBUG: ring immediately (bypasses the clock check) for on-device testing.
    void DebugRing(int ring) { TriggerRing(ring, "TEST"); }

private:
    static constexpr int kMaxAlarms = 10;
    static constexpr int kSnoozeMinutes = 5;
    static constexpr int kRingTimeoutSec = 60;
    static constexpr int kSampleRate = 16000;

    LcdDisplay* display_;

    std::mutex mutex_;
    std::vector<Alarm> alarms_;
    int next_id_ = 1;

    void Load();
    void Save();                              // alarms_ -> NVS (string JSON)
    std::string ToJsonLocked();               // caller holds mutex_
    bool FromJson(const std::string& json, std::string* err);

    // Periodic wall-clock check (esp_timer task context: no flash, no blocking).
    esp_timer_handle_t check_timer_ = nullptr;
    int last_fire_key_ = -1;   // (yday<<16|hh<<8|mm) of the last fire, de-dupes
    int64_t snooze_at_ = 0;    // epoch seconds for a pending snooze, 0 = none
    int snooze_ring_ = 0;
    std::string snooze_label_;
    static void CheckTimerCb(void* arg);
    void Check();

    // Ring worker (PSRAM stack, created once).
    TaskHandle_t ring_task_ = nullptr;
    StackType_t* ring_stack_ = nullptr;
    StaticTask_t* ring_tcb_ = nullptr;
    std::atomic<bool> ringing_{false};
    std::atomic<bool> stop_ring_{false};
    int active_ring_ = 0;
    std::string active_label_;
    void TriggerRing(int ring, const std::string& label);  // from checker
    static void RingTaskEntry(void* arg);
    void RingLoop();
    std::vector<int16_t> BuildRingPattern(int ring);  // one loopable segment

    // Ring overlay (landscape, like ClockHome). Built/torn down on the ring task.
    lv_obj_t* overlay_ = nullptr;
    void ShowOverlay(const std::string& label);
    void HideOverlay();

    // HTTP handlers (user_ctx = this).
    static esp_err_t HttpPage(httpd_req_t* req);
    static esp_err_t HttpGet(httpd_req_t* req);
    static esp_err_t HttpPost(httpd_req_t* req);
};

#endif  // ALARM_MANAGER_H
