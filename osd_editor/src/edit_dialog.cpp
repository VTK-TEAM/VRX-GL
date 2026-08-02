#include "edit_dialog.h"
#include "vt_telemetry_names.h"
#include <cstdio>

namespace osdedit {

TextField* ElementEditDialog::add_text_field(const std::string& label, const std::string& initial,
                                              std::function<void(const std::string&)> commit) {
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

void ElementEditDialog::build_value_fields() {
    OsdElement* el = el_;
    add_readonly_row("Канал (не редагується):", vt_telemetry_channel_label(el->data_channel()));
    if (!el->meta().empty()) {
        add_readonly_row("Мета:", el->meta());
    }
    add_text_field("Лейбл-префікс (текст/<іконка>)",
                   el->label(), [el](const std::string& v) { el->set_label(v); });
    add_text_field("Одиниці (UNITS)", el->units(), [el](const std::string& v) { el->set_units(v); });
    add_number_field("Знаків після коми (DECIMALS)", static_cast<float>(el->decimals()), 1.f, 0.f, 6.f, 0,
                     [el](float v) { el->set_decimals(static_cast<int>(v)); });
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
    case_list_area_ = list_area;
    rebuild_case_rows(list_area);
}

// Колесо над списком умов прокручує його. Перебудова після кожного
// кроку не марнотратство: рядків у видимій частині одиниці, а тримати
// їх усі створеними означало б тримати клікабельними й ті, що за межами
// діалогу.
bool ElementEditDialog::handle_wheel(int x, int y, int delta) {
    if (case_list_area_.w > 0 && case_scroll_max_ > 0 &&
        x >= case_list_area_.x && x < case_list_area_.x + case_list_area_.w &&
        y >= case_list_area_.y && y < case_list_area_.y + case_list_area_.h) {
        case_scroll_ -= delta * FIELD_GAP;
        if (case_scroll_ < 0) case_scroll_ = 0;
        if (case_scroll_ > case_scroll_max_) case_scroll_ = case_scroll_max_;
        request_rebuild();
        return true;
    }
    return Panel::handle_wheel(x, y, delta);
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
    const int content_h = (int)cases.size() * FIELD_GAP + FIELD_H;
    case_scroll_max_ = content_h - list_area.h;
    if (case_scroll_max_ < 0) case_scroll_max_ = 0;
    if (case_scroll_ > case_scroll_max_) case_scroll_ = case_scroll_max_;
    if (case_scroll_ < 0) case_scroll_ = 0;

    const int top = list_area.y;
    const int bottom = list_area.y + list_area.h;
    auto visible = [&](int y) { return y + FIELD_H > top && y < bottom; };

    int row_y = list_area.y - case_scroll_;
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

    if (!visible(row_y)) return;
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
    Panel::draw(renderer, font);
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

    cursor_y_ = bounds.y + 52;

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

    build_common_size_row();

    switch (el_->type()) {
        case ElementType::LABEL: build_label_fields(); break;
        case ElementType::VALUE: build_value_fields(); break;
        case ElementType::ENUM_SWITCH: build_enum_fields(); break;
        case ElementType::BAR: build_bar_fields(); break;
        case ElementType::HORIZON: build_horizon_fields(); break;
    }
}

} // namespace osdedit
