#include "alarm_manager.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display/lcd_display.h"  // LcdDisplay + DisplayLockGuard
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

#define TAG "Alarm"

extern "C" {
extern const lv_font_t font_vi_28;
extern const lv_font_t font_vi_20;
}

namespace {
const char* kRingNames[] = {"Beep", "Chime", "Melody", "Digital"};
constexpr int kRingN = sizeof(kRingNames) / sizeof(kRingNames[0]);

// Append a windowed sine tone (short attack/release to avoid clicks).
void AppendTone(std::vector<int16_t>& buf, double freq, int ms, int rate, double amp) {
    int n = rate * ms / 1000;
    int atk = rate * 5 / 1000, rel = rate * 15 / 1000;
    for (int i = 0; i < n; ++i) {
        double env = 1.0;
        if (i < atk) {
            env = static_cast<double>(i) / atk;
        } else if (i > n - rel) {
            env = static_cast<double>(n - i) / rel;
        }
        double s = sin(2.0 * M_PI * freq * i / rate) * amp * env;
        buf.push_back(static_cast<int16_t>(s * 32767.0));
    }
}
void AppendSilence(std::vector<int16_t>& buf, int ms, int rate) {
    buf.insert(buf.end(), static_cast<size_t>(rate) * ms / 1000, 0);
}

// "once"/"daily"/"weekdays"/"weekends" -> repeat mask (bit0=Sun..bit6=Sat).
uint8_t ParseRepeat(const std::string& s) {
    if (s == "daily") return 0x7F;
    if (s == "weekdays") return 0x3E;  // Mon..Fri (bits 1..5)
    if (s == "weekends") return 0x41;  // Sun + Sat (bits 0,6)
    return 0;                          // "once"
}
const char* RepeatName(uint8_t mask) {
    if (mask == 0) return "một lần";
    if (mask == 0x7F) return "hằng ngày";
    if (mask == 0x3E) return "T2-T6";
    if (mask == 0x41) return "cuối tuần";
    return "tùy chọn";
}
}  // namespace

int AlarmManager::RingCount() { return kRingN; }
const char* AlarmManager::RingName(int i) { return (i >= 0 && i < kRingN) ? kRingNames[i] : nullptr; }

AlarmManager::AlarmManager(LcdDisplay* display) : display_(display) {
    constexpr size_t kStack = 10240;
    ring_stack_ = static_cast<StackType_t*>(heap_caps_malloc(kStack, MALLOC_CAP_SPIRAM));
    ring_tcb_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (ring_stack_ != nullptr && ring_tcb_ != nullptr) {
        ring_task_ = xTaskCreateStaticPinnedToCore(RingTaskEntry, "alarm_ring", kStack, this, 4,
                                                   ring_stack_, ring_tcb_, 1);
    } else {
        ESP_LOGE(TAG, "No memory for ring task");
    }
}

AlarmManager::~AlarmManager() {
    if (check_timer_ != nullptr) {
        esp_timer_stop(check_timer_);
        esp_timer_delete(check_timer_);
    }
}

