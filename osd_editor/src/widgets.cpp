#include "widgets.h"
#include <cstdio>
#include <cmath>

namespace osdedit {

// ---------------------------------------------------------------- helpers

void draw_ui_text(SDL_Renderer* renderer, TTF_Font* font, const std::string& text,
                   int x, int y, SDL_Color color) {
    if (text.empty() || !font) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_Rect dst{x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

int measure_ui_text_width(TTF_Font* font, const std::string& text) {
    if (!font || text.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text.c_str(), &w, &h);
    return w;
}

// Спрощено: без реального заокруглення кутів (щоб не тягнути окрему
// геометрію) — просто суцільна заливка/рамка. Назва лишена як є, бо
// візуально на невеликих контролах різниця з реальним rounded-rect
// непомітна, а код разів у 5 простіший.
void fill_rounded_rect(SDL_Renderer* renderer, SDL_Rect r, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &r);
}

void draw_rounded_rect_border(SDL_Renderer* renderer, SDL_Rect r, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderDrawRect(renderer, &r);
}

// ---------------------------------------------------------------- Button

void Button::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    SDL_Color bg = bg_color;
    if (!enabled) { bg.r = bg.r / 2; bg.g = bg.g / 2; bg.b = bg.b / 2; }
    else if (pressed_) { bg.r = std::min(255, bg.r + 30); bg.g = std::min(255, bg.g + 30); bg.b = std::min(255, bg.b + 30); }
    fill_rounded_rect(renderer, bounds, bg);
    draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);

    int tw = measure_ui_text_width(font, label);
    int tx = bounds.x + (bounds.w - tw) / 2;
    int ty = bounds.y + (bounds.h - TTF_FontHeight(font)) / 2;
    draw_ui_text(renderer, font, label, tx, ty, enabled ? text_color : ui_color::TEXT_MUTED);
}

bool Button::handle_mouse_down(int x, int y) {
    if (!visible || !enabled) return false;
    if (!contains(x, y)) return false;
    pressed_ = true;
    if (on_click) on_click();
    return true;
}

// ---------------------------------------------------------------- UiLabel

void UiLabel::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    draw_ui_text(renderer, font, text, bounds.x, bounds.y, color);
}

// ---------------------------------------------------------------- Panel

void Panel::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    fill_rounded_rect(renderer, bounds, bg_color);
    if (draw_border) draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);
    for (auto& c : children_) c->draw(renderer, font);
}

bool Panel::handle_mouse_down(int x, int y) {
    if (!visible) return false;
    // Зворотній порядок — те, що намальовано ОСТАННІМ (найзверху),
    // мусить отримати клік ПЕРШИМ.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->handle_mouse_down(x, y)) return true;
    }
    return contains(x, y); // панель сама "з'їдає" клік всередині себе (модальність)
}

void Panel::handle_mouse_up(int x, int y) {
    if (!visible) return;
    for (auto& c : children_) c->handle_mouse_up(x, y);
}

void Panel::handle_mouse_motion(int x, int y) {
    if (!visible) return;
    for (auto& c : children_) c->handle_mouse_motion(x, y);
}

