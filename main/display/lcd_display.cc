#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings. Default to "dark" so the custom assets dark
    // skin (vivid background color) shows out of the box — a full flash wipes
    // NVS, so a "light" default would hide the custom look every time.
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    for (auto*& buffer : video_dma_buffers_) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
    video_dma_buffer_size_ = 0;
    if (video_previous_rgb332_ != nullptr) {
        heap_caps_free(video_previous_rgb332_);
        video_previous_rgb332_ = nullptr;
    }
    video_previous_rgb332_size_ = 0;
    video_previous_rgb332_valid_ = false;
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

// Strip height for raw video DMA transfers. 8 rows keeps the two ping-pong
// buffers at 2 x (320*8*2) = 10 KB of internal DMA RAM — internal memory gets
// tight when WiFi streaming and I2S audio playback run at the same time.
static constexpr int kVideoStripHeight = 8;
static constexpr int kVideoAudioIconSize = 28;

bool LcdDisplay::PreallocateVideoStripBuffers() {
    size_t strip_size = width_ * kVideoStripHeight * sizeof(uint16_t);
    if (video_dma_buffer_size_ >= strip_size) {
        return true;
    }
    for (auto*& buffer : video_dma_buffers_) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
    video_dma_buffer_size_ = 0;
    for (auto*& buffer : video_dma_buffers_) {
        buffer = static_cast<uint8_t*>(heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (buffer == nullptr) {
            ESP_LOGW(TAG, "Preallocating video DMA strips failed, need=%u bytes, largest_dma_block=%u",
                static_cast<unsigned>(strip_size),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)));
            return false;
        }
    }
    video_dma_buffer_size_ = strip_size;
    if (video_icon_dma_buffer_ == nullptr) {
        video_icon_dma_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(kVideoAudioIconSize * kVideoAudioIconSize * sizeof(uint16_t),
                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    ESP_LOGI(TAG, "Preallocated video DMA strips: %u bytes each", static_cast<unsigned>(strip_size));
    return true;
}

void LcdDisplay::SetVideoAudioIcon(int state) {
    video_audio_icon_state_ = state;
    // Force the render task to redraw the letterbox (and the icon with it) on
    // its next frame. Drawing here directly would race the SPI strip transfers
    // issued by the video render task.
    video_letterbox_cleared_ = false;
}

void LcdDisplay::DrawVideoAudioIcon(int src_h) {
    int state = video_audio_icon_state_;
    if (state < 0 || panel_ == nullptr) {
        return;
    }

    constexpr int kIconSize = kVideoAudioIconSize;
    int y_offset = (height_ - src_h) / 2;
    if (y_offset < kIconSize + 2) {
        return;  // No letterbox band to draw into
    }

    size_t icon_bytes = kIconSize * kIconSize * sizeof(uint16_t);
    if (video_icon_dma_buffer_ == nullptr) {
        video_icon_dma_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(icon_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    auto* icon = reinterpret_cast<uint16_t*>(video_icon_dma_buffer_);
    if (icon == nullptr) {
        return;
    }

    auto px = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
        uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        return __builtin_bswap16(c);
    };
    const uint16_t bg = px(40, 40, 48);
    const uint16_t fg = px(255, 255, 255);
    const uint16_t accent = state == 1 ? px(64, 220, 96) : px(235, 64, 64);

    for (size_t i = 0; i < static_cast<size_t>(kIconSize) * kIconSize; ++i) {
        icon[i] = bg;
    }
    auto set = [&](int x, int y, uint16_t c) {
        if (x >= 0 && x < kIconSize && y >= 0 && y < kIconSize) {
            icon[y * kIconSize + x] = c;
        }
    };

    // Speaker body and cone
    for (int y = 10; y <= 17; ++y) {
        for (int x = 4; x <= 8; ++x) {
            set(x, y, fg);
        }
    }
    for (int x = 9; x <= 13; ++x) {
        int spread = x - 8;
        for (int y = 10 - spread; y <= 17 + spread; ++y) {
            set(x, y, fg);
        }
    }

    if (state == 1) {
        // Sound waves
        for (int y = 11; y <= 16; ++y) {
            set(17, y, accent);
        }
        for (int y = 8; y <= 19; ++y) {
            set(20, y, accent);
            set(21, y, accent);
        }
    } else {
        // Mute cross
        for (int i = 0; i <= 8; ++i) {
            set(16 + i, 9 + i, accent);
            set(17 + i, 9 + i, accent);
            set(24 - i, 9 + i, accent);
            set(25 - i, 9 + i, accent);
        }
    }

    int x0 = width_ - kIconSize - 4;
    int y0 = (y_offset - kIconSize) / 2;
    esp_lcd_panel_draw_bitmap(panel_, x0, y0, x0 + kIconSize, y0 + kIconSize, icon);
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_image_set_scale(emoji_image_, 96);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_RIGHT, -lvgl_theme->spacing(4),
        text_font->line_height + lvgl_theme->spacing(2));
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_IGNORE_LAYOUT);

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_align(emoji_label_, LV_ALIGN_TOP_RIGHT, -lvgl_theme->spacing(4),
        text_font->line_height + lvgl_theme->spacing(2));
    lv_obj_set_style_text_font(emoji_label_, icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_IGNORE_LAYOUT);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
        }
        return;
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide floating emotion indicators once chat content is present.
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetVideoFrame(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        if (video_overlay_active_) {
            preview_image_cached_.reset();
            return;
        }
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    esp_timer_stop(preview_timer_);
    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        int scale_x = 256 * width_ / img_dsc->header.w;
        int scale_y = 256 * height_ / img_dsc->header.h;
        lv_image_set_scale(preview_image_, std::min(scale_x, scale_y));
    }

    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::DrawRgb565FrameDirect(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return;
    }

    int dst_w = width_;
    int dst_h = src_h * dst_w / src_w;
    if (dst_h > height_) {
        dst_h = height_;
        dst_w = src_w * dst_h / src_h;
    }
    dst_w = std::max(1, dst_w);
    dst_h = std::max(1, dst_h);

    size_t out_len = dst_w * dst_h * sizeof(uint16_t);
    uint16_t* out = static_cast<uint16_t*>(heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (out == nullptr) {
        out = static_cast<uint16_t*>(heap_caps_malloc(out_len, MALLOC_CAP_8BIT));
    }
    if (out == nullptr) {
        ESP_LOGW(TAG, "No memory for direct RGB565 frame");
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        int src_y = y * src_h / dst_h;
        const uint16_t* src_line = reinterpret_cast<const uint16_t*>(data + src_y * src_stride);
        uint16_t* dst_line = out + y * dst_w;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = x * src_w / dst_w;
            dst_line[x] = src_line[src_x];
        }
    }

    int x0 = (width_ - dst_w) / 2;
    int y0 = (height_ - dst_h) / 2;
    {
        DisplayLockGuard lock(this);
        esp_timer_stop(preview_timer_);
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        esp_lcd_panel_draw_bitmap(panel_, x0, y0, x0 + dst_w, y0 + dst_h, out);
    }

    heap_caps_free(out);
}

