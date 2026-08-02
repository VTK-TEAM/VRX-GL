// atlas_font.h — завантажує atlas.png + osd_glyph_info.bin (той самий
// вивід osd_atlas_builder.cpp, що споживає прошивка) і малює текст/
// іконки через SDL_Texture. Хеш-формула та формат коду гліфа —
// БІТ-В-БІТ копія з osd_source.cpp (find_glyph/draw_text/draw_icon),
// щоб WYSIWYG у редакторі дійсно відповідав тому, що покаже прошивка.
#pragma once

#include <SDL.h>
#include <SDL_image.h>
#include <cstdint>
#include "osd_types.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cctype>

namespace osdedit {

constexpr uint32_t SIZE_STEP = 0x1000u;
constexpr uint32_t ICON_HASH_SPACE = 0x1000u;
constexpr uint32_t CUSTOM_ICON_BASE = 0xE000u;

inline uint32_t simple_name_hash(const std::string& name) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        hash ^= static_cast<unsigned char>(std::tolower(c));
        hash *= 16777619u;
    }
    return hash % ICON_HASH_SPACE;
}

// БІНАРНО збігається з OsdGlyphInfo (osd_types.h) / osd_glyph_info
// (osd_atlas_builder.cpp) — 4+4*4=20 байт на запис.
struct GlyphInfo {
    uint32_t unicode_char;
    float left, top, width, height; // нормалізовані 0..1 текстурні координати
};

// Розбирає рядок з можливими "<icon_name>" вставками — ТОЧНА копія
// парсера draw_label_or_icon() з osd_source.cpp: послідовність
// text/icon-сегментів у порядку появи. Винесено окремо (не всередину
// AtlasFont), щоб canvas_renderer міг рахувати bounding box без
// реального малювання (dry-run для хіт-тесту).
struct LabelSegment {
    bool is_icon;
    std::string content;
};

inline std::vector<LabelSegment> parse_label_segments(const std::string& text) {
    std::vector<LabelSegment> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t lt = text.find('<', i);
        if (lt == std::string::npos) { out.push_back({false, text.substr(i)}); break; }
        if (lt > i) out.push_back({false, text.substr(i, lt - i)});
        size_t gt = text.find('>', lt + 1);
        if (gt == std::string::npos) { out.push_back({false, text.substr(lt)}); break; }
        if (gt == lt + 1) {
            out.push_back({false, text.substr(lt, gt - lt + 1)});
        } else {
            out.push_back({true, text.substr(lt + 1, gt - lt - 1)});
        }
        i = gt + 1;
    }
    return out;
}

class AtlasFont {
public:
    ~AtlasFont() { destroy(); }

    bool load(SDL_Renderer* renderer, const std::string& atlas_png_path,
              const std::string& glyph_bin_path, std::string* err) {
        destroy();

        SDL_Surface* surf = IMG_Load(atlas_png_path.c_str());
        if (!surf) {
            if (err) *err = std::string("не вдалося завантажити атлас: ") + IMG_GetError();
            return false;
        }
        atlas_w_ = surf->w;
        atlas_h_ = surf->h;
        texture_ = SDL_CreateTextureFromSurface(renderer, surf);
        SDL_FreeSurface(surf);
        if (!texture_) {
            if (err) *err = std::string("не вдалося створити текстуру атласу: ") + SDL_GetError();
            return false;
        }
        SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);

        std::ifstream f(glyph_bin_path, std::ios::binary);
        if (!f.is_open()) {
            if (err) *err = "не вдалося відкрити " + glyph_bin_path;
            return false;
        }
        uint32_t count = 0;
        f.read(reinterpret_cast<char*>(&count), sizeof(count));
        glyphs_.resize(count);
        f.read(reinterpret_cast<char*>(glyphs_.data()),
               static_cast<std::streamsize>(count) * sizeof(GlyphInfo));
        if (!f) {
            if (err) *err = "пошкоджений/обрізаний " + glyph_bin_path;
            return false;
        }

