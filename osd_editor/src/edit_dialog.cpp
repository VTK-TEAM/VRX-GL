#include "edit_dialog.h"

#include <sys/time.h>
#include <ctime>
#include <cstdlib>
#include "vt_telemetry_names.h"
#include <cstdio>

namespace osdedit {

TextField* ElementEditDialog::add_text_field(const std::string& label, const std::string& initial,
                                              std::function<void(const std::string&)> commit) {
    if (!row_visible(FIELD_H)) { cursor_y_ += FIELD_GAP; return nullptr; }
    TextField* tf = add<TextField>();
    tf->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, FIELD_H};
    tf->label_text = label;
    tf->value = initial;
    tf->on_request_focus = [this, tf, commit]() {
        if (request_keyboard_) request_keyboard_(tf, commit);
    };
    cursor_y_ += FIELD_GAP;
    return tf;
}

NumberField* ElementEditDialog::add_number_field(const std::string& label, float initial, float step,
                                                  float min_v, float max_v, int decimals,
                                                  std::function<void(float)> commit, float big_step) {
    if (!row_visible(FIELD_H)) { cursor_y_ += FIELD_GAP; return nullptr; }
    NumberField* nf = add<NumberField>();
    nf->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, FIELD_H};
    nf->label_text = label;
    nf->value = initial;
    nf->step = step;
    nf->big_step = (big_step > 0.f) ? big_step : (step * 10.f);
    nf->min_value = min_v;
    nf->max_value = max_v;
    nf->decimals = decimals;
    nf->on_change = commit;
    cursor_y_ += FIELD_GAP;
    return nf;
}

void ElementEditDialog::add_readonly_row(const std::string& label, const std::string& value) {
    UiLabel* l1 = add<UiLabel>();
    l1->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, 20};
    l1->text = label;
    l1->color = ui_color::TEXT_MUTED;

    UiLabel* l2 = add<UiLabel>();
    l2->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_ + 26, bounds.w - MARGIN * 2, FIELD_H};
    l2->text = value.empty() ? "(немає)" : value;
    l2->color = ui_color::TEXT;

    cursor_y_ += READONLY_ROW_GAP;
}

// Перемикач зроблено кнопкою, а не новим віджетом-галочкою: стан
// читається з самого напису ("так"/"ні" плюс колір), а нового класу з
// власним малюванням і потраплянням миші тут не окупалось би — це поки
// єдине двійкове поле в усьому редакторі.
void ElementEditDialog::add_toggle_row(const std::string& label, bool value,
                                       std::function<void(bool)> commit) {
    if (!row_visible(FIELD_H)) { cursor_y_ += FIELD_GAP; return; }

    UiLabel* l = add<UiLabel>();
    l->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_ - 20, bounds.w - MARGIN * 2, 18};
    l->text = label;
    l->color = ui_color::TEXT_MUTED;

    Button* b = add<Button>();
    b->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, FIELD_H};
    b->label = value ? "так — ховати елемент цілком" : "ні — показувати прочерки";
    b->bg_color = value ? ui_color::SUCCESS : ui_color::BG_LIGHT;
    b->text_color = value ? SDL_Color{15, 15, 15, 255} : ui_color::TEXT;
    b->on_click = [this, value, commit]() {
        commit(!value);
        request_rebuild();
    };
    cursor_y_ += FIELD_GAP;
}

void ElementEditDialog::add_section_title(const std::string& text) {
    UiLabel* l = add<UiLabel>();
    l->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, 24};
    l->text = text;
    l->color = ui_color::ACCENT;
    cursor_y_ += 38;
}

void ElementEditDialog::build_common_size_row() {
    UiLabel* l = add<UiLabel>();
    l->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, 120, 18};
    l->text = "Розмір:";
    l->color = ui_color::TEXT_MUTED;

    SizeIndexPicker* sp = add<SizeIndexPicker>();
    sp->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_ + 18, bounds.w - MARGIN * 2, FIELD_H};
    sp->selected = el_->size_index();
    OsdElement* el = el_;
    sp->on_change = [el](int idx) { el->set_size_index(idx); };
    cursor_y_ += FIELD_GAP;
}

void ElementEditDialog::build_label_fields() {
    OsdElement* el = el_;
    add_text_field("Текст / іконка (<ім'я> для іконки, можна змішувати з текстом)",
                   el->label(), [el](const std::string& v) { el->set_label(v); });
}