void AlarmManager::Start() {
    Load();
    const esp_timer_create_args_t args = {
        .callback = &AlarmManager::CheckTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_check",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &check_timer_) == ESP_OK) {
        esp_timer_start_periodic(check_timer_, 2 * 1000 * 1000);  // every 2 s
        ESP_LOGI(TAG, "Alarm checker started (%d alarms)", (int)alarms_.size());
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void AlarmManager::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings("alarm", false);
    next_id_ = settings.GetInt("nextid", 1);
    std::string json = settings.GetString("list", "");
    alarms_.clear();
    if (json.empty()) return;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) return;
    cJSON* arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "alarms");
    if (cJSON_IsArray(arr)) {
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, arr) {
            Alarm a;
            cJSON* v;
            if ((v = cJSON_GetObjectItem(it, "id"))) a.id = v->valueint;
            if ((v = cJSON_GetObjectItem(it, "hour"))) a.hour = v->valueint;
            if ((v = cJSON_GetObjectItem(it, "minute"))) a.minute = v->valueint;
            if ((v = cJSON_GetObjectItem(it, "enabled"))) a.enabled = cJSON_IsTrue(v);
            if ((v = cJSON_GetObjectItem(it, "repeat_mask"))) a.repeat_mask = v->valueint & 0x7F;
            if ((v = cJSON_GetObjectItem(it, "ring"))) a.ring = v->valueint;
            if ((v = cJSON_GetObjectItem(it, "label")) && cJSON_IsString(v)) a.label = v->valuestring;
            if (a.id <= 0) a.id = next_id_++;
            if ((int)alarms_.size() < kMaxAlarms) alarms_.push_back(a);
        }
    }
    cJSON_Delete(root);
}

std::string AlarmManager::ToJsonLocked() {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& a : alarms_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", a.id);
        cJSON_AddNumberToObject(o, "hour", a.hour);
        cJSON_AddNumberToObject(o, "minute", a.minute);
        cJSON_AddBoolToObject(o, "enabled", a.enabled);
        cJSON_AddNumberToObject(o, "repeat_mask", a.repeat_mask);
        cJSON_AddNumberToObject(o, "ring", a.ring);
        cJSON_AddStringToObject(o, "label", a.label.c_str());
        cJSON_AddItemToArray(arr, o);
    }
    char* s = cJSON_PrintUnformatted(arr);
    std::string out = s ? s : "[]";
    cJSON_free(s);
    cJSON_Delete(arr);
    return out;
}

void AlarmManager::Save() {
    std::lock_guard<std::mutex> lock(mutex_);
    Settings settings("alarm", true);
    settings.SetString("list", ToJsonLocked());
    settings.SetInt("nextid", next_id_);
}

bool AlarmManager::FromJson(const std::string& json, std::string* err) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        if (err) *err = "invalid json";
        return false;
    }
    cJSON* arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "alarms");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        if (err) *err = "no alarms array";
        return false;
    }
    std::vector<Alarm> parsed;
    cJSON* it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if ((int)parsed.size() >= kMaxAlarms) break;
        Alarm a;
        cJSON* v;
        if ((v = cJSON_GetObjectItem(it, "hour"))) a.hour = v->valueint;
        if ((v = cJSON_GetObjectItem(it, "minute"))) a.minute = v->valueint;
        a.enabled = true;
        if ((v = cJSON_GetObjectItem(it, "enabled"))) a.enabled = cJSON_IsTrue(v);
        if ((v = cJSON_GetObjectItem(it, "repeat_mask"))) a.repeat_mask = v->valueint & 0x7F;
        if ((v = cJSON_GetObjectItem(it, "ring"))) a.ring = v->valueint;
        if ((v = cJSON_GetObjectItem(it, "label")) && cJSON_IsString(v)) a.label = v->valuestring;
        if (a.hour < 0 || a.hour > 23 || a.minute < 0 || a.minute > 59) continue;
        if (a.ring < 0 || a.ring >= kRingN) a.ring = 0;
        parsed.push_back(a);
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& a : parsed) a.id = next_id_++;
        alarms_ = std::move(parsed);
    }
    Save();
    return true;
}

// ---------------------------------------------------------------------------
// Wall-clock check (esp_timer task: keep it short, no flash, no blocking)
// ---------------------------------------------------------------------------
void AlarmManager::CheckTimerCb(void* arg) { static_cast<AlarmManager*>(arg)->Check(); }