bool Panel::handle_wheel(int x, int y, int delta) {
    if (!visible) return false;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if ((*it)->handle_wheel(x, y, delta)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- NumberField

void NumberField::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    fill_rounded_rect(renderer, bounds, ui_color::BG_LIGHT);
    draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);

    if (!label_text.empty()) {
        draw_ui_text(renderer, font, label_text, bounds.x + 6, bounds.y - 18, ui_color::TEXT_MUTED);
    }

    // Чотири зони: [--][-]  значення  [+][++] — велике коло (big_step)
    // по краях, дрібне (step) ближче до центру. Раніше тут була лише
    // одна пара -/+ і big_step ніде не читався: щоб дійти від 0 до
    // 12.6 з кроком 0.1 треба було 126 кліків. Тепер [++] за 1-2 кліки
    // покриває більшість діапазону, [+] лишається для точного підбору.
    int btn_w = 26;
    SDL_Rect big_minus{bounds.x, bounds.y, btn_w, bounds.h};
    SDL_Rect minus{bounds.x + btn_w, bounds.y, btn_w, bounds.h};
    SDL_Rect plus{bounds.x + bounds.w - btn_w * 2, bounds.y, btn_w, bounds.h};
    SDL_Rect big_plus{bounds.x + bounds.w - btn_w, bounds.y, btn_w, bounds.h};

    fill_rounded_rect(renderer, big_minus, ui_color::BG);
    fill_rounded_rect(renderer, minus, ui_color::BG);
    fill_rounded_rect(renderer, plus, ui_color::BG);
    fill_rounded_rect(renderer, big_plus, ui_color::BG);

    draw_ui_text(renderer, font, "--", big_minus.x + 4, big_minus.y + 2, ui_color::TEXT_MUTED);
    draw_ui_text(renderer, font, "-", minus.x + 10, minus.y + 2, ui_color::TEXT);
    draw_ui_text(renderer, font, "+", plus.x + 9, plus.y + 2, ui_color::TEXT);
    draw_ui_text(renderer, font, "++", big_plus.x + 2, big_plus.y + 2, ui_color::TEXT_MUTED);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    int tw = measure_ui_text_width(font, buf);
    int tx = bounds.x + (bounds.w - tw) / 2;
    draw_ui_text(renderer, font, buf, tx, bounds.y + 2, ui_color::TEXT);
}

void NumberField::apply_delta(float d) {
    value += d;
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    if (on_change) on_change(value);
}

bool NumberField::handle_mouse_down(int x, int y) {
    if (!visible || !enabled || !contains(x, y)) return false;
    int btn_w = 26;
    if (x < bounds.x + btn_w) {
        apply_delta(-big_step);
    } else if (x < bounds.x + btn_w * 2) {
        apply_delta(-step);
    } else if (x >= bounds.x + bounds.w - btn_w) {
        apply_delta(big_step);
    } else if (x >= bounds.x + bounds.w - btn_w * 2) {
        apply_delta(step);
    }
    return true;
}

// ---------------------------------------------------------------- SizeIndexPicker

void SizeIndexPicker::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    static const char* NAMES[5] = {"XS", "S", "M", "B", "XL"};
    int cell_w = bounds.w / 5;
    for (int i = 0; i < 5; ++i) {
        SDL_Rect r{bounds.x + i * cell_w, bounds.y, cell_w - 4, bounds.h};
        fill_rounded_rect(renderer, r, i == selected ? ui_color::ACCENT : ui_color::BG_LIGHT);
        draw_rounded_rect_border(renderer, r, ui_color::BORDER);
        int tw = measure_ui_text_width(font, NAMES[i]);
        draw_ui_text(renderer, font, NAMES[i], r.x + (r.w - tw) / 2, r.y + 4,
                     i == selected ? SDL_Color{20, 20, 20, 255} : ui_color::TEXT);
    }
}

bool SizeIndexPicker::handle_mouse_down(int x, int y) {
    if (!visible || !enabled || !contains(x, y)) return false;
    int cell_w = bounds.w / 5;
    int idx = (x - bounds.x) / cell_w;
    if (idx < 0) idx = 0;
    if (idx > 4) idx = 4;
    selected = idx;
    if (on_change) on_change(selected);
    return true;
}

// ---------------------------------------------------------------- EnumOpPicker

void EnumOpPicker::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    static const char* SYMS[3] = {"=", ">", "<"};
    fill_rounded_rect(renderer, bounds, ui_color::BG_LIGHT);
    draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);
    std::string s = SYMS[op_index >= 0 && op_index < 3 ? op_index : 0];
    int tw = measure_ui_text_width(font, s);
    draw_ui_text(renderer, font, s, bounds.x + (bounds.w - tw) / 2, bounds.y + 3, ui_color::ACCENT);
}