bool LcdDisplay::DrawRgb565FrameDirectFast(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return false;
    }

    if (src_w != width_ || src_h <= 0 || src_h > height_ || src_stride != width_ * static_cast<int>(sizeof(uint16_t))) {
        DrawRgb565FrameDirect(data, src_w, src_h, src_stride);
        return true;
    }

    DisplayLockGuard lock(this);
    esp_timer_stop(preview_timer_);
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (preview_image_ != nullptr) {
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    ClearVideoLetterbox(src_h);

    constexpr int kStripHeight = kVideoStripHeight;
    size_t strip_size = width_ * kStripHeight * sizeof(uint16_t);
    if (video_dma_buffer_size_ < strip_size) {
        for (auto*& buffer : video_dma_buffers_) {
            if (buffer != nullptr) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        video_dma_buffer_size_ = 0;
        for (auto*& buffer : video_dma_buffers_) {
            buffer = static_cast<uint8_t*>(heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (buffer == nullptr) {
                ESP_LOGW(TAG, "No DMA memory for fast video strip, need=%u bytes", static_cast<unsigned>(strip_size));
                return false;
            }
        }
        video_dma_buffer_size_ = strip_size;
        ESP_LOGI(TAG, "Allocated fast video DMA strips: %u bytes each", static_cast<unsigned>(strip_size));
    }

    // Start on [1]: ClearVideoLetterbox may still have a DMA transfer queued
    // from the borrowed buffer [0]
    int buffer_index = 1;
    int y_offset = (height_ - src_h) / 2;
    for (int y = 0; y < src_h; y += kStripHeight) {
        int strip_h = std::min(kStripHeight, src_h - y);
        size_t current_size = width_ * strip_h * sizeof(uint16_t);
        uint8_t* dst = video_dma_buffers_[buffer_index];
        const uint8_t* src = data + y * src_stride;
        memcpy(dst, src, current_size);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_, 0, y_offset + y, width_, y_offset + y + strip_h, dst);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Fast video strip draw failed at y=%d: %s", y, esp_err_to_name(ret));
            return false;
        }
        buffer_index ^= 1;
        if (video_overlay_active_ && ((y / kStripHeight) % 4) == 3) {
            taskYIELD();
        }
    }
    return true;
}