void AlarmManager::Check() {
    time_t now = time(nullptr);
    if (now < 1700000000) return;  // wall clock not synced yet
    struct tm t;
    localtime_r(&now, &t);

    // Pending snooze fires first. TriggerRing only sets atomics + notifies (it
    // does not take mutex_), so it is safe to call while holding the lock.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snooze_at_ > 0 && now >= snooze_at_ && !ringing_) {
            int r = snooze_ring_;
            std::string lbl = snooze_label_;
            snooze_at_ = 0;
            TriggerRing(r, lbl);
            return;
        }
    }
    if (ringing_) return;

    int key = (t.tm_yday << 16) | (t.tm_hour << 8) | t.tm_min;
    if (key == last_fire_key_) return;

    int fire_ring = -1;
    std::string fire_label;
    bool one_shot = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& a : alarms_) {
            if (!a.enabled || a.hour != t.tm_hour || a.minute != t.tm_min) continue;
            if (a.repeat_mask != 0 && !(a.repeat_mask & (1 << t.tm_wday))) continue;
            fire_ring = a.ring;
            fire_label = a.label;
            if (a.repeat_mask == 0) {
                a.enabled = false;
                one_shot = true;
            }
            break;
        }
    }
    if (fire_ring >= 0) {
        last_fire_key_ = key;
        TriggerRing(fire_ring, fire_label);
        if (one_shot) {
            Application::GetInstance().Schedule([this]() { Save(); });
        }
    }
}

// ---------------------------------------------------------------------------
// Ring
// ---------------------------------------------------------------------------
void AlarmManager::TriggerRing(int ring, const std::string& label) {
    if (ringing_ || ring_task_ == nullptr) return;
    active_ring_ = (ring >= 0 && ring < kRingN) ? ring : 0;
    active_label_ = label;
    stop_ring_ = false;
    xTaskNotifyGive(ring_task_);
}

void AlarmManager::RingTaskEntry(void* arg) {
    auto* self = static_cast<AlarmManager*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (self->stop_ring_) continue;
        self->ringing_ = true;
        self->RingLoop();
        self->ringing_ = false;
    }
}

std::vector<int16_t> AlarmManager::BuildRingPattern(int ring) {
    std::vector<int16_t> b;
    const int r = kSampleRate;
    switch (ring) {
        case 1:  // Chime: ding-dong
            AppendTone(b, 659.25, 350, r, 0.5);
            AppendTone(b, 523.25, 550, r, 0.5);
            AppendSilence(b, 800, r);
            break;
        case 2:  // Melody: ascending arpeggio
            AppendTone(b, 523.25, 150, r, 0.5);
            AppendTone(b, 659.25, 150, r, 0.5);
            AppendTone(b, 783.99, 150, r, 0.5);
            AppendTone(b, 1046.5, 250, r, 0.5);
            AppendSilence(b, 500, r);
            break;
        case 3:  // Digital: fast warble
            for (int i = 0; i < 5; ++i) {
                AppendTone(b, 1046.5, 90, r, 0.45);
                AppendTone(b, 1318.5, 90, r, 0.45);
            }
            AppendSilence(b, 350, r);
            break;
        default:  // Beep
            AppendTone(b, 880.0, 200, r, 0.5);
            AppendSilence(b, 200, r);
            AppendTone(b, 880.0, 200, r, 0.5);
            AppendSilence(b, 600, r);
            break;
    }
    return b;
}

void AlarmManager::RingLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) return;
    if (!codec->output_enabled()) codec->EnableOutput(true);
    // SetOutputVolume writes NVS; do it on the main task (this is a PSRAM stack).
    Application::GetInstance().Schedule([]() {
        auto c = Board::GetInstance().GetAudioCodec();
        if (c != nullptr && c->output_volume() < 60) c->SetOutputVolume(85);
    });

    ShowOverlay(active_label_);
    ESP_LOGI(TAG, "Ringing: '%s' style=%s", active_label_.c_str(), kRingNames[active_ring_]);

    std::vector<int16_t> pattern = BuildRingPattern(active_ring_);
    int64_t start = esp_timer_get_time();
    while (!stop_ring_) {
        if ((esp_timer_get_time() - start) / 1000000 >= kRingTimeoutSec) {
            ESP_LOGI(TAG, "Ring timed out");
            break;
        }
        std::vector<int16_t> chunk = pattern;  // OutputData consumes the vector
        codec->OutputData(chunk);              // blocks on I2S -> real-time pacing
    }
    HideOverlay();
    ESP_LOGI(TAG, "Ring stopped (stop_ring=%d elapsed=%llds)", (int)stop_ring_.load(),
             (long long)((esp_timer_get_time() - start) / 1000000));
}