bool EnumOpPicker::handle_mouse_down(int x, int y) {
    if (!visible || !enabled || !contains(x, y)) return false;
    op_index = (op_index + 1) % 3;
    if (on_change) on_change(op_index);
    return true;
}

// ---------------------------------------------------------------- TextField

void TextField::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    if (!label_text.empty()) {
        draw_ui_text(renderer, font, label_text, bounds.x, bounds.y - 18, ui_color::TEXT_MUTED);
    }
    fill_rounded_rect(renderer, bounds, ui_color::BG_LIGHT);
    draw_rounded_rect_border(renderer, bounds, focused ? ui_color::BORDER_HL : ui_color::BORDER);

    std::string shown = value.empty() ? placeholder : value;
    SDL_Color c = value.empty() ? ui_color::TEXT_MUTED : ui_color::TEXT;
    draw_ui_text(renderer, font, shown, bounds.x + 8, bounds.y + (bounds.h - TTF_FontHeight(font)) / 2, c);
    if (focused) {
        int tw = measure_ui_text_width(font, value);
        SDL_SetRenderDrawColor(renderer, ui_color::ACCENT.r, ui_color::ACCENT.g, ui_color::ACCENT.b, 255);
        SDL_RenderDrawLine(renderer, bounds.x + 8 + tw + 2, bounds.y + 4,
                           bounds.x + 8 + tw + 2, bounds.y + bounds.h - 4);
    }
}

bool TextField::handle_mouse_down(int x, int y) {
    if (!visible || !enabled || !contains(x, y)) return false;
    if (on_request_focus) on_request_focus();
    return true;
}

// ---------------------------------------------------------------- ListView

void ListView::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible) return;
    fill_rounded_rect(renderer, bounds, ui_color::BG_LIGHT);
    draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);

    SDL_Rect clip_prev;
    SDL_RenderGetClipRect(renderer, &clip_prev);
    SDL_RenderSetClipRect(renderer, &bounds);

    int y = bounds.y - scroll_offset;
    for (size_t i = 0; i < items.size(); ++i) {
        SDL_Rect row{bounds.x, y, bounds.w, row_height};
        if (row.y + row.h > bounds.y && row.y < bounds.y + bounds.h) {
            if (static_cast<int>(i) % 2 == 1) {
                fill_rounded_rect(renderer, row, ui_color::BG);
            }
            draw_ui_text(renderer, font, items[i].text, row.x + 10, row.y + 4, ui_color::TEXT);
            if (!items[i].subtitle.empty()) {
                draw_ui_text(renderer, font, items[i].subtitle, row.x + 10, row.y + 4 + TTF_FontHeight(font),
                             ui_color::TEXT_MUTED);
            }
            SDL_SetRenderDrawColor(renderer, ui_color::BORDER.r, ui_color::BORDER.g, ui_color::BORDER.b, 120);
            SDL_RenderDrawLine(renderer, row.x, row.y + row.h - 1, row.x + row.w, row.y + row.h - 1);
        }
        y += row_height;
    }

    SDL_RenderSetClipRect(renderer, clip_prev.w > 0 ? &clip_prev : nullptr);
}

bool ListView::handle_mouse_down(int x, int y) {
    if (!visible || !contains(x, y)) return false;
    int rel_y = (y - bounds.y) + scroll_offset;
    int idx = rel_y / row_height;
    if (idx >= 0 && idx < static_cast<int>(items.size())) {
        if (on_select) on_select(idx);
    }
    return true;
}

bool ListView::handle_wheel(int x, int y, int delta) {
    if (!visible || !contains(x, y)) return false;
    scroll_offset -= delta * row_height;
    int max_scroll = std::max(0, static_cast<int>(items.size()) * row_height - bounds.h);
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > max_scroll) scroll_offset = max_scroll;
    return true;
}

// ---------------------------------------------------------------- OnScreenKeyboard