// ВИСТАВЛЕННЯ СИСТЕМНОГО ГОДИННИКА СТАНЦІЇ.
//
// Рішення відверто костильне, і це свідомо. Інтернету в полі немає,
// синхронізувати час нема з чим, а в записі й на екрані він потрібен: за
// ним потім розбирають політ. Городити окреме вікно налаштувань заради
// операції, яку роблять раз на кілька місяців, дорожче, ніж дописати
// півтора десятка рядків туди, де вже є і миша, і поля вводу.
//
// Прив'язка ДО КАНАЛУ, а не до нового типу елемента. Новий TYPE довелося
// б навчити розуміти й станцію, і писаря субтитрів, і схему конфігу — а
// в конфізі це лишається звичайним VALUE, який просто показує час.
// Редактор єдиний, кому тут потрібне щось більше.
static void apply_system_datetime(int hh, int mm, int dd, int mon, int yy) {
    std::time_t now = std::time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);

    lt.tm_hour = hh;
    lt.tm_min  = mm;
    lt.tm_sec  = 0;
    lt.tm_mday = dd;
    lt.tm_mon  = mon - 1;
    lt.tm_year = 100 + yy;          // yy=26 -> 2026
    lt.tm_isdst = -1;               // хай система сама розбереться з переходом

    const std::time_t t = mktime(&lt);
    if (t == (std::time_t)-1) {
        std::fprintf(stderr, "[годинник] неможлива дата, нічого не міняю\n");
        return;
    }
    struct timeval tv{};
    tv.tv_sec = t;
    if (settimeofday(&tv, nullptr) != 0) {
        std::fprintf(stderr, "[годинник] settimeofday не вдався (потрібен root)\n");
        return;
    }
    // У ГОДИННИК ПЛАТИ ТЕЖ. Без цього виставлений час живе до першого
    // перезавантаження — а станцію вимикають живленням, і саме після
    // цього час і потрібен правильний.
    if (std::system("hwclock --systohc >/dev/null 2>&1") != 0) {
        std::fprintf(stderr, "[годинник] системний час виставлено,"
                             " але в RTC записати не вдалось\n");
    }
    std::fprintf(stderr, "[годинник] встановлено %02d:%02d %02d.%02d.20%02d\n",
                 hh, mm, dd, mon, yy);
}

void ElementEditDialog::build_value_fields() {
    OsdElement* el = el_;
    add_readonly_row("Канал (не редагується):", vt_telemetry_channel_label(el->data_channel()));
    if (!el->meta().empty()) {
        add_readonly_row("Мета:", el->meta());
    }
    add_text_field("Лейбл-префікс (текст/<іконка>)",
                   el->label(), [el](const std::string& v) { el->set_label(v); });
    add_text_field("Одиниці (UNITS)", el->units(), [el](const std::string& v) { el->set_units(v); });
    if (!el->value_format().empty()) {
        // Формат не редагується: він властивість каналу, а не смак. Але
        // мовчати про нього не можна — інакше "чому тут двокрапка" не має
        // відповіді ніде.
        add_readonly_row("Формат (не редагується):", el->value_format());
    } else {
        add_number_field("Знаків після коми (DECIMALS)", static_cast<float>(el->decimals()), 1.f, 0.f, 6.f, 0,
                         [el](float v) { el->set_decimals(static_cast<int>(v)); });
    }

    add_toggle_row("Ховати, коли канал мовчить (HIDE_IF_NO_DATA)",
                   el->hide_if_no_data(),
                   [el](bool v) { el->set_hide_if_no_data(v); });

    // Годинник і дата станції — єдине місце, де їх можна виставити.
    const int ch = el->data_channel();
    if (ch == 211 || ch == 212) {
        std::time_t now = std::time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        clk_hh_ = lt.tm_hour; clk_mm_ = lt.tm_min;
        clk_dd_ = lt.tm_mday; clk_mon_ = lt.tm_mon + 1; clk_yy_ = lt.tm_year % 100;

        add_section_title("Системний годинник станції:");
        add_number_field("Година", (float)clk_hh_, 1.f, 0.f, 23.f, 0,
                         [this](float v) { clk_hh_ = (int)v; });
        add_number_field("Хвилина", (float)clk_mm_, 1.f, 0.f, 59.f, 0,
                         [this](float v) { clk_mm_ = (int)v; });
        add_number_field("День", (float)clk_dd_, 1.f, 1.f, 31.f, 0,
                         [this](float v) { clk_dd_ = (int)v; });
        add_number_field("Місяць", (float)clk_mon_, 1.f, 1.f, 12.f, 0,
                         [this](float v) { clk_mon_ = (int)v; });
        add_number_field("Рік (20xx)", (float)clk_yy_, 1.f, 24.f, 99.f, 0,
                         [this](float v) { clk_yy_ = (int)v; });

        Button* apply = add<Button>();
        apply->bounds = SDL_Rect{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2, FIELD_H};
        apply->label = "Встановити час і дату";
        apply->bg_color = ui_color::SUCCESS;
        apply->text_color = SDL_Color{15, 15, 15, 255};
        apply->on_click = [this]() {
            apply_system_datetime(clk_hh_, clk_mm_, clk_dd_, clk_mon_, clk_yy_);
            request_rebuild();
        };
        cursor_y_ += FIELD_GAP;
    }
}