void AlarmManager::Dismiss() {
    ESP_LOGI(TAG, "Dismiss requested");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snooze_at_ = 0;
    }
    stop_ring_ = true;
}

void AlarmManager::Snooze() {
    if (!ringing_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snooze_at_ = time(nullptr) + kSnoozeMinutes * 60;
        snooze_ring_ = active_ring_;
        snooze_label_ = active_label_;
    }
    stop_ring_ = true;
    ESP_LOGI(TAG, "Snoozed %d min", kSnoozeMinutes);
}

// ---------------------------------------------------------------------------
// Ring overlay (landscape, like ClockHome)
// ---------------------------------------------------------------------------
void AlarmManager::ShowOverlay(const std::string& label) {
    if (display_ == nullptr) return;
    DisplayLockGuard lock(display_);
    // Do NOT change rotation: the overlay sits on top of whatever screen is
    // active (the clock home is already landscape) and inherits its orientation.
    // Forcing a rotation here flipped the clock to portrait on dismiss and
    // appeared to provoke spurious touch events.
    overlay_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x1A0E0E), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    char hhmm[8] = "--:--";
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    strftime(hhmm, sizeof(hhmm), "%H:%M", &t);

    lv_obj_t* icon = lv_label_create(overlay_);
    lv_label_set_text(icon, LV_SYMBOL_BELL "  Báo thức");
    lv_obj_set_style_text_font(icon, &font_vi_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xF85149), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t* time_lbl = lv_label_create(overlay_);
    lv_label_set_text(time_lbl, hhmm);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0xF0F6FC), 0);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, -16);

    if (!label.empty()) {
        lv_obj_t* lab = lv_label_create(overlay_);
        lv_label_set_text(lab, label.c_str());
        lv_obj_set_style_text_font(lab, &font_vi_20, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(0xE3B341), 0);
        lv_obj_align(lab, LV_ALIGN_CENTER, 0, 30);
    }

    lv_obj_t* hint = lv_label_create(overlay_);
    lv_label_set_text(hint, "Chạm: báo lại  •  Nút BOOT: tắt");
    lv_obj_set_style_text_font(hint, &font_vi_20, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8B949E), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_move_foreground(overlay_);
}

void AlarmManager::HideOverlay() {
    if (display_ == nullptr) return;
    DisplayLockGuard lock(display_);
    if (overlay_ != nullptr) {
        lv_obj_del(overlay_);
        overlay_ = nullptr;
    }
    // Leave rotation untouched (the clock home stays landscape underneath).
    lv_obj_invalidate(lv_screen_active());
}