std::string OnScreenKeyboard::key_label(const Key& k) const {
    switch (k.kind) {
        case Key::Kind::BACKSPACE: return "<=";
        case Key::Kind::SPACE: return "space";
        case Key::Kind::SHIFT: return shift_ ? "SHIFT*" : "shift";
        case Key::Kind::LANG:
            if (layer_ == Layer::ICONS) return "ICO";
            if (layer_ == Layer::NUM) return "123";
            return layer_ == Layer::UA ? "UA" : "EN";
        case Key::Kind::DONE: return "OK";
        case Key::Kind::CHAR: {
            const std::string& base = (layer_ == Layer::UA)
                ? (shift_ ? k.upper_ua : k.lower_ua)
                : (shift_ ? k.upper_en : k.lower_en);
            return base;
        }
    }
    return "";
}

void OnScreenKeyboard::rebuild_letter_rows() {
    if (bounds.w <= 0 || bounds.h <= 0) return;

    static const std::vector<std::string> ROWS_EN_LOWER = {
        "qwertyuiop", "asdfghjkl", "zxcvbnm"
    };
    static const std::vector<std::string> ROWS_EN_UPPER = {
        "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"
    };
    static const std::vector<std::string> ROWS_UA_LOWER = {
        "йцукенгшщзх", "фівапролджє", "ячсмитьбю"
    };
    static const std::vector<std::string> ROWS_UA_UPPER = {
        "ЙЦУКЕНГШЩЗХ", "ФІВАПРОЛДЖЄ", "ЯЧСМИТЬБЮ"
    };
    static const std::vector<std::string> ROWS_NUM = {
        "1234567890",
        "-_/.:,;@#",
        "()[]{}+=*%$"
    };

    int rows = 4; // 3 літерні + 1 функціональний ряд
    int row_h = bounds.h / rows;
    int margin = 3;

    if (layer_ == Layer::NUM) {
        for (int r = 0; r < 3; ++r) {
            const std::string& row = ROWS_NUM[r];
            size_t n = row.size();
            int row_offset = 0;
            int cell_w = (bounds.w - row_offset) / static_cast<int>(n > 0 ? n : 1);
            for (size_t i = 0; i < n; ++i) {
                std::string ch(1, row[i]);
                Key k;
                k.kind = Key::Kind::CHAR;
                k.lower_en = ch;
                k.upper_en = ch;
                k.lower_ua = ch;
                k.upper_ua = ch;
                k.rect = SDL_Rect{bounds.x + row_offset + static_cast<int>(i) * cell_w + margin,
                                  bounds.y + r * row_h + margin,
                                  cell_w - margin * 2, row_h - margin * 2};
                keys_.push_back(k);
            }
        }
        return;
    }

    const std::vector<std::string>* rows_lower = &ROWS_EN_LOWER;
    const std::vector<std::string>* rows_upper = &ROWS_EN_UPPER;
    if (layer_ == Layer::UA) {
        rows_lower = &ROWS_UA_LOWER;
        rows_upper = &ROWS_UA_UPPER;
    }

    for (int r = 0; r < 3; ++r) {
        // UTF-8: кирилиця в наших рядках — завжди 2-байтні послідовності
        // (діапазон U+0400-U+04FF), латиниця — 1-байтні. Спрощений, але
        // коректний для ОБОХ наших алфавітів посимвольний розбір.
        auto utf8_chars = [](const std::string& s) {
            std::vector<std::string> out;
            size_t i = 0;
            while (i < s.size()) {
                unsigned char c = s[i];
                size_t len = (c < 0x80) ? 1 : 2;
                out.push_back(s.substr(i, len));
                i += len;
            }
            return out;
        };
        auto active_l = utf8_chars((*rows_lower)[r]);
        auto active_u = utf8_chars((*rows_upper)[r]);
        size_t n = active_l.size();

        int row_offset = r * 14; // невеликий зсув рядків, як на фізичній клаві
        int cell_w = (bounds.w - row_offset) / static_cast<int>(n > 0 ? n : 1);
        for (size_t i = 0; i < n; ++i) {
            Key k;
            k.kind = Key::Kind::CHAR;
            if (layer_ == Layer::UA) {
                k.lower_ua = active_l[i];
                k.upper_ua = i < active_u.size() ? active_u[i] : active_l[i];
            } else {
                k.lower_en = active_l[i];
                k.upper_en = i < active_u.size() ? active_u[i] : active_l[i];
            }
            k.rect = SDL_Rect{bounds.x + row_offset + static_cast<int>(i) * cell_w + margin,
                              bounds.y + r * row_h + margin,
                              cell_w - margin * 2, row_h - margin * 2};
            keys_.push_back(k);
        }
    }
}