void ElementEditDialog::build_enum_fields() {
    OsdElement* el = el_;
    add_readonly_row("Канал (не редагується):", vt_telemetry_channel_label(el->data_channel()));
    add_text_field("Лейбл-префікс (текст/<іконка>, опційно)",
                   el->label(), [el](const std::string& v) { el->set_label(v); });
    add_text_field("DEFAULT (якщо жоден CASE не збігся)",
                   el->enum_default(), [el](const std::string& v) { el->set_enum_default(v); });

    add_section_title("Умови (перевіряються по порядку зверху вниз):");

    // Область під умови — усе, що лишилось до низу діалогу. Саме її
    // висота і є те, у що список має вміститись; те, що не вміщається,
    // прокручується колесом.
    SDL_Rect list_area{bounds.x + MARGIN, cursor_y_, bounds.w - MARGIN * 2,
                       (bounds.y + bounds.h - MARGIN) - cursor_y_};
    if (list_area.h < FIELD_H) list_area.h = FIELD_H;
    rebuild_case_rows(list_area);
}

// Колесо над списком умов прокручує його. Перебудова після кожного
// кроку не марнотратство: рядків у видимій частині одиниці, а тримати
// їх усі створеними означало б тримати клікабельними й ті, що за межами
// діалогу.
bool ElementEditDialog::handle_wheel(int x, int y, int delta) {
    if (scroll_max_ > 0 && contains(x, y)) {
        scroll_ -= delta * FIELD_GAP;
        if (scroll_ < 0) scroll_ = 0;
        if (scroll_ > scroll_max_) scroll_ = scroll_max_;
        request_rebuild();
        return true;
    }
    return Panel::handle_wheel(x, y, delta);
}

// Чи потрапляє рядок висотою h у видиму частину діалогу. Рядок за межами
// не створюється взагалі: він однаково лишався б клікабельним, а видно
// його не було б.
bool ElementEditDialog::row_visible(int h) const {
    return cursor_y_ + h > content_top_ && cursor_y_ < bounds.y + bounds.h - MARGIN;
}