// ---------------------------------------------------------------------------
// Web UI (LAN port 80, on ClockHome's httpd server)
// ---------------------------------------------------------------------------
static const char kAlarmPageHtml[] = R"HTML(<!doctype html><html lang=vi><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Báo thức</title>
<style>body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#0d1117;color:#e6edf3}
h2{color:#58a6ff}.a{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:12px;margin:10px 0}
.r{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:4px 0}input[type=time]{font-size:20px;padding:4px;background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px}
select,input[type=text]{padding:6px;background:#0d1117;color:#e6edf3;border:1px solid #30363d;border-radius:6px}
.d{display:inline-block;width:30px;text-align:center;padding:5px 0;border:1px solid #30363d;border-radius:6px;cursor:pointer;user-select:none}
.d.on{background:#1f6feb;border-color:#1f6feb;color:#fff}button{font-size:15px;padding:9px 14px;border:0;border-radius:8px;background:#238636;color:#fff;cursor:pointer;margin:4px 4px 4px 0}
button.x{background:#6e2330}button.s{background:#1f6feb}#msg{margin-top:10px;color:#3fb950}a{color:#58a6ff}</style></head><body>
<h2>⏰ Báo thức</h2><div id=list></div>
<button onclick=addRow()>+ Thêm</button><button class=s onclick=save()>Lưu</button>
<div id=msg></div><p><a href="/">← Cài đặt đồng hồ</a></p>
<script>
var rings=[],DAYS=['CN','T2','T3','T4','T5','T6','T7'];
function row(a){var d=document.createElement('div');d.className='a';
var rs=rings.map(function(n,i){return '<option value='+i+(i==a.ring?' selected':'')+'>'+n+'</option>'}).join('');
var hh=('0'+a.hour).slice(-2),mm=('0'+a.minute).slice(-2);
var days=DAYS.map(function(n,i){return '<span class="d'+((a.repeat_mask>>i&1)?' on':'')+'" data-b='+i+' onclick="this.classList.toggle(\'on\')">'+n+'</span>'}).join('');
d.innerHTML='<div class=r><input type=time class=t value="'+hh+':'+mm+'"> <label><input type=checkbox class=en '+(a.enabled?'checked':'')+'> Bật</label> <button class=x onclick="this.closest(\'.a\').remove()">Xóa</button></div>'+
'<div class=r>Chuông: <select class=rg>'+rs+'</select> <input type=text class=lb placeholder="Nhãn (vd. Dậy đi học)" value="'+(a.label||'').replace(/"/g,'&quot;')+'"></div>'+
'<div class=r>'+days+'</div>';
document.getElementById('list').appendChild(d);}
function addRow(){row({hour:7,minute:0,enabled:true,repeat_mask:0,ring:0,label:''});}
function load(){fetch('/api/alarms').then(function(r){return r.json()}).then(function(j){rings=j.rings;document.getElementById('list').innerHTML='';(j.alarms||[]).forEach(row);});}
function collect(){var out=[];document.querySelectorAll('#list .a').forEach(function(d){var t=d.querySelector('.t').value.split(':');var m=0;d.querySelectorAll('.d.on').forEach(function(s){m|=1<<(+s.dataset.b)});out.push({hour:+t[0],minute:+t[1],enabled:d.querySelector('.en').checked,ring:+d.querySelector('.rg').value,repeat_mask:m,label:d.querySelector('.lb').value});});return out;}
function save(){fetch('/api/alarms',{method:'POST',body:JSON.stringify({alarms:collect()})}).then(function(r){return r.json()}).then(function(j){document.getElementById('msg').textContent=j.ok?'Đã lưu ✓':'Lỗi: '+(j.error||'');});}
load();
</script></body></html>)HTML";

esp_err_t AlarmManager::HttpPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kAlarmPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t AlarmManager::HttpGet(httpd_req_t* req) {
    auto* self = static_cast<AlarmManager*>(req->user_ctx);
    cJSON* root = cJSON_CreateObject();
    cJSON* rings = cJSON_CreateArray();
    for (int i = 0; i < kRingN; ++i) cJSON_AddItemToArray(rings, cJSON_CreateString(kRingNames[i]));
    cJSON_AddItemToObject(root, "rings", rings);
    std::string alarms_json;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        alarms_json = self->ToJsonLocked();
    }
    cJSON* arr = cJSON_Parse(alarms_json.c_str());
    cJSON_AddItemToObject(root, "alarms", arr ? arr : cJSON_CreateArray());
    char* s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    cJSON_free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t AlarmManager::HttpPost(httpd_req_t* req) {
    auto* self = static_cast<AlarmManager*>(req->user_ctx);
    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    std::string body(total, '\0');
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, &body[received], total - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }
    std::string err;
    bool ok = self->FromJson(body, &err);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        std::string j = "{\"ok\":false,\"error\":\"" + err + "\"}";
        httpd_resp_send(req, j.c_str(), HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

void AlarmManager::RegisterHttp(httpd_handle_t server) {
    if (server == nullptr) return;
    httpd_uri_t page = {.uri = "/alarms", .method = HTTP_GET, .handler = HttpPage, .user_ctx = this};
    httpd_uri_t get = {.uri = "/api/alarms", .method = HTTP_GET, .handler = HttpGet, .user_ctx = this};
    httpd_uri_t post = {.uri = "/api/alarms", .method = HTTP_POST, .handler = HttpPost, .user_ctx = this};
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
    ESP_LOGI(TAG, "Alarm web UI registered (/alarms)");
}

// ---------------------------------------------------------------------------
// MCP tools (chat control)
// ---------------------------------------------------------------------------
void AlarmManager::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.alarm.set",
        "Đặt một báo thức. Dùng khi người dùng nói: đặt báo thức, hẹn giờ báo thức, báo thức lúc ... "
        "repeat: 'once' (một lần), 'daily' (hằng ngày), 'weekdays' (T2-T6), 'weekends' (cuối tuần). "
        "ring 0=Beep 1=Chime 2=Melody 3=Digital.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59),
            Property("ring", kPropertyTypeInteger, 0, 0, kRingN - 1),
            Property("repeat", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& p) -> ReturnValue {
            Alarm a;
            a.hour = p["hour"].value<int>();
            a.minute = p["minute"].value<int>();
            a.ring = p["ring"].value<int>();
            a.repeat_mask = ParseRepeat(p["repeat"].value<std::string>());
            a.label = p["label"].value<std::string>();
            a.enabled = true;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if ((int)alarms_.size() >= kMaxAlarms) {
                    throw std::runtime_error("đã đạt số báo thức tối đa");
                }
                a.id = next_id_++;
                alarms_.push_back(a);
            }
            Save();
            char msg[96];
            snprintf(msg, sizeof(msg), "Đã đặt báo thức %02d:%02d (%s, chuông %s)", a.hour, a.minute,
                     RepeatName(a.repeat_mask), kRingNames[a.ring]);
            return std::string(msg);
        });

    mcp.AddTool("self.alarm.list", "Liệt kê các báo thức đang đặt.", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            std::lock_guard<std::mutex> lock(mutex_);
            if (alarms_.empty()) return std::string("Chưa có báo thức nào.");
            std::string out;
            char line[128];
            for (const auto& a : alarms_) {
                snprintf(line, sizeof(line), "#%d %02d:%02d %s %s%s%s\n", a.id, a.hour, a.minute,
                         RepeatName(a.repeat_mask), kRingNames[a.ring],
                         a.label.empty() ? "" : " - ", a.label.c_str());
                out += line;
                if (!a.enabled) out += "  (tắt)\n";
            }
            return out;
        });

    mcp.AddTool("self.alarm.cancel",
        "Hủy báo thức. id=0 để hủy tất cả, hoặc truyền id của báo thức cần hủy (xem self.alarm.list).",
        PropertyList({Property("id", kPropertyTypeInteger, 0, 0, 9999)}),
        [this](const PropertyList& p) -> ReturnValue {
            int id = p["id"].value<int>();
            size_t before;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                before = alarms_.size();
                if (id == 0) {
                    alarms_.clear();
                } else {
                    alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                                 [id](const Alarm& a) { return a.id == id; }),
                                  alarms_.end());
                }
            }
            Save();
            return std::string(id == 0 ? "Đã hủy tất cả báo thức." : "Đã hủy báo thức.");
        });

    mcp.AddTool("self.alarm.stop", "Tắt chuông báo thức đang reo.", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            Dismiss();
            return std::string("Đã tắt chuông.");
        });
}
