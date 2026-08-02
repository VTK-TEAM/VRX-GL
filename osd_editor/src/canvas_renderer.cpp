#include "canvas_renderer.h"
#include "vt_telemetry_fetch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace osdedit {

namespace {

// Обгортка над спільною vt_fetch_channel_value() (vt_telemetry_fetch.h) —
// та сама логіка, що й OsdSource::fetch_channel_value() у рушії VRX,
// одне джерело істини на обидва місця. Тут лише переклад "META-рядок" ->
// два bool, щоб виклики нижче лишились без змін.
bool fetch_channel_value(VtTelemetryStorage* storage, int channel_id,
                          const std::string& meta, float* out_value) {
    if (!storage) return false;
    return vt_fetch_channel_value(*storage, channel_id, meta == "AGE_S", meta == "RATE_HZ", out_value);
}

// ЩО ПОКАЗАТИ, КОЛИ ПОКАЗУВАТИ НІЧОГО.
//
// Елемент, який у поточному стані даних не дає жодного символу — ENUM без
// збігу й без DEFAULT (як причина дизарму, коли їх немає), порожній
// LABEL, — на полотні не малюється зовсім. У станції це правильно: немає
// чого казати, нічого й не займає місця.
//
// У РЕДАКТОРІ це помилка. Невидимий елемент не можна ні вибрати, ні
// пересунути, ні видалити: він є у файлі, займе місце в польоті, а тут
// його наче й немає. Тому підставляємо явну заглушку — вона показує, ЩО
// елемент існує і ДЕ він стоїть, і зникне сама, щойно з'явиться значення.
const char* kNoText = "NOTEXT";

std::string with_placeholder(const std::string& s) {
    return s.empty() ? std::string(kNoText) : s;
}

std::string eval_enum(const OsdElement& el, float value) {
    constexpr float EQ_EPSILON = 0.0001f;
    for (const auto& c : el.cases()) {
        bool matched = false;
        switch (c.op) {
            case CompareOp::EQ: matched = std::fabs(value - c.threshold) < EQ_EPSILON; break;
            case CompareOp::GT: matched = (value > c.threshold); break;
            case CompareOp::LT: matched = (value < c.threshold); break;
        }
        if (matched) return c.label;
    }
    return el.enum_default();
}

} // namespace

bool CanvasRenderer::screen_to_canvas_norm(int screen_x, int screen_y, float* nx, float* ny) const {
    if (canvas_rect_.w <= 0 || canvas_rect_.h <= 0) return false;
    if (screen_x < canvas_rect_.x || screen_x >= canvas_rect_.x + canvas_rect_.w ||
        screen_y < canvas_rect_.y || screen_y >= canvas_rect_.y + canvas_rect_.h) {
        return false;
    }
    *nx = static_cast<float>(screen_x - canvas_rect_.x) / static_cast<float>(canvas_rect_.w);
    *ny = static_cast<float>(screen_y - canvas_rect_.y) / static_cast<float>(canvas_rect_.h);
    return true;
}

void CanvasRenderer::canvas_norm_to_screen(float nx, float ny, int* screen_x, int* screen_y) const {
    *screen_x = canvas_rect_.x + static_cast<int>(nx * canvas_rect_.w);
    *screen_y = canvas_rect_.y + static_cast<int>(ny * canvas_rect_.h);
}

void CanvasRenderer::draw_placeholder_rect(SDL_Renderer* renderer, SDL_Rect r, const char* label) const {
    // Явний "чогось бракує" плейсхолдер — сіра штрихована рамка +
    // напівпрозора заливка, а НЕ мовчазна відсутність елемента. Дуже
    // навмисно: користувач має ПОБАЧИТИ на етапі редагування, що файл
    // картинки (EMPTY_IMAGE/FILL_IMAGE/IMAGE) не знайдено, а не
    // виявити це вже під час польоту.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 120, 120, 120, 90);
    SDL_RenderFillRect(renderer, &r);
    SDL_SetRenderDrawColor(renderer, 230, 60, 60, 220);
    // штрихована рамка (проста версія — через сегменти)
    for (int x = r.x; x < r.x + r.w; x += 8) {
        SDL_RenderDrawLine(renderer, x, r.y, std::min(x + 4, r.x + r.w), r.y);
        SDL_RenderDrawLine(renderer, x, r.y + r.h - 1, std::min(x + 4, r.x + r.w), r.y + r.h - 1);
    }
    for (int y = r.y; y < r.y + r.h; y += 8) {
        SDL_RenderDrawLine(renderer, r.x, y, r.x, std::min(y + 4, r.y + r.h));
        SDL_RenderDrawLine(renderer, r.x + r.w - 1, y, r.x + r.w - 1, std::min(y + 4, r.y + r.h));
    }
    if (label && *label) {
        font_.draw_text(renderer, label, r.x + 4, r.y + 2, 0, SDL_Color{255, 255, 255, 255});
    }
}