void ElementEditDialog::rebuild_case_rows(SDL_Rect list_area) {
    OsdElement* el = el_;
    auto cases = el->cases();

    // ПРОКРУТКА. Умов може бути скільки завгодно: у причин блокування арму
    // їх тридцять, і без прокрутки список ішов за низ діалогу, а далі й за
    // край екрана — разом із кнопкою "додати умову", тобто діалог ставав
    // непридатним рівно на тому елементі, заради якого й потрібен.
    //
    // Рядки, що не потрапляють у видиму область, НЕ СТВОРЮЮТЬСЯ взагалі.
    // Це не лише економія: віджет за межами діалогу однаково лишався б
    // клікабельним, і натиснути невидиму кнопку "X" було б легше, ніж
    // видиму.
    const int top = content_top_;
    const int bottom = bounds.y + bounds.h - MARGIN;
    auto visible = [&](int y) { return y + FIELD_H > top && y < bottom; };

    int row_y = list_area.y;
    for (size_t i = 0; i < cases.size(); ++i) {
        if (!visible(row_y)) { row_y += FIELD_GAP; continue; }
        EnumOpPicker* op = add<EnumOpPicker>();
        op->bounds = SDL_Rect{list_area.x, row_y, 36, FIELD_H};
        op->op_index = static_cast<int>(cases[i].op);
        size_t idx = i;
        op->on_change = [el, idx](int new_op) {
            auto cs = el->cases();
            if (idx < cs.size()) { cs[idx].op = static_cast<CompareOp>(new_op); el->set_cases(cs); }
        };

        NumberField* val = add<NumberField>();
        val->bounds = SDL_Rect{list_area.x + 42, row_y, 168, FIELD_H};
        val->value = cases[i].threshold;
        val->step = 0.1f;
        val->decimals = 2;
        val->on_change = [el, idx](float v) {
            auto cs = el->cases();
            if (idx < cs.size()) { cs[idx].threshold = v; el->set_cases(cs); }
        };

        TextField* lbl = add<TextField>();
        lbl->bounds = SDL_Rect{list_area.x + 216, row_y, list_area.w - 216 - 40, FIELD_H};
        lbl->value = cases[i].label;
        lbl->on_request_focus = [this, lbl, el, idx]() {
            if (request_keyboard_) {
                request_keyboard_(lbl, [el, idx](const std::string& v) {
                    auto cs = el->cases();
                    if (idx < cs.size()) { cs[idx].label = v; el->set_cases(cs); }
                });
            }
        };

        Button* del = add<Button>();
        del->bounds = SDL_Rect{list_area.x + list_area.w - 34, row_y, 34, FIELD_H};
        del->label = "X";
        del->bg_color = ui_color::DANGER;
        del->on_click = [this, el, idx]() {
            auto cs = el->cases();
            if (idx < cs.size()) {
                cs.erase(cs.begin() + static_cast<long>(idx));
                el->set_cases(cs);
            }
            request_rebuild();
        };

        row_y += FIELD_GAP;
    }

    cursor_y_ = row_y;
    if (!visible(row_y)) { cursor_y_ += FIELD_GAP; return; }
    Button* add_case = add<Button>();
    add_case->bounds = SDL_Rect{list_area.x, row_y, list_area.w, FIELD_H};
    add_case->label = "+ додати умову";
    add_case->bg_color = ui_color::BG_LIGHT;
    add_case->on_click = [this, el]() {
        auto cs = el->cases();
        EnumCaseView nc;
        nc.op = CompareOp::LT;
        nc.threshold = 0.f;
        nc.label = "";
        cs.push_back(nc);
        el->set_cases(cs);
        request_rebuild();
    };
    row_y += FIELD_GAP;

    cursor_y_ = row_y;
}

void ElementEditDialog::request_rebuild() {
    pending_rebuild_ = true;
}

void ElementEditDialog::draw(SDL_Renderer* renderer, TTF_Font* font) {
    if (pending_rebuild_) {
        pending_rebuild_ = false;
        // Клавіатура (якщо відкрита) може бути прив'язана до TextField
        // усередині ЦЬОГО діалогу — build() зараз знищить усі поточні
        // children_ (clear_children()), включно з тим TextField-ом.
        // Без цього виклику keyboard_.target лишився б висячим
        // вказівником. EditorApp прив'язує сюди hide_keyboard().
        if (on_before_rebuild) on_before_rebuild();
        build(bounds, el_, request_keyboard_, on_click_close_);
    }

    // ЗАГОЛОВОК НЕ ПРОКРУЧУЄТЬСЯ Й НЕ ОБРІЗАЄТЬСЯ, ВМІСТ — НАВПАКИ.
    //
    // Спершу я обрізав увесь діалог по його межі. Низ це полагодило, а
    // верх ні: вміст, прокручений угору, налазив на заголовок і на кнопку
    // "Готово" — вони ж усередині тієї самої межі.
    //
    // Тому діалог малюється у два проходи. Перші header_count_ дітей —
    // заголовок і "Готово" — це рамка: вони не рухаються й малюються
    // поверх. Решта це вміст: він живе у своєму прямокутнику й за нього не
    // виходить ні вгору, ні вниз.
    if (!visible) return;
    fill_rounded_rect(renderer, bounds, bg_color);
    if (draw_border) draw_rounded_rect_border(renderer, bounds, ui_color::BORDER);

    const SDL_Rect content{bounds.x, content_top_,
                           bounds.w, (bounds.y + bounds.h) - content_top_};
    SDL_Rect prev{};
    SDL_RenderGetClipRect(renderer, &prev);
    SDL_RenderSetClipRect(renderer, &content);
    for (size_t i = (size_t)header_count_; i < children().size(); ++i) {
        children()[i]->draw(renderer, font);
    }
    SDL_RenderSetClipRect(renderer, prev.w > 0 ? &prev : nullptr);

    for (size_t i = 0; i < (size_t)header_count_ && i < children().size(); ++i) {
        children()[i]->draw(renderer, font);
    }
}

