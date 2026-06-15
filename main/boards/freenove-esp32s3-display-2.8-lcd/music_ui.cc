#include "music_ui.h"

#include "display/display.h"  // DisplayLockGuard

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "MusicUi"

// Vietnamese-capable fonts (shared with ClockHome; defined in font_vi_*.c).
extern "C" {
extern const lv_font_t font_vi_28;
extern const lv_font_t font_vi_20;
extern const lv_font_t font_vi_16;
}

namespace {
constexpr int kArtX = 16;             // left margin of the album art
constexpr int kTextX = kArtX + MusicUi::kArtW + 12;  // text column starts here
constexpr int kTextW = 320 - kTextX - 10;            // remaining width for text
}  // namespace

void MusicUi::Show(const char* title, const char* artists) {
    if (visible_) {
        UpdateText(title, artists);
        return;
    }
    DisplayLockGuard lock(display_);
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);

    overlay_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(overlay_, 0, 0);
    lv_obj_set_style_radius(overlay_, 0, 0);
    lv_obj_set_style_border_width(overlay_, 0, 0);
    lv_obj_set_style_pad_all(overlay_, 0, 0);
    lv_obj_set_style_bg_color(overlay_, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);

    // Album-art placeholder (a rounded dark tile with a note) until the JPEG
    // has been downloaded + decoded.
    art_placeholder_ = lv_obj_create(overlay_);
    lv_obj_set_size(art_placeholder_, kArtW, kArtH);
    lv_obj_set_pos(art_placeholder_, kArtX, (240 - kArtH) / 2);
    lv_obj_set_style_radius(art_placeholder_, 10, 0);
    lv_obj_set_style_border_width(art_placeholder_, 0, 0);
    lv_obj_set_style_bg_color(art_placeholder_, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_bg_opa(art_placeholder_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(art_placeholder_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* note = lv_label_create(art_placeholder_);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &font_vi_28, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0x3FB950), 0);
    lv_obj_center(note);

    // Title (wraps to up to a few lines).
    title_label_ = lv_label_create(overlay_);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title_label_, kTextW);
    lv_obj_set_style_text_font(title_label_, &font_vi_20, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_hex(0xF0F6FC), 0);
    lv_obj_set_pos(title_label_, kTextX, 56);

    // Artist line.
    artist_label_ = lv_label_create(overlay_);
    lv_label_set_long_mode(artist_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(artist_label_, kTextW);
    lv_obj_set_style_text_font(artist_label_, &font_vi_16, 0);
    lv_obj_set_style_text_color(artist_label_, lv_color_hex(0xA5D6FF), 0);
    lv_obj_set_pos(artist_label_, kTextX, 130);

    // Status line at the bottom of the text column.
    status_label_ = lv_label_create(overlay_);
    lv_label_set_text(status_label_, LV_SYMBOL_AUDIO "  Đang phát");
    lv_obj_set_style_text_font(status_label_, &font_vi_16, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0xE3B341), 0);
    lv_obj_set_pos(status_label_, kTextX, 200);

    visible_ = true;
    UpdateText(title, artists);
    lv_obj_move_foreground(overlay_);
    ESP_LOGI(TAG, "Music UI shown");
}

void MusicUi::UpdateText(const char* title, const char* artists) {
    DisplayLockGuard lock(display_);
    if (title_label_ != nullptr) {
        bool empty = (title == nullptr || title[0] == '\0');
        lv_label_set_text(title_label_, empty ? "Đang tìm nhạc..." : title);
    }
    if (artist_label_ != nullptr) {
        lv_label_set_text(artist_label_, artists != nullptr ? artists : "");
    }
}

void MusicUi::SetAlbumArt(uint16_t* pixels, int w, int h) {
    if (pixels == nullptr) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (!visible_ || overlay_ == nullptr) {
        heap_caps_free(pixels);  // overlay gone; drop the buffer
        return;
    }
    FreeArt();
    art_buf_ = pixels;

    art_dsc_ = {};
    art_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    art_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    art_dsc_.header.w = w;
    art_dsc_.header.h = h;
    art_dsc_.header.stride = w * sizeof(uint16_t);
    art_dsc_.data = reinterpret_cast<const uint8_t*>(art_buf_);
    art_dsc_.data_size = static_cast<uint32_t>(w) * h * sizeof(uint16_t);

    if (art_img_ == nullptr) {
        art_img_ = lv_image_create(overlay_);
    }
    lv_image_set_src(art_img_, &art_dsc_);
    lv_obj_set_pos(art_img_, kArtX, (240 - h) / 2);
    if (art_placeholder_ != nullptr) {
        lv_obj_add_flag(art_placeholder_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "Album art set %dx%d", w, h);
}

void MusicUi::Hide() {
    if (!visible_) {
        return;
    }
    {
        DisplayLockGuard lock(display_);
        if (overlay_ != nullptr) {
            lv_obj_del(overlay_);  // deletes the image + labels too
            overlay_ = nullptr;
            art_img_ = nullptr;
            art_placeholder_ = nullptr;
            title_label_ = nullptr;
            artist_label_ = nullptr;
            status_label_ = nullptr;
        }
        lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_0);
        lv_obj_invalidate(lv_screen_active());
    }
    FreeArt();  // the LVGL image is gone now, safe to free the pixels
    visible_ = false;
    ESP_LOGI(TAG, "Music UI hidden");
}

void MusicUi::FreeArt() {
    if (art_buf_ != nullptr) {
        heap_caps_free(art_buf_);
        art_buf_ = nullptr;
    }
}
