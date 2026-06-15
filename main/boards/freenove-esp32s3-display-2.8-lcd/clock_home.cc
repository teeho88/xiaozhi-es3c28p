#include "clock_home.h"

#include "board.h"
#include "display/lcd_display.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <wifi_manager.h>

#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "network_interface.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#define TAG "ClockHome"

// Vietnamese-capable Montserrat fonts (generated with lv_font_conv; glyph subset
// covers full Vietnamese diacritics + degree sign). Defined in font_vi_*.c in
// this board directory. The time digits keep using the built-in montserrat_48.
extern "C" {
extern const lv_font_t font_vi_28;
extern const lv_font_t font_vi_20;
extern const lv_font_t font_vi_16;
}

namespace {

// ---------------------------------------------------------------------------
// Vietnamese lunar calendar (Ho Ngoc Duc algorithm, timezone +7). Pure math,
// no network needed.
// ---------------------------------------------------------------------------
int LunarInt(double d) { return static_cast<int>(std::floor(d)); }

long JdFromDate(int dd, int mm, int yy) {
    int a = LunarInt((14 - mm) / 12.0);
    int y = yy + 4800 - a;
    int m = mm + 12 * a - 3;
    long jd = dd + LunarInt((153 * m + 2) / 5.0) + 365L * y + LunarInt(y / 4.0) -
              LunarInt(y / 100.0) + LunarInt(y / 400.0) - 32045;
    if (jd < 2299161) {
        jd = dd + LunarInt((153 * m + 2) / 5.0) + 365L * y + LunarInt(y / 4.0) - 32083;
    }
    return jd;
}

double NewMoonAa(int k) {
    double T = k / 1236.85;
    double T2 = T * T, T3 = T2 * T;
    double dr = M_PI / 180.0;
    double Jd1 = 2415020.75933 + 29.53058868 * k + 0.0001178 * T2 - 0.000000155 * T3;
    Jd1 += 0.00033 * sin((166.56 + 132.87 * T - 0.009173 * T2) * dr);
    double M = 359.2242 + 29.10535608 * k - 0.0000333 * T2 - 0.00000347 * T3;
    double Mpr = 306.0253 + 385.81691806 * k + 0.0107306 * T2 + 0.00001236 * T3;
    double F = 21.2964 + 390.67050646 * k - 0.0016528 * T2 - 0.00000239 * T3;
    double C1 = (0.1734 - 0.000393 * T) * sin(M * dr) + 0.0021 * sin(2 * dr * M);
    C1 = C1 - 0.4068 * sin(Mpr * dr) + 0.0161 * sin(2 * dr * Mpr);
    C1 = C1 - 0.0004 * sin(3 * dr * Mpr);
    C1 = C1 + 0.0104 * sin(2 * dr * F) - 0.0051 * sin((M + Mpr) * dr);
    C1 = C1 - 0.0074 * sin((M - Mpr) * dr) + 0.0004 * sin((2 * F + M) * dr);
    C1 = C1 - 0.0004 * sin((2 * F - M) * dr) - 0.0006 * sin((2 * F + Mpr) * dr);
    C1 = C1 + 0.0010 * sin((2 * F - Mpr) * dr) + 0.0005 * sin((2 * Mpr + M) * dr);
    double deltat;
    if (T < -11) {
        deltat = 0.001 + 0.000839 * T + 0.0002261 * T2 - 0.00000845 * T3 - 0.000000081 * T * T3;
    } else {
        deltat = -0.000278 + 0.000265 * T + 0.000262 * T2;
    }
    return Jd1 + C1 - deltat;
}

double SunLongitudeAa(double jdn) {
    double T = (jdn - 2451545.0) / 36525.0;
    double T2 = T * T;
    double dr = M_PI / 180.0;
    double M = 357.52910 + 35999.05030 * T - 0.0001559 * T2 - 0.00000048 * T * T2;
    double L0 = 280.46645 + 36000.76983 * T + 0.0003032 * T2;
    double DL = (1.914600 - 0.004817 * T - 0.000014 * T2) * sin(dr * M);
    DL += (0.019993 - 0.000101 * T) * sin(dr * 2 * M) + 0.000290 * sin(dr * 3 * M);
    double L = (L0 + DL) * dr;
    L = L - 2 * M_PI * LunarInt(L / (2 * M_PI));
    return L;
}

int GetSunLongitude(int dayNumber, double tz) {
    return LunarInt(SunLongitudeAa(dayNumber - 0.5 - tz / 24.0) / M_PI * 6);
}
int GetNewMoonDay(int k, double tz) { return LunarInt(NewMoonAa(k) + 0.5 + tz / 24.0); }

int GetLunarMonth11(int yy, double tz) {
    double off = JdFromDate(31, 12, yy) - 2415021.076998695;
    int k = LunarInt(off / 29.530588853);
    int nm = GetNewMoonDay(k, tz);
    if (GetSunLongitude(nm, tz) >= 9) {
        nm = GetNewMoonDay(k - 1, tz);
    }
    return nm;
}

int GetLeapMonthOffset(int a11, double tz) {
    int k = LunarInt((a11 - 2415021.076998695) / 29.530588853 + 0.5);
    int last = 0, i = 1;
    int arc = GetSunLongitude(GetNewMoonDay(k + i, tz), tz);
    do {
        last = arc;
        i++;
        arc = GetSunLongitude(GetNewMoonDay(k + i, tz), tz);
    } while (arc != last && i < 14);
    return i - 1;
}

void Solar2Lunar(int dd, int mm, int yy, double tz, int& lday, int& lmonth, int& lyear, bool& leap) {
    long dayNumber = JdFromDate(dd, mm, yy);
    int k = LunarInt((dayNumber - 2415021.076998695) / 29.530588853);
    int monthStart = GetNewMoonDay(k + 1, tz);
    if (monthStart > dayNumber) {
        monthStart = GetNewMoonDay(k, tz);
    }
    int a11 = GetLunarMonth11(yy, tz);
    int b11 = a11;
    if (a11 >= monthStart) {
        lyear = yy;
        a11 = GetLunarMonth11(yy - 1, tz);
    } else {
        lyear = yy + 1;
        b11 = GetLunarMonth11(yy + 1, tz);
    }
    lday = static_cast<int>(dayNumber - monthStart + 1);
    int diff = LunarInt((monthStart - a11) / 29.0);
    leap = false;
    lmonth = diff + 11;
    if (b11 - a11 > 365) {
        int leapMonthDiff = GetLeapMonthOffset(a11, tz);
        if (diff >= leapMonthDiff) {
            lmonth = diff + 10;
            if (diff == leapMonthDiff) {
                leap = true;
            }
        }
    }
    if (lmonth > 12) {
        lmonth -= 12;
    }
    if (lmonth >= 11 && diff < 4) {
        lyear -= 1;
    }
}

// WMO weather code -> short Vietnamese description (with diacritics)
const char* WeatherDesc(int code) {
    if (code == 0) return "Trời quang";
    if (code <= 2) return "Ít mây";
    if (code == 3) return "Nhiều mây";
    if (code == 45 || code == 48) return "Sương mù";
    if (code >= 51 && code <= 57) return "Mưa phùn";
    if (code >= 61 && code <= 67) return "Mưa";
    if (code >= 71 && code <= 77) return "Tuyết";
    if (code >= 80 && code <= 82) return "Mưa rào";
    if (code >= 85 && code <= 86) return "Mưa tuyết";
    if (code >= 95) return "Dông";
    return "";
}

constexpr int kWpWidth = 320;
constexpr int kWpHeight = 240;
constexpr size_t kWpPixels = static_cast<size_t>(kWpWidth) * kWpHeight;
constexpr size_t kWpBytes = kWpPixels * sizeof(uint16_t);
constexpr uint32_t kWpMagic = 0x31314c57;  // "WLP1" little-endian header tag
constexpr size_t kWpHeaderBytes = 16;      // magic(4) + w(2) + h(2) + padding

const esp_partition_t* WallpaperPartition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "wallpaper");
}