        index_.clear();
        index_.reserve(glyphs_.size());
        for (size_t i = 0; i < glyphs_.size(); ++i) {
            index_[glyphs_[i].unicode_char] = i;
        }
        return true;
    }

    void destroy() {
        if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
        glyphs_.clear();
        index_.clear();
    }

    bool is_loaded() const { return texture_ != nullptr; }

    // Малює один гліф за кодом у позицію (x,y), повертає x ПІСЛЯ гліфа.
    // Пряме дзеркало blit_glyph() з osd_source.cpp, тільки SDL_RenderCopy
    // замість ручного alpha-blend у CPU-буфер.
    int blit_glyph(SDL_Renderer* renderer, const GlyphInfo& g, int x, int y,
                    SDL_Color tint = {255, 255, 255, 255},
                    int ref_w = -1, int ref_h = -1) const {
        int base_w = (ref_w > 0) ? ref_w : atlas_w_;
        int base_h = (ref_h > 0) ? ref_h : atlas_h_;

        SDL_Rect src;
        src.x = static_cast<int>(g.left * atlas_w_);
        src.y = static_cast<int>(g.top * atlas_h_);
        src.w = static_cast<int>(g.width * atlas_w_);
        src.h = static_cast<int>(g.height * atlas_h_);
        if (src.w < 1) src.w = 1;
        if (src.h < 1) src.h = 1;

        // РОЗМІР ГЛІФА — ТОЧНО ЯК У СТАНЦІЇ.
        //
        // Було: g.width * ширина_полотна і g.height * висота_полотна,
        // тобто числа з .bin трактувались як частка ЕКРАНА. Вони не
        // частка екрана, а частка АТЛАСА: g.width * atlas_w дає рівно
        // цілі 14/21/28/34/42 пікселів, а як частка екрана — 9.3 x 12.6,
        // ще й з іншою пропорцією. Різні дільники по осях додатково
        // сплющували літеру.
        //
        // Наслідок був не косметичний: редактор показував не те, що
        // намалює станція, а розкладку роблять саме по ньому.
        //
        // Тепер піксельний розмір з атласа множиться на той самий
        // множник, що й у станції: висота полотна / kOsdLayoutRefHeight.
        (void)base_w;
        const float scale = (base_h > 0)
                          ? static_cast<float>(base_h) / static_cast<float>(kOsdLayoutRefHeight)
                          : 1.0f;
        SDL_Rect dst{
            x,
            y,
            std::max(1, static_cast<int>(g.width * atlas_w_ * scale + 0.5f)),
            std::max(1, static_cast<int>(g.height * atlas_h_ * scale + 0.5f))
        };
        SDL_SetTextureColorMod(texture_, tint.r, tint.g, tint.b);
        SDL_SetTextureAlphaMod(texture_, tint.a);
        SDL_RenderCopy(renderer, texture_, &src, &dst);
        return x + dst.w;
    }

    const GlyphInfo* find_glyph(uint32_t full_code) const {
        auto it = index_.find(full_code);
        if (it == index_.end()) return nullptr;
        return &glyphs_[it->second];
    }

    // UTF-8 -> codepoint -> blit_glyph, ТОЧНА копія draw_text().
    int draw_text(SDL_Renderer* renderer, const std::string& text, int x, int y,
                  int size_index, SDL_Color tint = {255, 255, 255, 255},
                  int ref_w = -1, int ref_h = -1) const {
        uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
        int cursor_x = x;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t codepoint = 0;
            unsigned char c1 = text[i];
            if (c1 < 0x80) { codepoint = c1; i += 1; }
            else if ((c1 & 0xE0) == 0xC0 && i + 1 < text.size()) {
                codepoint = ((c1 & 0x1F) << 6) | (text[i + 1] & 0x3F); i += 2;
            } else if ((c1 & 0xF0) == 0xE0 && i + 2 < text.size()) {
                codepoint = ((c1 & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
                i += 3;
            } else if (i + 3 < text.size()) {
                codepoint = ((c1 & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) |
                            ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
                i += 4;
            } else break;

            const GlyphInfo* g = find_glyph(codepoint + size_offset);
            if (!g) continue;
            cursor_x = blit_glyph(renderer, *g, cursor_x, y, tint, ref_w, ref_h);
        }
        return cursor_x;
    }

    int draw_icon(SDL_Renderer* renderer, const std::string& icon_name, int x, int y,
                  int size_index, SDL_Color tint = {255, 255, 255, 255},
                  int ref_w = -1, int ref_h = -1) const {
        uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
        uint32_t code = CUSTOM_ICON_BASE + simple_name_hash(icon_name) + size_offset;
        const GlyphInfo* g = find_glyph(code);
        if (!g) return x;
        return blit_glyph(renderer, *g, x, y, tint, ref_w, ref_h);
    }

    // Той самий mixed icon/text парсер, що й draw_label_or_icon() у
    // прошивці (баг-фікс "<img1> kek <img2>" вже врахований тут).
    int draw_label_or_icon(SDL_Renderer* renderer, const std::string& text, int x, int y,
                           int size_index, SDL_Color tint = {255, 255, 255, 255},
                           int ref_w = -1, int ref_h = -1) const {
        for (const auto& seg : parse_label_segments(text)) {
            if (seg.is_icon) {
                x = draw_icon(renderer, seg.content, x, y, size_index, tint, ref_w, ref_h);
            } else if (!seg.content.empty()) {
                x = draw_text(renderer, seg.content, x, y, size_index, tint, ref_w, ref_h);
            }
        }
        return x;
    }

    // Рахує bounding box рядка (bez малювання) — для хіт-тесту на канві.
    // width/height рахуються з тих самих гліфів, що й реальний рендер.
    void measure_label_or_icon(const std::string& text, int size_index,
                                int* out_w, int* out_h,
                                int ref_w = -1, int ref_h = -1) const {
        int base_w = (ref_w > 0) ? ref_w : atlas_w_;
        int base_h = (ref_h > 0) ? ref_h : atlas_h_;
        int w = 0, h = 0;
        for (const auto& seg : parse_label_segments(text)) {
            if (seg.is_icon) {
                uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
                uint32_t code = CUSTOM_ICON_BASE + simple_name_hash(seg.content) + size_offset;
                const GlyphInfo* g = find_glyph(code);
                if (g) {
                    w += std::max(1, static_cast<int>(g->width * base_w + 0.5f));
                    h = std::max(h, std::max(1, static_cast<int>(g->height * base_h + 0.5f)));
                }
            } else {
                w += measure_text_width(seg.content, size_index, base_w);
                h = std::max(h, measure_text_height(seg.content, size_index, base_h));
            }
        }
        *out_w = w;
        *out_h = h;
    }

    int measure_text_width(const std::string& text, int size_index, int base_w = -1) const {
        int wbase = (base_w > 0) ? base_w : atlas_w_;
        uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
        int w = 0;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t codepoint = 0;
            unsigned char c1 = text[i];
            if (c1 < 0x80) { codepoint = c1; i += 1; }
            else if ((c1 & 0xE0) == 0xC0 && i + 1 < text.size()) {
                codepoint = ((c1 & 0x1F) << 6) | (text[i + 1] & 0x3F); i += 2;
            } else if ((c1 & 0xF0) == 0xE0 && i + 2 < text.size()) {
                codepoint = ((c1 & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
                i += 3;
            } else if (i + 3 < text.size()) {
                codepoint = ((c1 & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) |
                            ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
                i += 4;
            } else break;
            const GlyphInfo* g = find_glyph(codepoint + size_offset);
            if (g) w += std::max(1, static_cast<int>(g->width * wbase + 0.5f));
        }
        return w;
    }

    int measure_text_height(const std::string& text, int size_index, int base_h = -1) const {
        int hbase = (base_h > 0) ? base_h : atlas_h_;
        uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
        int h = 0;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t codepoint = static_cast<unsigned char>(text[i]);
            if (codepoint >= 0x80) { ++i; continue; } // спрощено: висота рахується з ASCII, достатньо для bbox
            const GlyphInfo* g = find_glyph(codepoint + size_offset);
            if (g) h = std::max(h, std::max(1, static_cast<int>(g->height * hbase + 0.5f)));
            ++i;
        }
        return h;
    }

    int atlas_w() const { return atlas_w_; }
    int atlas_h() const { return atlas_h_; }

private:
    SDL_Texture* texture_ = nullptr;
    int atlas_w_ = 0, atlas_h_ = 0;
    std::vector<GlyphInfo> glyphs_;
    std::unordered_map<uint32_t, size_t> index_;
};

} // namespace osdedit
