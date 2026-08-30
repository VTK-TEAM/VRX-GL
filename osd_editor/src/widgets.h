// widgets.h — мінімальний, але справжній UI-тулкіт для редактора:
// базовий Widget + конкретні контроли. Все розраховано на МИШУ (клік,
// рух, колесо) — жодних гарячих клавіш, бо клавіатури немає (звідси й
// OnScreenKeyboard нижче).
//
// АРХІТЕКТУРА: Widget — абстрактний інтерфейс (draw/handle_*). Panel —
// контейнер, що володіє дочірніми Widget через unique_ptr і форвардить
// події. Конкретні контроли (Button, TextField, ...) — листові вузли.
// Це звичайна composite-структура, свідомо без over-engineering
// (жодних layout-менеджерів — координати задаються вручну при побудові
// діалогу, їх небагато).
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>

#include "osd_schema.h"

namespace osdedit {

// Спільна палітра — щоб усі контроли виглядали як один застосунок.
namespace ui_color {
constexpr SDL_Color BG          {28, 30, 36, 235};
constexpr SDL_Color BG_LIGHT    {40, 43, 51, 255};
constexpr SDL_Color BORDER      {70, 74, 84, 255};
constexpr SDL_Color BORDER_HL   {90, 180, 255, 255};
constexpr SDL_Color TEXT        {230, 230, 235, 255};
constexpr SDL_Color TEXT_MUTED  {150, 152, 160, 255};
constexpr SDL_Color ACCENT      {70, 150, 230, 255};
constexpr SDL_Color DANGER      {220, 70, 70, 255};
constexpr SDL_Color SUCCESS     {70, 190, 120, 255};
constexpr SDL_Color WARNING     {230, 160, 60, 255};
}

// ---------------------------------------------------------------- Widget

class Widget {
public:
    SDL_Rect bounds{0, 0, 0, 0};
    bool visible = true;
    bool enabled = true;

    virtual ~Widget() = default;
    virtual void draw(SDL_Renderer* renderer, TTF_Font* font) = 0;

    // Повертає true, якщо подія "спожита" цим віджетом (зупиняє
    // подальшу передачу батьківським контейнером за потреби).
    virtual bool handle_mouse_down(int x, int y) { (void)x; (void)y; return false; }
    virtual void handle_mouse_up(int x, int y) { (void)x; (void)y; }
    virtual void handle_mouse_motion(int x, int y) { (void)x; (void)y; }
    virtual bool handle_wheel(int x, int y, int delta) { (void)x; (void)y; (void)delta; return false; }

    bool contains(int x, int y) const {
        return x >= bounds.x && x < bounds.x + bounds.w &&
               y >= bounds.y && y < bounds.y + bounds.h;
    }
};

// Допоміжна функція малювання UI-тексту (TTF, НЕ атлас OSD-шрифту —
// той у canvas_renderer, для превʼю самого OSD; тут звичайний
// системний шрифт для меню/кнопок/полів, бо там потрібен повний
// UTF-8/кирилиця набір без обмежень атласу).
void draw_ui_text(SDL_Renderer* renderer, TTF_Font* font, const std::string& text,
                   int x, int y, SDL_Color color);
int measure_ui_text_width(TTF_Font* font, const std::string& text);

void fill_rounded_rect(SDL_Renderer* renderer, SDL_Rect r, SDL_Color color);
void draw_rounded_rect_border(SDL_Renderer* renderer, SDL_Rect r, SDL_Color color);

// ---------------------------------------------------------------- Button

class Button : public Widget {
public:
    std::string label;
    SDL_Color bg_color = ui_color::BG_LIGHT;
    SDL_Color text_color = ui_color::TEXT;
    std::function<void()> on_click;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;

private:
    bool pressed_ = false;
};

// ---------------------------------------------------------------- Label (нередаговний текст)

class UiLabel : public Widget {
public:
    std::string text;
    SDL_Color color = ui_color::TEXT;
    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
};

// ---------------------------------------------------------------- Panel (контейнер)

class Panel : public Widget {
public:
    SDL_Color bg_color = ui_color::BG;
    bool draw_border = true;

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        auto w = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = w.get();
        children_.push_back(std::move(w));
        return ptr;
    }

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
    void handle_mouse_up(int x, int y) override;
    void handle_mouse_motion(int x, int y) override;
    bool handle_wheel(int x, int y, int delta) override;

    std::vector<std::unique_ptr<Widget>>& children() { return children_; }
    void clear_children() { children_.clear(); }

protected:
    std::vector<std::unique_ptr<Widget>> children_;
};

// ---------------------------------------------------------------- NumberField
// Числове поле БЕЗ клавіатури — керується кнопками -/+ (і -10/+10 для
// швидкого кроку). Свідомий UX-вибір під "миша єдиний ввід": не треба
// відкривати екранну клавіатуру заради кожної цифри.

class NumberField : public Widget {
public:
    // Ширина однієї кнопки. Рахується від ширини поля, а не константою:
    // чотири кнопки по 26 пікселів це 104 зі 110, які поле має в рядку
    // умови, і значенню лишалось шість — воно малювалось по центру
    // ВСЬОГО поля, тобто поверх кнопок.
    int button_w() const;

public:
    std::string label_text;
    float value = 0.f;
    float step = 1.f;
    float big_step = 10.f;  // тепер РЕАЛЬНО використовується — див. widgets.cpp.
                             // Раніше поле існувало, але handle_mouse_down() його
                             // ніколи не читав: -/+ завжди робили лише крок `step`,
                             // тож дійти від 0 до, скажімо, 12.6 доводилось 126
                             // кліками. Тепер по краях кожної кнопки — по дві зони
                             // ("--"/"-" зліва, "+"/"++" справа).
    float min_value = -1e9f;
    float max_value = 1e9f;
    int decimals = 2;
    std::function<void(float)> on_change;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;

private:
    void apply_delta(float d);
};