bool LcdDisplay::DrawRgb332FrameDirectFast(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return false;
    }

    if (src_w != width_ || src_h <= 0 || src_h > height_ || src_stride != width_) {
        return false;
    }

    bool locked = false;
    if (!video_overlay_active_) {
        if (!Lock(100)) {
            ESP_LOGW(TAG, "RGB332 draw skipped: LVGL lock busy");
            return false;
        }
        locked = true;
        esp_timer_stop(preview_timer_);
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (preview_image_ != nullptr) {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static uint16_t rgb332_to_panel565[256];
    static bool rgb332_table_ready = false;
    if (!rgb332_table_ready) {
        for (int p = 0; p < 256; ++p) {
            uint8_t r3 = (p >> 5) & 0x07;
            uint8_t g3 = (p >> 2) & 0x07;
            uint8_t b2 = p & 0x03;
            uint16_t r5 = (r3 << 2) | (r3 >> 1);
            uint16_t g6 = (g3 << 3) | g3;
            uint16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
            uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;
            rgb332_to_panel565[p] = __builtin_bswap16(rgb565);
        }
        rgb332_table_ready = true;
    }

    constexpr int kBlockHeight = kVideoStripHeight;
    size_t strip_size = width_ * kBlockHeight * sizeof(uint16_t);
    if (video_dma_buffer_size_ < strip_size) {
        for (auto*& buffer : video_dma_buffers_) {
            if (buffer != nullptr) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        video_dma_buffer_size_ = 0;
        for (auto*& buffer : video_dma_buffers_) {
            buffer = static_cast<uint8_t*>(heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (buffer == nullptr) {
                ESP_LOGW(TAG, "No DMA memory for RGB332 video strip, need=%u bytes", static_cast<unsigned>(strip_size));
                if (locked) {
                    Unlock();
                }
                return false;
            }
        }
        video_dma_buffer_size_ = strip_size;
        ESP_LOGI(TAG, "Allocated RGB332 video DMA strips: %u bytes each", static_cast<unsigned>(strip_size));
    }

    size_t frame_size = src_stride * src_h;
    bool use_dirty_cache = true;
    if (use_dirty_cache && video_previous_rgb332_size_ < frame_size) {
        if (video_previous_rgb332_ != nullptr) {
            heap_caps_free(video_previous_rgb332_);
            video_previous_rgb332_ = nullptr;
        }
        video_previous_rgb332_size_ = 0;
        video_previous_rgb332_valid_ = false;
        video_previous_rgb332_ = static_cast<uint8_t*>(heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (video_previous_rgb332_ == nullptr) {
            video_previous_rgb332_ = static_cast<uint8_t*>(heap_caps_malloc(frame_size, MALLOC_CAP_8BIT));
        }
        if (video_previous_rgb332_ == nullptr) {
            ESP_LOGW(TAG, "No memory for previous RGB332 frame");
            if (locked) {
                Unlock();
            }
            return false;
        }
        video_previous_rgb332_size_ = frame_size;
    }

    // Start on [1]: ClearVideoLetterbox may still have a DMA transfer queued
    // from the borrowed buffer [0]
    int buffer_index = 1;
    int y_offset = (height_ - src_h) / 2;
    ClearVideoLetterbox(src_h);
    int dirty_bands = 0;
    int dirty_pixels = 0;
    static uint32_t overlay_frame_counter = 0;
    if (!video_overlay_active_) {
        overlay_frame_counter = 0;
    } else {
        overlay_frame_counter++;
    }
    bool force_full = !use_dirty_cache || !video_previous_rgb332_valid_ ||
        (video_overlay_active_ && (overlay_frame_counter % 30) == 0);

    for (int y = 0; y < src_h; y += kBlockHeight) {
        int block_h = std::min(kBlockHeight, src_h - y);
        bool dirty = force_full;
        if (use_dirty_cache && !dirty) {
            const size_t row_bytes = static_cast<size_t>(src_w);
            for (int row = 0; row < block_h; ++row) {
                const uint8_t* cur = data + (y + row) * src_stride;
                const uint8_t* prev = video_previous_rgb332_ + (y + row) * src_stride;
                if (memcmp(cur, prev, row_bytes) != 0) {
                    dirty = true;
                    break;
                }
            }
        }

        if (!dirty) {
            continue;
        }

        auto* dst = reinterpret_cast<uint16_t*>(video_dma_buffers_[buffer_index]);
        int out = 0;
        for (int row = 0; row < block_h; ++row) {
            const uint8_t* src = data + (y + row) * src_stride;
            for (int col = 0; col < src_w; ++col) {
                dst[out++] = rgb332_to_panel565[src[col]];
            }
        }

        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_, 0, y_offset + y, src_w, y_offset + y + block_h, dst);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "RGB332 band draw failed at y=%d h=%d: %s",
                y, block_h, esp_err_to_name(ret));
            if (locked) {
                Unlock();
            }
            return false;
        }
        dirty_bands++;
        dirty_pixels += src_w * block_h;
        buffer_index ^= 1;
        if (video_overlay_active_ && (dirty_bands % 4) == 0) {
            taskYIELD();
        }
    }

    if (use_dirty_cache) {
        memcpy(video_previous_rgb332_, data, frame_size);
        video_previous_rgb332_valid_ = true;
    }
    if (dirty_bands == 0) {
        if (locked) {
            Unlock();
        }
        return true;
    }
    if (force_full || dirty_pixels < (src_w * src_h)) {
        ESP_LOGD(TAG, "RGB332 dirty draw: bands=%d pixels=%d/%d", dirty_bands, dirty_pixels, src_w * src_h);
    }
    if (locked) {
        Unlock();
    }
    return true;
}

void LcdDisplay::ResetVideoDirtyCache() {
    video_previous_rgb332_valid_ = false;
}

void LcdDisplay::ClearVideoLetterbox(int src_h) {
    if (video_letterbox_cleared_ || panel_ == nullptr || src_h <= 0 || src_h >= height_) {
        video_letterbox_cleared_ = true;
        return;
    }

    int y_offset = (height_ - src_h) / 2;
    if (y_offset <= 0) {
        video_letterbox_cleared_ = true;
        return;
    }

    constexpr int kClearStripHeight = 8;
    size_t clear_size = width_ * kClearStripHeight * sizeof(uint16_t);
    // Borrow a preallocated video strip buffer: while streaming with audio
    // there may be no spare internal DMA block left to allocate
    bool borrowed = video_dma_buffer_size_ >= clear_size && video_dma_buffers_[0] != nullptr;
    auto* clear = borrowed ? video_dma_buffers_[0]
        : static_cast<uint8_t*>(heap_caps_malloc(clear_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (clear == nullptr) {
        // Give up instead of retrying (and logging) on every frame
        video_letterbox_cleared_ = true;
        ESP_LOGW(TAG, "No DMA memory to clear video letterbox, need=%u bytes",
            static_cast<unsigned>(clear_size));
        return;
    }

    memset(clear, 0, clear_size);
    auto clear_range = [&](int y0, int y1) {
        for (int y = y0; y < y1; y += kClearStripHeight) {
            int strip_h = std::min(kClearStripHeight, y1 - y);
            esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + strip_h, clear);
        }
    };
    clear_range(0, y_offset);
    clear_range(y_offset + src_h, height_);
    if (!borrowed) {
        heap_caps_free(clear);
    }
    DrawVideoAudioIcon(src_h);
    video_letterbox_cleared_ = true;
}

void LcdDisplay::SetRawPanelGeometry(int width, int height, bool mirror_x, bool mirror_y, bool swap_xy) {
    if (panel_ == nullptr || width <= 0 || height <= 0) {
        return;
    }
    DisplayLockGuard lock(this);
    width_ = width;
    height_ = height;
    video_previous_rgb332_valid_ = false;
    video_letterbox_cleared_ = false;
    esp_lcd_panel_swap_xy(panel_, swap_xy);
    esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);
    ESP_LOGI(TAG, "Raw panel geometry: %dx%d mirror=(%d,%d) swap_xy=%d",
        width_, height_, mirror_x, mirror_y, swap_xy);
}

void LcdDisplay::SetVideoOverlayActive(bool active) {
    if (!active && lvgl_stopped_for_video_) {
        lvgl_port_resume();
        lvgl_stopped_for_video_ = false;
        ESP_LOGI(TAG, "LVGL resumed after video overlay");
    }

    LvglDisplay::SetVideoOverlayActive(active);
    if (!setup_ui_called_) {
        return;
    }

    {
        DisplayLockGuard lock(this);
        auto set_hidden = [active](lv_obj_t* obj) {
            if (obj == nullptr) {
                return;
            }
            if (active) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            }
        };

        set_hidden(top_bar_);
        set_hidden(status_bar_);
        set_hidden(content_);
        set_hidden(side_bar_);
        set_hidden(bottom_bar_);
        set_hidden(emoji_box_);
        set_hidden(emoji_label_);
        set_hidden(emoji_image_);
        set_hidden(preview_image_);
        if (low_battery_popup_ != nullptr) {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
        if (active && panel_ != nullptr) {
            video_previous_rgb332_valid_ = false;
            video_letterbox_cleared_ = false;
            constexpr int kClearStripHeight = 12;
            size_t clear_size = width_ * kClearStripHeight * sizeof(uint16_t);
            auto* clear = static_cast<uint8_t*>(heap_caps_malloc(clear_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (clear != nullptr) {
                memset(clear, 0, clear_size);
                for (int y = 0; y < height_; y += kClearStripHeight) {
                    int strip_h = std::min(kClearStripHeight, height_ - y);
                    esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + strip_h, clear);
                }
                heap_caps_free(clear);
            }
        }
        if (!active) {
            preview_image_cached_.reset();
            video_previous_rgb332_valid_ = false;
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(nullptr);
        }
    }

    if (active && !lvgl_stopped_for_video_) {
        lvgl_port_stop();
        lvgl_stopped_for_video_ = true;
        ESP_LOGI(TAG, "LVGL stopped for video overlay");
    }
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() was called but label not created)", role, content);
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::ClearVideoLetterbox(int src_h) {
    if (video_letterbox_cleared_ || panel_ == nullptr || src_h <= 0 || src_h >= height_) {
        video_letterbox_cleared_ = true;
        return;
    }

    int y_offset = (height_ - src_h) / 2;
    if (y_offset <= 0) {
        video_letterbox_cleared_ = true;
        return;
    }

    constexpr int kClearStripHeight = 8;
    size_t clear_size = width_ * kClearStripHeight * sizeof(uint16_t);
    // Borrow a preallocated video strip buffer: while streaming with audio
    // there may be no spare internal DMA block left to allocate
    bool borrowed = video_dma_buffer_size_ >= clear_size && video_dma_buffers_[0] != nullptr;
    auto* clear = borrowed ? video_dma_buffers_[0]
        : static_cast<uint8_t*>(heap_caps_malloc(clear_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (clear == nullptr) {
        // Give up instead of retrying (and logging) on every frame
        video_letterbox_cleared_ = true;
        ESP_LOGW(TAG, "No DMA memory to clear video letterbox, need=%u bytes",
            static_cast<unsigned>(clear_size));
        return;
    }

    memset(clear, 0, clear_size);
    auto clear_range = [&](int y0, int y1) {
        for (int y = y0; y < y1; y += kClearStripHeight) {
            int strip_h = std::min(kClearStripHeight, y1 - y);
            esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + strip_h, clear);
        }
    };
    clear_range(0, y_offset);
    clear_range(y_offset + src_h, height_);
    if (!borrowed) {
        heap_caps_free(clear);
    }
    DrawVideoAudioIcon(src_h);
    video_letterbox_cleared_ = true;
}

void LcdDisplay::SetVideoFrame(std::unique_ptr<LvglImage> image) {
    if (image == nullptr) {
        preview_image_cached_.reset();
        return;
    }
    SetPreviewImage(std::move(image));
}

void LcdDisplay::DrawRgb565FrameDirect(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return;
    }

    int dst_w = width_;
    int dst_h = src_h * dst_w / src_w;
    if (dst_h > height_) {
        dst_h = height_;
        dst_w = src_w * dst_h / src_h;
    }
    dst_w = std::max(1, dst_w);
    dst_h = std::max(1, dst_h);

    size_t out_len = dst_w * dst_h * sizeof(uint16_t);
    uint16_t* out = static_cast<uint16_t*>(heap_caps_malloc(out_len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (out == nullptr) {
        out = static_cast<uint16_t*>(heap_caps_malloc(out_len, MALLOC_CAP_8BIT));
    }
    if (out == nullptr) {
        ESP_LOGW(TAG, "No memory for direct RGB565 frame");
        return;
    }

    for (int y = 0; y < dst_h; ++y) {
        int src_y = y * src_h / dst_h;
        const uint16_t* src_line = reinterpret_cast<const uint16_t*>(data + src_y * src_stride);
        uint16_t* dst_line = out + y * dst_w;
        for (int x = 0; x < dst_w; ++x) {
            int src_x = x * src_w / dst_w;
            dst_line[x] = src_line[src_x];
        }
    }

    DisplayLockGuard lock(this);
    int x0 = (width_ - dst_w) / 2;
    int y0 = (height_ - dst_h) / 2;
    esp_lcd_panel_draw_bitmap(panel_, x0, y0, x0 + dst_w, y0 + dst_h, out);
    heap_caps_free(out);
}

bool LcdDisplay::DrawRgb565FrameDirectFast(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return false;
    }

    if (src_w != width_ || src_h <= 0 || src_h > height_ || src_stride != width_ * static_cast<int>(sizeof(uint16_t))) {
        DrawRgb565FrameDirect(data, src_w, src_h, src_stride);
        return true;
    }

    // Hold the LVGL port lock for the whole frame: the LVGL flush task and
    // this raw draw path share one SPI panel, and concurrent access asserts
    // inside the SPI driver (crash decoded at spi_device_release_bus). The
    // lock is uncontended while LVGL is stopped for the video overlay.
    DisplayLockGuard lock(this);

    constexpr int kStripHeight = kVideoStripHeight;
    size_t strip_size = width_ * kStripHeight * sizeof(uint16_t);
    if (video_dma_buffer_size_ < strip_size) {
        for (auto*& buffer : video_dma_buffers_) {
            if (buffer != nullptr) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        video_dma_buffer_size_ = 0;
        for (auto*& buffer : video_dma_buffers_) {
            buffer = static_cast<uint8_t*>(heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (buffer == nullptr) {
                ESP_LOGW(TAG, "No DMA memory for fast RGB565 video strip, need=%u bytes", static_cast<unsigned>(strip_size));
                return false;
            }
        }
        video_dma_buffer_size_ = strip_size;
        ESP_LOGI(TAG, "Allocated fast RGB565 video DMA strips: %u bytes each", static_cast<unsigned>(strip_size));
    }

    // Start on [1]: ClearVideoLetterbox may still have a DMA transfer queued
    // from the borrowed buffer [0]
    int buffer_index = 1;
    int y_offset = (height_ - src_h) / 2;
    ClearVideoLetterbox(src_h);
    for (int y = 0; y < src_h; y += kStripHeight) {
        int strip_h = std::min(kStripHeight, src_h - y);
        size_t current_size = width_ * strip_h * sizeof(uint16_t);
        uint8_t* dst = video_dma_buffers_[buffer_index];
        const uint8_t* src = data + y * src_stride;
        memcpy(dst, src, current_size);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_, 0, y_offset + y, width_, y_offset + y + strip_h, dst);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Fast RGB565 video strip draw failed at y=%d: %s", y, esp_err_to_name(ret));
            return false;
        }
        buffer_index ^= 1;
        if (video_overlay_active_ && ((y / kStripHeight) % 4) == 3) {
            taskYIELD();
        }
    }
    return true;
}

bool LcdDisplay::DrawRgb332FrameDirectFast(const uint8_t* data, int src_w, int src_h, int src_stride) {
    if (data == nullptr || src_w <= 0 || src_h <= 0 || src_stride <= 0 || panel_ == nullptr) {
        return false;
    }

    if (src_w != width_ || src_h <= 0 || src_h > height_ || src_stride != width_) {
        return false;
    }

    // Serialize against the LVGL flush task — shared SPI panel, see
    // DrawRgb565FrameDirectFast
    DisplayLockGuard lock(this);

    static uint16_t rgb332_to_panel565[256];
    static bool rgb332_table_ready = false;
    if (!rgb332_table_ready) {
        for (int p = 0; p < 256; ++p) {
            uint8_t r3 = (p >> 5) & 0x07;
            uint8_t g3 = (p >> 2) & 0x07;
            uint8_t b2 = p & 0x03;
            uint16_t r5 = (r3 << 2) | (r3 >> 1);
            uint16_t g6 = (g3 << 3) | g3;
            uint16_t b5 = (b2 << 3) | (b2 << 1) | (b2 >> 1);
            uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;
            rgb332_to_panel565[p] = __builtin_bswap16(rgb565);
        }
        rgb332_table_ready = true;
    }

    constexpr int kBlockHeight = kVideoStripHeight;
    size_t strip_size = width_ * kBlockHeight * sizeof(uint16_t);
    if (video_dma_buffer_size_ < strip_size) {
        for (auto*& buffer : video_dma_buffers_) {
            if (buffer != nullptr) {
                heap_caps_free(buffer);
                buffer = nullptr;
            }
        }
        video_dma_buffer_size_ = 0;
        for (auto*& buffer : video_dma_buffers_) {
            buffer = static_cast<uint8_t*>(heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (buffer == nullptr) {
                ESP_LOGW(TAG, "No DMA memory for RGB332 video strip, need=%u bytes", static_cast<unsigned>(strip_size));
                return false;
            }
        }
        video_dma_buffer_size_ = strip_size;
        ESP_LOGI(TAG, "Allocated RGB332 video DMA strips: %u bytes each", static_cast<unsigned>(strip_size));
    }

    size_t frame_size = src_stride * src_h;
    bool use_dirty_cache = true;
    if (use_dirty_cache && video_previous_rgb332_size_ < frame_size) {
        if (video_previous_rgb332_ != nullptr) {
            heap_caps_free(video_previous_rgb332_);
            video_previous_rgb332_ = nullptr;
        }
        video_previous_rgb332_size_ = 0;
        video_previous_rgb332_valid_ = false;
        video_previous_rgb332_ = static_cast<uint8_t*>(heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (video_previous_rgb332_ == nullptr) {
            video_previous_rgb332_ = static_cast<uint8_t*>(heap_caps_malloc(frame_size, MALLOC_CAP_8BIT));
        }
        if (video_previous_rgb332_ == nullptr) {
            ESP_LOGW(TAG, "No memory for previous RGB332 frame");
            return false;
        }
        video_previous_rgb332_size_ = frame_size;
    }

    // Start on [1]: ClearVideoLetterbox may still have a DMA transfer queued
    // from the borrowed buffer [0]
    int buffer_index = 1;
    int y_offset = (height_ - src_h) / 2;
    ClearVideoLetterbox(src_h);
    static uint32_t overlay_frame_counter = 0;
    if (!video_overlay_active_) {
        overlay_frame_counter = 0;
    } else {
        overlay_frame_counter++;
    }
    bool force_full = !use_dirty_cache || !video_previous_rgb332_valid_ ||
        (video_overlay_active_ && (overlay_frame_counter % 30) == 0);

    for (int y = 0; y < src_h; y += kBlockHeight) {
        int block_h = std::min(kBlockHeight, src_h - y);
        bool dirty = force_full;
        if (use_dirty_cache && !dirty) {
            const size_t row_bytes = static_cast<size_t>(src_w);
            for (int row = 0; row < block_h; ++row) {
                const uint8_t* cur = data + (y + row) * src_stride;
                const uint8_t* prev = video_previous_rgb332_ + (y + row) * src_stride;
                if (memcmp(cur, prev, row_bytes) != 0) {
                    dirty = true;
                    break;
                }
            }
        }
        if (!dirty) {
            continue;
        }

        auto* dst = reinterpret_cast<uint16_t*>(video_dma_buffers_[buffer_index]);
        int out = 0;
        for (int row = 0; row < block_h; ++row) {
            const uint8_t* src = data + (y + row) * src_stride;
            for (int col = 0; col < src_w; ++col) {
                dst[out++] = rgb332_to_panel565[src[col]];
            }
        }

        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_, 0, y_offset + y, src_w, y_offset + y + block_h, dst);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "RGB332 band draw failed at y=%d h=%d: %s", y, block_h, esp_err_to_name(ret));
            return false;
        }
        buffer_index ^= 1;
        if (video_overlay_active_ && ((y / kBlockHeight) % 4) == 3) {
            taskYIELD();
        }
    }

    if (use_dirty_cache) {
        memcpy(video_previous_rgb332_, data, frame_size);
        video_previous_rgb332_valid_ = true;
    }
    return true;
}

void LcdDisplay::ResetVideoDirtyCache() {
    video_previous_rgb332_valid_ = false;
}

void LcdDisplay::SetRawPanelGeometry(int width, int height, bool mirror_x, bool mirror_y, bool swap_xy) {
    if (panel_ == nullptr || width <= 0 || height <= 0) {
        return;
    }
    DisplayLockGuard lock(this);
    width_ = width;
    height_ = height;
    video_previous_rgb332_valid_ = false;
    video_letterbox_cleared_ = false;
    esp_lcd_panel_swap_xy(panel_, swap_xy);
    esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);
    ESP_LOGI(TAG, "Raw panel geometry: %dx%d mirror=(%d,%d) swap_xy=%d",
        width_, height_, mirror_x, mirror_y, swap_xy);
}

void LcdDisplay::SetVideoOverlayActive(bool active) {
    LvglDisplay::SetVideoOverlayActive(active);
    if (!setup_ui_called_) {
        return;
    }
    DisplayLockGuard lock(this);
    if (active) {
        video_previous_rgb332_valid_ = false;
        video_letterbox_cleared_ = false;
    }
    auto set_hidden = [active](lv_obj_t* obj) {
        if (obj == nullptr) {
            return;
        }
        if (active) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    };
    set_hidden(top_bar_);
    set_hidden(status_bar_);
    set_hidden(content_);
    set_hidden(bottom_bar_);
    set_hidden(emoji_label_);
    set_hidden(emoji_image_);
    set_hidden(preview_image_);
    if (!active) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(nullptr);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (emoji_image_ != nullptr) {
        lv_image_set_scale(emoji_image_, 96);
        lv_obj_align(emoji_image_, LV_ALIGN_TOP_RIGHT, -4, 18);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_align(emoji_label_, LV_ALIGN_TOP_RIGHT, -4, 18);
    }

    // In WeChat message style, keep emotion indicators out of the message area.
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