void OnScreenKeyboard::rebuild_function_row() {
    if (bounds.w <= 0 || bounds.h <= 0) return;

    int rows = 4;
    int row_h = bounds.h / rows;
    int margin = 3;

    int fr_y = bounds.y + 3 * row_h + margin;
    int fr_h = row_h - margin * 2;
    int x = bounds.x + margin;

    auto add_func = [&](Key::Kind kind, int w) {
        Key k; k.kind = kind;
        k.rect = SDL_Rect{x, fr_y, w, fr_h};
        keys_.push_back(k);
        x += w + margin;
    };

    // LANG тепер циклічно перемикає ТРИ шари (EN -> UA -> ICONS -> EN),
    // не два — див. apply_key(). SHIFT в ICONS-шарі просто не має
    // видимого ефекту (нема великих/малих іконок), лишений як є заради
    // простоти — не шкодить, просто неактивний нюанс UI.
    int unit = (bounds.w - margin * 6) / 10;
    add_func(Key::Kind::LANG, unit * 2);
    add_func(Key::Kind::SHIFT, unit * 2);
    add_func(Key::Kind::SPACE, unit * 3);
    add_func(Key::Kind::BACKSPACE, unit * 2);
    add_func(Key::Kind::DONE, unit * 1);
}

void OnScreenKeyboard::rebuild_icon_grid() {
    icon_cells_.clear();
    if (!icons_ || icons_->empty() || bounds.w <= 0 || bounds.h <= 0) return;

    int rows = 4;
    int row_h = bounds.h / rows;
    int margin = 3;
    constexpr int VISIBLE_ROWS = 3; // та сама висота, що й 3 літерні ряди в EN/UA

    // Динамічно рахуємо кількість колонок так, щоб УСІ іконки влізли
    // рівно в 3 ряди без скролу.
    int icon_cols = static_cast<int>((icons_->size() + VISIBLE_ROWS - 1) / VISIBLE_ROWS);
    if (icon_cols < 1) icon_cols = 1;
    int cell_w = bounds.w / icon_cols;
    icon_scroll_row_ = 0;

    for (size_t i = 0; i < icons_->size(); ++i) {
        int abs_row = static_cast<int>(i) / icon_cols;
        int col = static_cast<int>(i) % icon_cols;
        int visible_row = abs_row - icon_scroll_row_;
        if (visible_row < 0 || visible_row >= VISIBLE_ROWS) continue;

        IconCell cell;
        cell.icon_index = static_cast<int>(i);
        cell.rect = SDL_Rect{bounds.x + col * cell_w + margin,
                             bounds.y + visible_row * row_h + margin,
                             cell_w - margin * 2, row_h - margin * 2};
        icon_cells_.push_back(cell);
    }
}