const char kConfigPageHtml[] = R"HTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Đồng hồ XiaoZhi</title>
<style>
body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:20px;max-width:480px;margin:auto}
h1{font-size:1.3em;color:#58a6ff}
.card{background:#161b22;border:1px solid #30363d;border-radius:10px;padding:14px;margin:12px 0}
label{display:flex;justify-content:space-between;align-items:center;padding:8px 0}
input[type=checkbox]{width:22px;height:22px}
input[type=range]{width:55%}
.colors{display:flex;gap:10px;padding:8px 0}
.sw{width:34px;height:34px;border-radius:50%;border:3px solid transparent;cursor:pointer}
.sw.sel{border-color:#fff}
button{width:100%;padding:12px;font-size:1.05em;background:#238636;color:#fff;border:0;border-radius:8px;margin-top:8px}
#msg{text-align:center;padding:6px;color:#58a6ff}
</style></head><body>
<h1>&#9200; Đồng hồ XiaoZhi</h1>
<p><a href="/alarms" style="color:#58a6ff">&#9200; Cài đặt báo thức &rarr;</a></p>
<div class="card">
<label>Định dạng 24 giờ <input type="checkbox" id="use_24h"></label>
<label>Hiện giây <input type="checkbox" id="show_seconds"></label>
<label>Hiện ngày tháng <input type="checkbox" id="show_date"></label>
<label>Độ sáng <input type="range" id="brightness" min="5" max="100"></label>
<div>Màu chữ đồng hồ</div>
<div class="colors" id="colors"></div>
</div>
<button onclick="save()">Lưu cài đặt</button>
<div id="msg"></div>
<div class="card">
<div>Hình nền</div>
<input type="file" accept="image/*" id="wp" style="width:100%;padding:8px 0">
<button onclick="upload()" style="background:#1f6feb">Tải hình nền lên</button>
<button onclick="clearwp()" style="background:#6e2630;margin-top:6px">Xóa hình nền</button>
<div id="wpmsg" style="text-align:center;padding:6px;color:#58a6ff"></div>
</div>
<script>
const PRESETS=[0x58a6ff,0xffffff,0x3fb950,0xf0883e,0xff7b72,0xd2a8ff];
let color=PRESETS[0];
const cdiv=document.getElementById('colors');
PRESETS.forEach(c=>{const d=document.createElement('div');d.className='sw';
d.style.background='#'+c.toString(16).padStart(6,'0');
d.onclick=()=>{color=c;[...cdiv.children].forEach(x=>x.classList.remove('sel'));d.classList.add('sel');};
cdiv.appendChild(d);});
function mark(){[...cdiv.children].forEach((d,i)=>d.classList.toggle('sel',PRESETS[i]===color));}
fetch('/api/clock').then(r=>r.json()).then(s=>{
['use_24h','show_seconds','show_date'].forEach(k=>document.getElementById(k).checked=!!s[k]);
document.getElementById('brightness').value=s.brightness;color=s.color;mark();});
function save(){
const body={use_24h:document.getElementById('use_24h').checked,
show_seconds:document.getElementById('show_seconds').checked,
show_date:document.getElementById('show_date').checked,
brightness:+document.getElementById('brightness').value,color:color};
fetch('/api/clock',{method:'POST',body:JSON.stringify(body)})
.then(r=>document.getElementById('msg').textContent=r.ok?'Đã lưu ✓':'Lỗi!');}
function upload(){
const f=document.getElementById('wp').files[0];
const m=document.getElementById('wpmsg');
if(!f){m.textContent='Chọn ảnh trước';return;}
m.textContent='Đang xử lý ảnh...';
const img=new Image();
img.onload=()=>{
// "cover" crop to 320x240 then export JPEG so the device only decodes a small image
const cv=document.createElement('canvas');cv.width=320;cv.height=240;
const ctx=cv.getContext('2d');
const s=Math.max(320/img.width,240/img.height);
const w=img.width*s,h=img.height*s;
ctx.drawImage(img,(320-w)/2,(240-h)/2,w,h);
cv.toBlob(b=>{
m.textContent='Đang tải lên... '+Math.round(b.size/1024)+'KB';
fetch('/api/wallpaper',{method:'POST',headers:{'Content-Type':'image/jpeg'},body:b})
.then(r=>m.textContent=r.ok?'Đã đặt hình nền ✓':'Lỗi giải mã ảnh!')
.catch(()=>m.textContent='Lỗi mạng');
},'image/jpeg',0.85);
};
img.onerror=()=>m.textContent='Ảnh không hợp lệ';
img.src=URL.createObjectURL(f);}
function clearwp(){
fetch('/api/wallpaper',{method:'DELETE'})
.then(r=>document.getElementById('wpmsg').textContent=r.ok?'Đã xóa hình nền ✓':'Lỗi!');}
</script></body></html>)HTML";

const char* kWeekdays[] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư",
                           "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};

esp_err_t HandleConfigPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kConfigPageHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandleGetClock(httpd_req_t* req) {
    Settings settings("clock", false);
    char json[160];
    snprintf(json, sizeof(json),
        "{\"use_24h\":%s,\"show_seconds\":%s,\"show_date\":%s,\"brightness\":%d,\"color\":%u}",
        settings.GetBool("use_24h", true) ? "true" : "false",
        settings.GetBool("show_seconds", true) ? "true" : "false",
        settings.GetBool("show_date", true) ? "true" : "false",
        static_cast<int>(settings.GetInt("brightness", 80)),
        static_cast<unsigned>(settings.GetInt("color", 0x58A6FF)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandlePostClock(httpd_req_t* req) {
    char buf[256];
    int len = httpd_req_recv(req, buf, std::min(req->content_len, sizeof(buf) - 1));
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == nullptr) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    {
        Settings settings("clock", true);
        auto set_bool = [&](const char* key) {
            cJSON* item = cJSON_GetObjectItem(root, key);
            if (cJSON_IsBool(item)) {
                settings.SetBool(key, cJSON_IsTrue(item));
            }
        };
        set_bool("use_24h");
        set_bool("show_seconds");
        set_bool("show_date");
        cJSON* brightness = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(brightness)) {
            int value = brightness->valueint;
            settings.SetInt("brightness", value < 5 ? 5 : (value > 100 ? 100 : value));
        }
        cJSON* color = cJSON_GetObjectItem(root, "color");
        if (cJSON_IsNumber(color)) {
            settings.SetInt("color", color->valueint & 0xFFFFFF);
        }
    }
    cJSON_Delete(root);

    auto* clock_home = static_cast<ClockHome*>(req->user_ctx);
    clock_home->ApplySettings();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandlePostWallpaper(httpd_req_t* req) {
    // The phone already cropped/resized to a small ~320x240 JPEG, so the body
    // is tens of KB at most
    if (req->content_len == 0 || req->content_len > 200 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    auto* jpeg = static_cast<uint8_t*>(heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, reinterpret_cast<char*>(jpeg) + received, req->content_len - received);
        if (r <= 0) {
            heap_caps_free(jpeg);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += r;
    }

    auto* clock_home = static_cast<ClockHome*>(req->user_ctx);
    bool ok = clock_home->SaveWallpaper(jpeg, received);
    heap_caps_free(jpeg);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "decode failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandleDeleteWallpaper(httpd_req_t* req) {
    const esp_partition_t* part = WallpaperPartition();
    if (part != nullptr) {
        esp_partition_erase_range(part, 0, part->size);
    }
    auto* clock_home = static_cast<ClockHome*>(req->user_ctx);
    clock_home->ApplySettings();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

}  // namespace

ClockHome::ClockHome(LcdDisplay* display) : display_(display) {
    LoadSettings();
}

void ClockHome::LoadSettings() {
    Settings settings("clock", false);
    use_24h_ = settings.GetBool("use_24h", true);
    show_seconds_ = settings.GetBool("show_seconds", true);
    show_date_ = settings.GetBool("show_date", true);
    brightness_ = settings.GetInt("brightness", 80);
    accent_color_ = static_cast<uint32_t>(settings.GetInt("color", 0x58A6FF));
}

void ClockHome::Show() {
    if (visible_) {
        return;
    }
    LoadSettings();
    {
        DisplayLockGuard lock(display_);
        // Rotate to landscape just for the clock; esp_lvgl_port reconfigures
        // the panel (MADCTL) on the resolution-changed event. ROTATION_270 on
        // this panel's portrait base equals the proven video-mode geometry.
        lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);
        CreateUi();
        UpdateClock();
        timer_ = lv_timer_create(ClockTimerCb, 1000, this);
    }
    Board::GetInstance().GetBacklight()->SetBrightness(brightness_);
    visible_ = true;
    ESP_LOGI(TAG, "Clock home shown");
}

void ClockHome::Hide() {
    if (!visible_) {
        return;
    }
    {
        DisplayLockGuard lock(display_);
        if (timer_ != nullptr) {
            lv_timer_del(timer_);
            timer_ = nullptr;
        }
        if (overlay_ != nullptr) {
            lv_obj_del(overlay_);  // deletes children (wallpaper image, labels) too
            overlay_ = nullptr;
            wallpaper_img_ = nullptr;
            time_shadow_ = nullptr;
            time_label_ = nullptr;
            solar_label_ = nullptr;
            lunar_label_ = nullptr;
            weather_label_ = nullptr;
            info_label_ = nullptr;
        }
        // Back to the portrait chat UI orientation
        lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);
        lv_obj_invalidate(lv_screen_active());
    }
    // The LVGL image referenced these pixels; safe to free now it is deleted
    FreeWallpaperPixels();
    visible_ = false;
    ESP_LOGI(TAG, "Clock home hidden");
}

void ClockHome::CreateUi() {
    overlay_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // Wallpaper (if any) goes behind everything else
    bool has_wallpaper = false;
    if (LoadWallpaperPixels() != nullptr) {
        wallpaper_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        wallpaper_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        wallpaper_dsc_.header.w = kWpWidth;
        wallpaper_dsc_.header.h = kWpHeight;
        wallpaper_dsc_.header.stride = kWpWidth * sizeof(uint16_t);
        wallpaper_dsc_.data = wallpaper_pixels_;
        wallpaper_dsc_.data_size = kWpBytes;
        wallpaper_img_ = lv_image_create(overlay_);
        lv_image_set_src(wallpaper_img_, &wallpaper_dsc_);
        lv_obj_align(wallpaper_img_, LV_ALIGN_CENTER, 0, 0);
        has_wallpaper = true;
    }

    // Weather line at the top (filled in by the weather task)
    weather_label_ = lv_label_create(overlay_);
    lv_label_set_text(weather_label_, "");
    lv_obj_set_style_text_font(weather_label_, &font_vi_20, 0);
    lv_obj_set_style_text_color(weather_label_, lv_color_hex(0xE3B341), 0);
    lv_obj_align(weather_label_, LV_ALIGN_TOP_MID, 0, 6);

    // Drop shadow behind the time for legibility over any wallpaper
    if (has_wallpaper) {
        time_shadow_ = lv_label_create(overlay_);
        lv_label_set_text(time_shadow_, "--:--");
        lv_obj_set_style_text_font(time_shadow_, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(time_shadow_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_opa(time_shadow_, LV_OPA_70, 0);
        lv_obj_align(time_shadow_, LV_ALIGN_CENTER, 2, -30);
    }

    time_label_ = lv_label_create(overlay_);
    lv_label_set_text(time_label_, "--:--");
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_hex(accent_color_), 0);
    lv_obj_align(time_label_, LV_ALIGN_CENTER, 0, -32);

    // Solar date — large and clear
    solar_label_ = lv_label_create(overlay_);
    lv_label_set_text(solar_label_, "");
    lv_obj_set_style_text_font(solar_label_, &font_vi_28, 0);
    lv_obj_set_style_text_color(solar_label_, lv_color_hex(0xF0F6FC), 0);
    lv_obj_align(solar_label_, LV_ALIGN_CENTER, 0, 18);

    // Lunar date
    lunar_label_ = lv_label_create(overlay_);
    lv_label_set_text(lunar_label_, "");
    lv_obj_set_style_text_font(lunar_label_, &font_vi_20, 0);
    lv_obj_set_style_text_color(lunar_label_, lv_color_hex(0xA5D6FF), 0);
    lv_obj_align(lunar_label_, LV_ALIGN_CENTER, 0, 52);

    info_label_ = lv_label_create(overlay_);
    auto ip = WifiManager::GetInstance().GetIpAddress();
    char info[96];
    if (ip.empty()) {
        snprintf(info, sizeof(info), "Nói 'Hey Tiger' để trò chuyện");
    } else {
        snprintf(info, sizeof(info), "'Hey Tiger' để trò chuyện | http://%s", ip.c_str());
    }
    lv_obj_set_style_text_font(info_label_, &font_vi_16, 0);
    lv_obj_set_style_text_color(info_label_, lv_color_hex(0x6E7681), 0);
    lv_obj_set_style_text_align(info_label_, LV_TEXT_ALIGN_CENTER, 0);
    // Constrain to the screen width and marquee-scroll right-to-left when the
    // text is wider than the display (fits statically when it isn't).
    lv_obj_set_width(info_label_, kWpWidth - 16);
    lv_label_set_long_mode(info_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(info_label_, info);
    lv_obj_align(info_label_, LV_ALIGN_BOTTOM_MID, 0, -4);

    UpdateWeatherLabel();
    lv_obj_move_foreground(overlay_);
}

void ClockHome::UpdateClock() {
    if (time_label_ == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    if (t.tm_year < 2025 - 1900) {
        lv_label_set_text(time_label_, "--:--");
        if (solar_label_ != nullptr) {
            lv_label_set_text(solar_label_, "Đang đồng bộ giờ...");
        }
        if (lunar_label_ != nullptr) {
            lv_label_set_text(lunar_label_, "");
        }
        return;
    }

    char buf[24];
    if (use_24h_) {
        strftime(buf, sizeof(buf), show_seconds_ ? "%H:%M:%S" : "%H:%M", &t);
    } else {
        strftime(buf, sizeof(buf), show_seconds_ ? "%I:%M:%S %p" : "%I:%M %p", &t);
    }
    lv_label_set_text(time_label_, buf);
    if (time_shadow_ != nullptr) {
        lv_label_set_text(time_shadow_, buf);
    }

    if (solar_label_ != nullptr) {
        if (show_date_) {
            char date[64];
            snprintf(date, sizeof(date), "%s, %02d/%02d/%04d",
                kWeekdays[t.tm_wday], t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            lv_label_set_text(solar_label_, date);
        } else {
            lv_label_set_text(solar_label_, "");
        }
    }

    if (lunar_label_ != nullptr) {
        if (show_date_) {
            int lday = 0, lmonth = 0, lyear = 0;
            bool leap = false;
            Solar2Lunar(t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, 7.0, lday, lmonth, lyear, leap);
            char lunar[64];
            snprintf(lunar, sizeof(lunar), "Âm lịch: %02d/%02d%s", lday, lmonth, leap ? " (nhuận)" : "");
            lv_label_set_text(lunar_label_, lunar);
        } else {
            lv_label_set_text(lunar_label_, "");
        }
    }
}

void ClockHome::UpdateWeatherLabel() {
    if (weather_label_ == nullptr) {
        return;
    }
    std::string text;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_valid_) {
            text = weather_text_;
            if (!weather_city_.empty()) {
                text += "  ";
                text += weather_city_;
            }
        }
    }
    lv_label_set_text(weather_label_, text.c_str());
}

void ClockHome::ClockTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<ClockHome*>(lv_timer_get_user_data(timer));
    self->UpdateClock();
}

void ClockHome::ApplySettings() {
    LoadSettings();
    if (!visible_) {
        return;
    }
    {
        DisplayLockGuard lock(display_);
        // Rebuild the overlay so wallpaper add/remove and color changes all
        // take effect (rotation is already set; keep it)
        if (overlay_ != nullptr) {
            lv_obj_del(overlay_);
            overlay_ = nullptr;
            wallpaper_img_ = nullptr;
            time_shadow_ = nullptr;
            time_label_ = nullptr;
            solar_label_ = nullptr;
            lunar_label_ = nullptr;
            weather_label_ = nullptr;
            info_label_ = nullptr;
        }
        FreeWallpaperPixels();
        CreateUi();
        UpdateClock();
    }
    Board::GetInstance().GetBacklight()->SetBrightness(brightness_);
}

uint8_t* ClockHome::LoadWallpaperPixels() {
    if (wallpaper_pixels_ != nullptr) {
        return wallpaper_pixels_;
    }
    const esp_partition_t* part = WallpaperPartition();
    if (part == nullptr) {
        return nullptr;
    }
    uint8_t header[kWpHeaderBytes];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK) {
        return nullptr;
    }
    uint32_t magic;
    memcpy(&magic, header, sizeof(magic));
    uint16_t w, h;
    memcpy(&w, header + 4, sizeof(w));
    memcpy(&h, header + 6, sizeof(h));
    if (magic != kWpMagic || w != kWpWidth || h != kWpHeight) {
        return nullptr;  // no/invalid wallpaper stored
    }

    auto* pixels = static_cast<uint8_t*>(heap_caps_malloc(kWpBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        ESP_LOGW(TAG, "No PSRAM for wallpaper buffer");
        return nullptr;
    }
    if (esp_partition_read(part, kWpHeaderBytes, pixels, kWpBytes) != ESP_OK) {
        heap_caps_free(pixels);
        return nullptr;
    }
    wallpaper_pixels_ = pixels;
    return wallpaper_pixels_;
}

void ClockHome::FreeWallpaperPixels() {
    if (wallpaper_pixels_ != nullptr) {
        heap_caps_free(wallpaper_pixels_);
        wallpaper_pixels_ = nullptr;
    }
}

bool ClockHome::SaveWallpaper(const uint8_t* jpeg, size_t len) {
    const esp_partition_t* part = WallpaperPartition();
    if (part == nullptr) {
        ESP_LOGE(TAG, "No wallpaper partition");
        return false;
    }

    uint8_t* decoded = nullptr;
    size_t decoded_len = 0, dw = 0, dh = 0, stride = 0;
    esp_err_t ret = jpeg_to_image(jpeg, len, &decoded, &decoded_len, &dw, &dh, &stride);
    if (ret != ESP_OK || decoded == nullptr) {
        ESP_LOGE(TAG, "Wallpaper JPEG decode failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "Wallpaper decoded %ux%u stride=%u", (unsigned)dw, (unsigned)dh, (unsigned)stride);

    // Build the exact 320x240 RGB565 frame (nearest-neighbor; the phone already
    // cropped to 4:3 so this is a light resample at most)
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(kWpBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame == nullptr) {
        heap_caps_free(decoded);
        ESP_LOGE(TAG, "No PSRAM for wallpaper frame");
        return false;
    }
    auto* dst = reinterpret_cast<uint16_t*>(frame);
    for (int y = 0; y < kWpHeight; ++y) {
        int sy = dh > 0 ? static_cast<int>(static_cast<size_t>(y) * dh / kWpHeight) : 0;
        const auto* src_row = reinterpret_cast<const uint16_t*>(decoded + sy * stride);
        for (int x = 0; x < kWpWidth; ++x) {
            int sx = dw > 0 ? static_cast<int>(static_cast<size_t>(x) * dw / kWpWidth) : 0;
            dst[y * kWpWidth + x] = src_row[sx];
        }
    }
    heap_caps_free(decoded);

    // Erase + write header then pixels (1MB partition, well under the budget)
    uint8_t header[kWpHeaderBytes] = {};
    memcpy(header, &kWpMagic, sizeof(kWpMagic));
    uint16_t w = kWpWidth, h = kWpHeight;
    memcpy(header + 4, &w, sizeof(w));
    memcpy(header + 6, &h, sizeof(h));

    size_t erase_size = (kWpHeaderBytes + kWpBytes + part->erase_size - 1) /
        part->erase_size * part->erase_size;
    bool ok = esp_partition_erase_range(part, 0, erase_size) == ESP_OK &&
              esp_partition_write(part, 0, header, sizeof(header)) == ESP_OK &&
              esp_partition_write(part, kWpHeaderBytes, frame, kWpBytes) == ESP_OK;
    heap_caps_free(frame);
    if (!ok) {
        ESP_LOGE(TAG, "Wallpaper flash write failed");
        return false;
    }

    ESP_LOGI(TAG, "Wallpaper saved");
    ApplySettings();  // rebuild UI so the new wallpaper shows immediately
    return true;
}

void ClockHome::StartConfigServer() {
    if (server_ != nullptr) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;  // clock(5) + alarm(3) + headroom
    config.stack_size = 6144;
    config.recv_wait_timeout = 15;  // allow time for the wallpaper upload body

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start clock config server");
        return;
    }

    httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = HandleConfigPage, .user_ctx = this};
    httpd_uri_t get_api = {.uri = "/api/clock", .method = HTTP_GET, .handler = HandleGetClock, .user_ctx = this};
    httpd_uri_t post_api = {.uri = "/api/clock", .method = HTTP_POST, .handler = HandlePostClock, .user_ctx = this};
    httpd_uri_t post_wp = {.uri = "/api/wallpaper", .method = HTTP_POST, .handler = HandlePostWallpaper, .user_ctx = this};
    httpd_uri_t del_wp = {.uri = "/api/wallpaper", .method = HTTP_DELETE, .handler = HandleDeleteWallpaper, .user_ctx = this};
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &get_api);
    httpd_register_uri_handler(server, &post_api);
    httpd_register_uri_handler(server, &post_wp);
    httpd_register_uri_handler(server, &del_wp);

    server_ = server;
    ESP_LOGI(TAG, "Clock config server started on port 80");

    StartWeatherTask();
}

namespace {
// GET a URL into a std::string (small JSON responses). Returns false on error.
bool HttpGetJson(const std::string& url, std::string& out, int connect_id) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(connect_id);
    if (!http) {
        return false;
    }
    http->SetTimeout(10000);
    http->SetHeader("User-Agent", "xiaozhi-clock");
    if (!http->Open("GET", url)) {
        return false;
    }
    if (http->GetStatusCode() != 200) {
        http->Close();
        return false;
    }
    out.clear();
    char buf[512];
    while (true) {
        int r = http->Read(buf, sizeof(buf));
        if (r < 0) {
            http->Close();
            return false;
        }
        if (r == 0) {
            break;
        }
        out.append(buf, r);
        if (out.size() > 8192) {
            break;  // weather/geo responses are small; guard runaway
        }
    }
    http->Close();
    return !out.empty();
}
}  // namespace

void ClockHome::StartWeatherTask() {
    if (weather_task_ != nullptr) {
        return;
    }
    constexpr size_t kStack = 8192;
    if (weather_task_stack_ == nullptr) {
        weather_task_stack_ = static_cast<StackType_t*>(heap_caps_malloc(kStack, MALLOC_CAP_SPIRAM));
    }
    if (weather_task_tcb_ == nullptr) {
        weather_task_tcb_ = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    }
    if (weather_task_stack_ == nullptr || weather_task_tcb_ == nullptr) {
        ESP_LOGE(TAG, "No memory for weather task");
        return;
    }
    weather_task_ = xTaskCreateStatic(WeatherTaskEntry, "clock_weather", kStack, this, 2,
        weather_task_stack_, weather_task_tcb_);
}

void ClockHome::WeatherTaskEntry(void* arg) {
    static_cast<ClockHome*>(arg)->WeatherLoop();
    vTaskDelete(nullptr);
}

void ClockHome::WeatherLoop() {
    // Let WiFi/network settle before the first request
    vTaskDelay(pdMS_TO_TICKS(4000));
    while (true) {
        bool ok = FetchWeatherOnce();
        if (ok && visible_) {
            DisplayLockGuard lock(display_);
            UpdateWeatherLabel();
        }
        // Refresh every 30 min on success, retry sooner on failure
        vTaskDelay(pdMS_TO_TICKS(ok ? 30 * 60 * 1000 : 60 * 1000));
    }
}

bool ClockHome::FetchWeatherOnce() {
    std::string body;
    // 1) Geolocate by public IP (free HTTP endpoint)
    if (!HttpGetJson("http://ip-api.com/json/?fields=status,lat,lon,city", body, 5)) {
        ESP_LOGW(TAG, "Weather: IP geolocation failed");
        return false;
    }
    cJSON* geo = cJSON_Parse(body.c_str());
    if (geo == nullptr) {
        return false;
    }
    cJSON* lat = cJSON_GetObjectItem(geo, "lat");
    cJSON* lon = cJSON_GetObjectItem(geo, "lon");
    cJSON* city = cJSON_GetObjectItem(geo, "city");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(geo);
        return false;
    }
    double latitude = lat->valuedouble;
    double longitude = lon->valuedouble;
    std::string city_name = cJSON_IsString(city) ? city->valuestring : "";
    cJSON_Delete(geo);

    // 2) Current weather from open-meteo (HTTPS, no API key)
    char url[200];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code",
        latitude, longitude);
    if (!HttpGetJson(url, body, 5)) {
        ESP_LOGW(TAG, "Weather: forecast fetch failed");
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* temp = current ? cJSON_GetObjectItem(current, "temperature_2m") : nullptr;
    cJSON* code = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
    if (!cJSON_IsNumber(temp)) {
        cJSON_Delete(root);
        return false;
    }
    int temp_c = static_cast<int>(std::lround(temp->valuedouble));
    int wmo = cJSON_IsNumber(code) ? code->valueint : -1;
    cJSON_Delete(root);

    char text[64];
    const char* desc = WeatherDesc(wmo);
    if (desc[0] != '\0') {
        snprintf(text, sizeof(text), "%d°C  %s", temp_c, desc);
    } else {
        snprintf(text, sizeof(text), "%d°C", temp_c);
    }

    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_text_ = text;
        weather_city_ = city_name;
        weather_valid_ = true;
    }
    ESP_LOGI(TAG, "Weather: %s %s (%.3f,%.3f)", text, city_name.c_str(), latitude, longitude);
    return true;
}
