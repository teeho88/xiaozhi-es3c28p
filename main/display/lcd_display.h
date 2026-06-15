#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    uint8_t* video_dma_buffers_[2] = {nullptr, nullptr};
    size_t video_dma_buffer_size_ = 0;
    uint8_t* video_icon_dma_buffer_ = nullptr;
    uint8_t* video_previous_rgb332_ = nullptr;
    size_t video_previous_rgb332_size_ = 0;
    bool video_previous_rgb332_valid_ = false;
    bool video_letterbox_cleared_ = false;
    bool lvgl_stopped_for_video_ = false;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    // -1 = hidden, 0 = muted, 1 = audio on. Drawn into the top letterbox while
    // the raw video overlay is active (LVGL is stopped there).
    std::atomic<int> video_audio_icon_state_{-1};

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;
    void ClearVideoLetterbox(int src_h);
    void DrawVideoAudioIcon(int src_h);

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetVideoFrame(std::unique_ptr<LvglImage> image);
    virtual void DrawRgb565FrameDirect(const uint8_t* data, int width, int height, int stride);
    virtual bool DrawRgb565FrameDirectFast(const uint8_t* data, int width, int height, int stride);
    virtual bool DrawRgb332FrameDirectFast(const uint8_t* data, int width, int height, int stride);
    virtual void ResetVideoDirtyCache();
    virtual void SetVideoAudioIcon(int state);
    bool PreallocateVideoStripBuffers();
    virtual void SetRawPanelGeometry(int width, int height, bool mirror_x, bool mirror_y, bool swap_xy);
    virtual void SetVideoOverlayActive(bool active) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