void OnScreenKeyboard::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (!visible || !target) return;
    fill_rounded_rect(renderer, bounds, ui_color::BG);
    draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);

    keys_.clear();
    icon_cells_.clear();
    if (layer_ == Layer::ICONS) {
        rebuild_icon_grid();
    } else {
        rebuild_letter_rows();
    }
    rebuild_function_row(); // завжди, незалежно від шару

    if (layer_ == Layer::ICONS) {
        if (!icons_ || icons_->empty()) {
            draw_ui_text(renderer, font, "Немає іконок — перевір, що osd_icon_names.json є в CWD",
                         bounds.x + 12, bounds.y + 12, ui_color::TEXT_MUTED);
        }
        for (const auto& cell : icon_cells_) {
            fill_rounded_rect(renderer, cell.rect, ui_color::BG_LIGHT);
            draw_rounded_rect_border(renderer, cell.rect, ui_color::BORDER);

            const IconKeyInfo& info = (*icons_)[cell.icon_index];
            if (icon_draw_fn_) {
                icon_draw_fn_(renderer, info.name, cell.rect);
            }
        }
    }

    for (const auto& k : keys_) {
        SDL_Color bg = ui_color::BG_LIGHT;
        if (k.kind == Key::Kind::DONE) bg = ui_color::SUCCESS;
        else if (k.kind == Key::Kind::SHIFT && shift_) bg = ui_color::ACCENT;
        else if (k.kind == Key::Kind::LANG) bg = ui_color::ACCENT;
        fill_rounded_rect(renderer, k.rect, bg);
        draw_rounded_rect_border(renderer, k.rect, ui_color::BORDER);

        std::string lbl = key_label(k);
        int tw = measure_ui_text_width(font, lbl);
        int th = TTF_FontHeight(font);
        draw_ui_text(renderer, font, lbl, k.rect.x + (k.rect.w - tw) / 2,
                     k.rect.y + (k.rect.h - th) / 2, ui_color::TEXT);
    }
}

void OnScreenKeyboard::apply_key(const Key& k) {
    if (!target) return;
    switch (k.kind) {
        case Key::Kind::CHAR: {
            *target += key_label(k);
            if (shift_) shift_ = false; // shift одноразовий, як на телефонній клаві
            if (on_change) on_change();
            break;
        }
        case Key::Kind::BACKSPACE: {
            if (!target->empty()) {
                // видаляємо ОСТАННІЙ UTF-8 символ (не байт) — інакше
                // backspace на кирилиці "з'їдав" би пів-символу. Іконка-
                // токен ("<battery>") тут теж коректно "з'їдається" по
                // одному байту з кінця — щоб видалити токен цілком,
                // треба кілька натискань backspace, це прийнятно (той
                // самий backspace UX, що й на телефоні для emoji).
                size_t i = target->size();
                do { --i; } while (i > 0 && (static_cast<unsigned char>((*target)[i]) & 0xC0) == 0x80);
                target->erase(i);
            }
            if (on_change) on_change();
            break;
        }
        case Key::Kind::SPACE: *target += " "; if (on_change) on_change(); break;
        case Key::Kind::SHIFT: shift_ = !shift_; break;
        case Key::Kind::LANG:
            layer_ = (layer_ == Layer::EN) ? Layer::UA
                   : (layer_ == Layer::UA) ? Layer::NUM
                   : (layer_ == Layer::NUM) ? Layer::ICONS
                                            : Layer::EN;
            break;
        case Key::Kind::DONE: if (on_done) on_done(); break;
    }
}

bool OnScreenKeyboard::handle_mouse_down(int x, int y) {
    if (!visible || !target || !contains(x, y)) return false;

    if (layer_ == Layer::ICONS) {
        for (const auto& cell : icon_cells_) {
            if (x >= cell.rect.x && x < cell.rect.x + cell.rect.w &&
                y >= cell.rect.y && y < cell.rect.y + cell.rect.h) {
                *target += (*icons_)[cell.icon_index].token;
                if (on_change) on_change();
                return true;
            }
        }
    }

    for (const auto& k : keys_) {
        if (x >= k.rect.x && x < k.rect.x + k.rect.w && y >= k.rect.y && y < k.rect.y + k.rect.h) {
            apply_key(k);
            break;
        }
    }
    return true;
}

bool OnScreenKeyboard::handle_wheel(int x, int y, int delta) {
    if (!visible || !target || !contains(x, y)) return false;
    (void)delta;
    return true;
}

} // namespace osdedit