// ---------------------------------------------------------------- SizeIndexPicker
// 5 кнопок (XS..XL) для size_index 0..4.

class SizeIndexPicker : public Widget {
public:
    int selected = 0;
    std::function<void(int)> on_change;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
};

// ---------------------------------------------------------------- EnumOpPicker
// 3-позиційний перемикач EQ / GT / LT (клік циклічно перемикає).

class EnumOpPicker : public Widget {
public:
    int op_index = 0; // 0=EQ,1=GT,2=LT
    std::function<void(int)> on_change;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
};

// ---------------------------------------------------------------- TextField
// Клік відкриває екранну клавіатуру (керується ззовні через
// on_request_focus — сам TextField нічого не знає про клавіатуру,
// EditorApp вирішує, де її показати).

class TextField : public Widget {
public:
    std::string label_text;
    std::string value;
    std::string placeholder;
    bool focused = false;
    std::function<void()> on_request_focus;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
};

// ---------------------------------------------------------------- ListView
// Простий вертикальний список кліковних рядків, з колесом миші для
// прокрутки. Використовується і для меню "+", і для списку CASES.

struct ListItem {
    std::string text;
    std::string subtitle; // додатковий сірий рядок під основним (опційно)

    // ЗАГОЛОВОК ГРУПИ. Не вибирається й малюється інакше: у каталозі
    // півтори сотні елементів, і без поділу на групи потрібний рядок
    // шукається прокруткою навмання.
    bool header = false;
};

class ListView : public Widget {
public:
    std::vector<ListItem> items;
    int row_height = 40;
    int scroll_offset = 0;
    std::function<void(int index)> on_select;

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
    bool handle_wheel(int x, int y, int delta) override;
};

// ---------------------------------------------------------------- OnScreenKeyboard
// EN (QWERTY) + UA (ЙЦУКЕН) + ICONS (грід доступних іконок атласу)
// розкладки, перемикач мови/режиму по колу, Shift, Backspace, Space,
// Done. Працює з ЗОВНІШНІМ буфером (target) — сам клас не знає, для
// якого TextField він відкритий; EditorApp прив'язує target =
// &якесь_поле.value перед показом.
//
// ICONS-режим НАВМИСНО не залежить від AtlasFont напряму (widgets.h —
// загальний UI-тулкіт без знання про специфіку OSD-атласу) — рендер
// прев'ю конкретної іконки інжектиться ззовні через IconDrawFn, той
// самий підхід dependency-inversion, що й KeyboardRequestFn у
// edit_dialog.h. Список іконок так само — просто вказівник на чужий
// vector (власник — EditorApp/IconCatalog), клавіатура нічого не копіює.

class OnScreenKeyboard : public Widget {
public:
    std::string* target = nullptr; // куди пишемо символи (nullptr = не активна)
    std::function<void()> on_done;
    std::function<void()> on_change; // викликається після КОЖНОЇ зміни тексту (не після SHIFT/LANG)

    // cell_rect — повний прямокутник клітинки; колбек сам вирішує як
    // позиціонувати прев'ю іконки всередині (по центру тощо).
    using IconDrawFn = std::function<void(SDL_Renderer*, const std::string& icon_name, const SDL_Rect& cell_rect)>;
    void set_icon_source(const std::vector<IconKeyInfo>* icons, IconDrawFn draw_fn) {
        icons_ = icons;
        icon_draw_fn_ = std::move(draw_fn);
    }

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;
    bool handle_mouse_down(int x, int y) override;
    bool handle_wheel(int x, int y, int delta) override;

    void bind(std::string* text_target) {
        target = text_target;
        shift_ = false;
        icon_scroll_row_ = 0;
    }

private:
    enum class Layer { EN, UA, NUM, ICONS };
    Layer layer_ = Layer::EN;
    bool shift_ = false;

    const std::vector<IconKeyInfo>* icons_ = nullptr;
    IconDrawFn icon_draw_fn_;
    int icon_scroll_row_ = 0;

    struct Key {
        SDL_Rect rect;
        std::string lower_en, upper_en, lower_ua, upper_ua;
        enum class Kind { CHAR, BACKSPACE, SPACE, SHIFT, LANG, DONE } kind = Kind::CHAR;
    };
    std::vector<Key> keys_; // літерні ряди (EN/UA) + функціональний ряд (усі шари)

    struct IconCell {
        SDL_Rect rect;
        int icon_index; // індекс в *icons_
    };
    std::vector<IconCell> icon_cells_; // лише в ICONS-шарі

    void rebuild_letter_rows();   // 3 ряди літер — лише для EN/UA
    void rebuild_function_row();  // LANG/SHIFT/SPACE/BACKSPACE/DONE — завжди
    void rebuild_icon_grid();     // ICON_COLS x 3 грід — лише для ICONS

    std::string key_label(const Key& k) const;
    void apply_key(const Key& k);
};

} // namespace osdedit
