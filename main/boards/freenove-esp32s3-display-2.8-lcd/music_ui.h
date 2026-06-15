#ifndef MUSIC_UI_H
#define MUSIC_UI_H

#include <lvgl.h>

#include <cstdint>
#include <string>

class Display;

// Full-screen "now playing" overlay shown while Zing music plays: square album
// art on the left, song title + artist + status on the right (landscape 320x240,
// same orientation as ClockHome). All LVGL work runs under DisplayLockGuard, so
// it is safe to drive from the board's touch/poll task. Album art pixels are
// RGB565, adopted (owned) by this object and freed on Hide()/replacement.
class MusicUi {
public:
    static constexpr int kArtW = 180;
    static constexpr int kArtH = 180;

    explicit MusicUi(Display* display) : display_(display) {}
    ~MusicUi() { Hide(); }

    void Show(const char* title, const char* artists);
    void UpdateText(const char* title, const char* artists);
    // Adopts `pixels` (kArtW*kArtH RGB565, heap_caps-allocated). w/h are the
    // actual buffer dimensions; pass kArtW/kArtH.
    void SetAlbumArt(uint16_t* pixels, int w, int h);
    void Hide();
    bool IsVisible() const { return visible_; }

private:
    void FreeArt();

    Display* display_;
    bool visible_ = false;

    lv_obj_t* overlay_ = nullptr;
    lv_obj_t* art_img_ = nullptr;
    lv_obj_t* art_placeholder_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* artist_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;

    lv_img_dsc_t art_dsc_ = {};
    uint16_t* art_buf_ = nullptr;
};

#endif  // MUSIC_UI_H
