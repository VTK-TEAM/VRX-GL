// canvas_renderer.h — малює "екран OSD" (фон-фото + всі елементи) в
// прямокутник canvas_rect_ вікна редактора, letterbox якщо
// співвідношення сторін вікна не збігається з реальним екраном. Для
// кожного елемента дублює диспетчеризацію за типом з osd_source.cpp
// render(), але замість запису у DRM dumb-буфер — SDL_RenderCopy.
//
// VALUE/ENUM_SWITCH/BAR/HORIZON можуть малювати live-значення з
// VtTelemetryStorage (коли він підключений), або fallback-поведінку без даних.
#pragma once

#include "atlas_font.h"
#include "osd_element.h"
#include "image_cache.h"
#include "vt_telemetry_storage.h"
#include <SDL.h>
#include <string>
#include <vector>

namespace osdedit {

struct ElementHitBox {
    std::string key;
    SDL_Rect rect; // екранні координати вікна (вже з offset канви)
};

class CanvasRenderer {
public:
    explicit CanvasRenderer(AtlasFont& font) : font_(font) {}

    void set_canvas_rect(SDL_Rect r) { canvas_rect_ = r; }
    SDL_Rect canvas_rect() const { return canvas_rect_; }

    void set_background(SDL_Texture* tex) { bg_texture_ = tex; }

    // Малює все і повертає hit-list у ПОРЯДКУ малювання (останній
    // елемент — зверху, тож при пошуку кліку треба йти з кінця списку,
    // щоб клікалось на те, що візуально зверху).
    std::vector<ElementHitBox> render(SDL_Renderer* renderer,
                                      const std::vector<OsdElement>& elements,
                                      const std::string& selected_key,
                                      ImageCache& images,
                                      VtTelemetryStorage* storage) const;

    // Переведення координат: клік вікна (пікселі) -> нормалізовані 0..1
    // координати КАНВИ (те, що зберігається як L/T елемента). false —
    // клік поза канвою (в летербоксі чи поза нею взагалі).
    bool screen_to_canvas_norm(int screen_x, int screen_y, float* nx, float* ny) const;
    void canvas_norm_to_screen(float nx, float ny, int* screen_x, int* screen_y) const;

private:
    void draw_element(SDL_Renderer* renderer, const OsdElement& el, bool is_selected,
                      ImageCache& images, std::vector<ElementHitBox>* hits,
                      VtTelemetryStorage* storage) const;

    void draw_placeholder_rect(SDL_Renderer* renderer, SDL_Rect r, const char* label) const;

    AtlasFont& font_;
    SDL_Rect canvas_rect_{0, 0, 0, 0};
    SDL_Texture* bg_texture_ = nullptr;
};

} // namespace osdedit