void CanvasRenderer::draw_element(SDL_Renderer* renderer, const OsdElement& el, bool is_selected,
                                  ImageCache& images, std::vector<ElementHitBox>* hits,
                                  VtTelemetryStorage* storage) const {
    int sx, sy;
    canvas_norm_to_screen(el.l(), el.t(), &sx, &sy);
    const int ref_w = canvas_rect_.w;
    const int ref_h = canvas_rect_.h;
    SDL_Color tint{255, 255, 255, 255};

    switch (el.type()) {
    case ElementType::LABEL: {
        int w = 0, h = 0;
        const std::string text = with_placeholder(el.label());
        font_.measure_label_or_icon(text, el.size_index(), &w, &h, ref_w, ref_h);
        font_.draw_label_or_icon(renderer, text, sx, sy, el.size_index(), tint, ref_w, ref_h);
        if (w <= 0) w = 12;
        if (h <= 0) h = 16;
        hits->push_back({el.key(), SDL_Rect{sx - 3, sy - 3, w + 6, h + 6}});
        break;
    }
    case ElementType::VALUE: {
        std::string value_str = "--";
        float value = 0.f;
        if (fetch_channel_value(storage, el.data_channel(), el.meta(), &value)) {
            char num_buf[32];
            std::snprintf(num_buf, sizeof(num_buf), "%.*f", el.decimals(), value);
            value_str = num_buf;
        }
        int lw = 0, lh = 0, vw = 0, vh = 0;
        font_.measure_label_or_icon(el.label(), el.size_index(), &lw, &lh, ref_w, ref_h);
        int after_label = font_.draw_label_or_icon(renderer, el.label(), sx, sy, el.size_index(), tint, ref_w, ref_h);
        font_.measure_label_or_icon(value_str, el.size_index(), &vw, &vh, ref_w, ref_h);
        int after_value = font_.draw_label_or_icon(renderer, value_str, after_label, sy, el.size_index(), tint, ref_w, ref_h);

        int uw = 0, uh = 0;
        if (!el.units().empty()) {
            font_.measure_label_or_icon(el.units(), el.size_index(), &uw, &uh, ref_w, ref_h);
            font_.draw_label_or_icon(renderer, el.units(), after_value, sy, el.size_index(), tint, ref_w, ref_h);
        }

        int total_w = (lw > 0 ? lw : 0) + vw + uw;
        int total_h = std::max(lh, vh);
        total_h = std::max(total_h, uh);
        if (total_w <= 0) total_w = 12;
        if (total_h <= 0) total_h = 16;
        hits->push_back({el.key(), SDL_Rect{sx - 3, sy - 3, total_w + 6, total_h + 6}});
        break;
    }
    case ElementType::ENUM_SWITCH: {
        float value = 0.f;
        bool have_value = fetch_channel_value(storage, el.data_channel(), el.meta(), &value);
        std::string preview = have_value ? eval_enum(el, value) : el.enum_default();
        // Порожньо і в підпису, і в значенні — елемент лишився б невидимим.
        if (preview.empty() && el.label().empty()) preview = kNoText;

        int lw = 0, lh = 0, pw = 0, ph = 0;
        font_.measure_label_or_icon(el.label(), el.size_index(), &lw, &lh, ref_w, ref_h);
        int after_label = font_.draw_label_or_icon(renderer, el.label(), sx, sy, el.size_index(), tint, ref_w, ref_h);
        font_.measure_label_or_icon(preview, el.size_index(), &pw, &ph, ref_w, ref_h);
        font_.draw_label_or_icon(renderer, preview, after_label, sy, el.size_index(), tint, ref_w, ref_h);
        int total_w = (lw > 0 ? lw : 0) + pw;
        int total_h = std::max(lh, ph);
        if (total_w <= 0) total_w = 12;
        if (total_h <= 0) total_h = 16;
        hits->push_back({el.key(), SDL_Rect{sx - 3, sy - 3, total_w + 6, total_h + 6}});
        break;
    }
    case ElementType::BAR: {
        int bw = static_cast<int>(el.bar_w() * canvas_rect_.w);
        int bh = static_cast<int>(el.bar_h() * canvas_rect_.h);
        if (bw < 4) bw = 4;
        if (bh < 4) bh = 4;
        SDL_Rect full{sx, sy, bw, bh};

        SDL_Texture* empty_tex = images.get(el.bar_empty_image());
        SDL_Texture* fill_tex = images.get(el.bar_fill_image());
        if (empty_tex) {
            SDL_RenderCopy(renderer, empty_tex, nullptr, &full);
        } else {
            draw_placeholder_rect(renderer, full, "empty?");
        }
        float value = el.bar_min();
        fetch_channel_value(storage, el.data_channel(), el.meta(), &value);

        float range = el.bar_max() - el.bar_min();
        float frac = (range != 0.f) ? (value - el.bar_min()) / range : 0.f;
        if (frac < 0.f) frac = 0.f;
        if (frac > 1.f) frac = 1.f;
        int fill_w = static_cast<int>(bw * frac + 0.5f);

        if (fill_tex) {
            SDL_Rect fill_clip{sx, sy, fill_w, bh};
            SDL_Rect src{0, 0, 0, 0};
            SDL_QueryTexture(fill_tex, nullptr, nullptr, &src.w, &src.h);
            SDL_Rect src_clip{0, 0, static_cast<int>(src.w * frac), src.h};
            if (fill_w > 0 && src_clip.w > 0) {
                SDL_RenderCopy(renderer, fill_tex, &src_clip, &fill_clip);
            }
        } else if (!empty_tex) {
            // якщо ОБИДВА відсутні, другий плейсхолдер зайвий — досить одного
        } else {
            if (fill_w > 0) {
                SDL_Rect fill_clip{sx, sy, fill_w, bh};
                draw_placeholder_rect(renderer, fill_clip, "fill?");
            }
        }

        // Відсоток заповнення збоку від бару — та сама логіка, що й
        // OsdSource::draw_bar() у рушії VRX, щоб превʼю не розходилось з
        // реальним рендером на екрані дрона.
        {
            int percent = static_cast<int>(std::round(frac * 100.0f));
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            const int gap_px = std::max(2, static_cast<int>(0.004f * canvas_rect_.w));
            const int text_x = sx + bw + gap_px;
            const std::string percent_text = std::to_string(percent) + "%";
            font_.draw_label_or_icon(renderer, percent_text, text_x, sy, el.size_index(), tint, ref_w, ref_h);
        }

        hits->push_back({el.key(), full});
        break;
    }
    case ElementType::HORIZON: {
        // Відповідність runtime: горизонт фіксовано по центру і на
        // 100% висоти canvas.
        const int cx = canvas_rect_.x + canvas_rect_.w / 2;
        const int cy = canvas_rect_.y + canvas_rect_.h / 2;

        SDL_Texture* base_tex = images.get(el.horizon_base_image());
        SDL_Texture* ptr_tex = images.get(el.horizon_pointer_image());
        SDL_Texture* legacy_tex = images.get(el.image_path());
        if (!base_tex) base_tex = legacy_tex;
        if (!ptr_tex) ptr_tex = legacy_tex;

        SDL_Texture* size_tex = base_tex ? base_tex : ptr_tex;
        int ih = canvas_rect_.h;
        if (ih < 4) ih = 4;
        int iw = 4;
        if (size_tex) {
            int src_w = 0, src_h = 0;
            SDL_QueryTexture(size_tex, nullptr, nullptr, &src_w, &src_h);
            if (src_w > 0 && src_h > 0) {
                iw = static_cast<int>(std::round(static_cast<double>(ih) * src_w / src_h));
                if (iw < 4) iw = 4;
            }
        }
        SDL_Rect full{cx - iw / 2, cy - ih / 2, iw, ih};

        float angle_deg_f = 0.f;
        fetch_channel_value(storage, el.data_channel(), el.meta(), &angle_deg_f);
        double angle_deg = static_cast<double>(angle_deg_f);

        if (base_tex) {
            SDL_RenderCopy(renderer, base_tex, nullptr, &full);
        }
        if (ptr_tex) {
            SDL_RenderCopyEx(renderer, ptr_tex, nullptr, &full, -angle_deg, nullptr, SDL_FLIP_NONE);
        }
        if (!base_tex && !ptr_tex) {
            draw_placeholder_rect(renderer, full, "horizon?");
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
            SDL_RenderDrawLine(renderer, full.x, cy, full.x + full.w, cy);
            SDL_RenderDrawLine(renderer, cx, full.y, cx, full.y + full.h);
        }
        // Щоб HORIZON (який займає майже всю канву) не блокував вибір
        // інших елементів, зона хіт-тесту лишається компактною біля
        // центра.
        SDL_Rect pick{cx - 24, cy - 24, 48, 48};
        hits->push_back({el.key(), pick});
        break;
    }
    }

    if (is_selected) {
        auto& box = hits->back().rect;
        SDL_SetRenderDrawColor(renderer, 80, 200, 255, 255);
        SDL_Rect outline{box.x - 2, box.y - 2, box.w + 4, box.h + 4};
        SDL_RenderDrawRect(renderer, &outline);
    }
}

std::vector<ElementHitBox> CanvasRenderer::render(SDL_Renderer* renderer,
                                                   const std::vector<OsdElement>& elements,
                                                   const std::string& selected_key,
                                                   ImageCache& images,
                                                   VtTelemetryStorage* storage) const {
    SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
    SDL_RenderClear(renderer);

    if (bg_texture_) {
        SDL_RenderCopy(renderer, bg_texture_, nullptr, &canvas_rect_);
    } else {
        SDL_SetRenderDrawColor(renderer, 40, 60, 90, 255);
        SDL_RenderFillRect(renderer, &canvas_rect_);
    }

    SDL_Rect clip_prev;
    SDL_RenderGetClipRect(renderer, &clip_prev);
    SDL_RenderSetClipRect(renderer, &canvas_rect_);

    std::vector<ElementHitBox> hits;
    hits.reserve(elements.size());
    for (const auto& el : elements) {
        draw_element(renderer, el, el.key() == selected_key, images, &hits, storage);
    }

    SDL_RenderSetClipRect(renderer, clip_prev.w > 0 ? &clip_prev : nullptr);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
    SDL_RenderDrawRect(renderer, &canvas_rect_);

    return hits;
}

} // namespace osdedit