void ElementEditDialog::build_bar_fields() {
    OsdElement* el = el_;
    add_readonly_row("Канал (не редагується):", vt_telemetry_channel_label(el->data_channel()));
    add_number_field("Мінімум (MIN)", el->bar_min(), 0.1f, -1e6f, 1e6f, 2,
                     [el](float v) { el->set_bar_min(v); }, /*big_step=*/1.0f);
    add_number_field("Максимум (MAX)", el->bar_max(), 0.1f, -1e6f, 1e6f, 2,
                     [el](float v) { el->set_bar_max(v); }, /*big_step=*/1.0f);
    add_number_field("Ширина бару (частка екрана)", el->bar_w(), 0.01f, 0.01f, 1.f, 3,
                     [el](float v) { el->set_bar_w(v); }, /*big_step=*/0.05f);
    add_number_field("Висота бару (частка екрана)", el->bar_h(), 0.005f, 0.005f, 1.f, 3,
                     [el](float v) { el->set_bar_h(v); }, /*big_step=*/0.02f);
    add_text_field("Шлях до фону (EMPTY_IMAGE)", el->bar_empty_image(),
                   [el](const std::string& v) { el->set_bar_empty_image(v); });
    add_text_field("Шлях до заповнення (FILL_IMAGE)", el->bar_fill_image(),
                   [el](const std::string& v) { el->set_bar_fill_image(v); });
}

void ElementEditDialog::build_horizon_fields() {
    OsdElement* el = el_;
    add_readonly_row("Канал (не редагується):", vt_telemetry_channel_label(el->data_channel()));
    add_text_field("Статичний шар (BASE_IMAGE)", el->horizon_base_image(),
                   [el](const std::string& v) { el->set_horizon_base_image(v); });
    add_text_field("Обертовий шар (POINTER_IMAGE)", el->horizon_pointer_image(),
                   [el](const std::string& v) { el->set_horizon_pointer_image(v); });
}

void ElementEditDialog::build(SDL_Rect area, OsdElement* el, KeyboardRequestFn request_keyboard,
                               std::function<void()> on_close) {
    clear_children();
    pending_rebuild_ = false;
    el_ = el;
    request_keyboard_ = request_keyboard;
    on_click_close_ = on_close;
    bounds = area;
    bg_color = ui_color::BG;

    // ПРОКРУТКА ВСЬОГО ДІАЛОГУ, а не окремо списку умов.
    //
    // Полів побільшало (формат, годинник із датою), і за нижній край
    // почали виходити вже не лише умови. Одна прокрутка на весь вміст
    // простіша за дві окремі й не має швів між ними.
    //
    // Заголовок і "Готово" лишаються на місці: вони не частина вмісту, а
    // рамка навколо нього.
    content_top_ = bounds.y + 52;
    cursor_y_ = content_top_ - scroll_;

    UiLabel* title = add<UiLabel>();
    title->bounds = SDL_Rect{bounds.x + MARGIN, bounds.y + 12, bounds.w - MARGIN * 2, 22};
    title->text = std::string("Редагування: ") + el_->key() +
                  "  [" + element_type_display_name(el_->type()) + "]";
    title->color = ui_color::TEXT;

    Button* close_btn = add<Button>();
    close_btn->bounds = SDL_Rect{bounds.x + bounds.w - 90, bounds.y + 8, 78, 30};
    close_btn->label = "Готово";
    close_btn->bg_color = ui_color::SUCCESS;
    close_btn->on_click = on_close;

    // Усе, що додано до цього рядка, — рамка діалогу: заголовок і
    // "Готово". Вони не прокручуються.
    header_count_ = (int)children().size();

    build_common_size_row();

    switch (el_->type()) {
        case ElementType::LABEL: build_label_fields(); break;
        case ElementType::VALUE: build_value_fields(); break;
        case ElementType::ENUM_SWITCH: build_enum_fields(); break;
        case ElementType::BAR: build_bar_fields(); break;
        case ElementType::HORIZON: build_horizon_fields(); break;
    }

    // Скільки вмісту вийшло й скільки з нього видно.
    content_h_ = (cursor_y_ + scroll_) - content_top_;
    const int view_h = (bounds.y + bounds.h - MARGIN) - content_top_;
    scroll_max_ = content_h_ - view_h;
    if (scroll_max_ < 0) scroll_max_ = 0;
    if (scroll_ > scroll_max_) { scroll_ = scroll_max_; request_rebuild(); }
}

} // namespace osdedit
