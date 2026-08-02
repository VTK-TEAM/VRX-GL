// edit_dialog.h — панель редагування ОДНОГО елемента. Набір полів
// будується ДИНАМІЧНО за el->type() (build_fields_for_type) — не
// окремий підклас на кожен тип, а один клас з фабричним методом,
// оскільки поля суттєво перетинаються (S є всюди, LABEL є в
// LABEL/VALUE/ENUM_SWITCH) і дублювання 5 майже однакових класів дало
// б менше користі, ніж один параметризований.
//
// L/T (позиція) тут НЕ редагується — позиція міняється перетягуванням
// прямо на канві (WYSIWYG). Канал (DATACHANNEL) — завжди read-only
// текст, без інпута: жорстко прив'язаний при додаванні з каталогу.
#pragma once

#include "widgets.h"
#include "osd_element.h"
#include <functional>

namespace osdedit {

class ElementEditDialog : public Panel {
public:
    // request_keyboard: викликається, коли текстове поле просить фокус —
    // діалог передає TextField* і callback commit(новий_текст), який
    // сам знає, у яке поле el_ це записати. EditorApp вирішує, ДЕ і ЯК
    // показати клавіатуру; сам діалог про клавіатуру нічого не знає.
    using KeyboardRequestFn = std::function<void(TextField*, std::function<void(const std::string&)>)>;

    void build(SDL_Rect area, OsdElement* el, KeyboardRequestFn request_keyboard,
               std::function<void()> on_close);

    // Викликається ПЕРЕД тим, як діалог сам себе перебудує (напр. після
    // додавання/видалення CASE — див. request_rebuild()). EditorApp
    // прив'язує сюди hide_keyboard(): якщо екранна клавіатура зараз
    // відкрита й прив'язана до TextField УСЕРЕДИНІ цього діалогу,
    // rebuild знищить той TextField (clear_children()), а keyboard_.target
    // лишиться висячим вказівником на звільнену пам'ять. Без цього хука
    // це реальний use-after-free, що чекає на клік "додати умову" з
    // відкритою клавіатурою.
    std::function<void()> on_before_rebuild;

    OsdElement* element() const { return el_; }

    void draw(SDL_Renderer* renderer, TTF_Font* font) override;

private:
    OsdElement* el_ = nullptr;
    KeyboardRequestFn request_keyboard_;
    std::function<void()> on_click_close_;

    // Відкладена перебудова: кнопки "+ додати умову"/"X" (видалити) не
    // викликають build() НАПРЯМУ зі свого on_click (це означало б
    // clear_children() посеред Panel::handle_mouse_down(), що саме
    // зараз ітерує/викликає ЦЕЙ САМИЙ Button — спрацьовувало лише
    // випадково, бо handle_mouse_down завжди повертає true одразу після
    // кліку і цикл більше не чіпає інвалідований ітератор). Натомість
    // вони просто виставляють прапорець; фактична перебудова
    // відбувається в draw() — гарантовано ПОЗА будь-яким стеком
    // обробки вхідних подій.
    bool pending_rebuild_ = false;
    void request_rebuild();

    int cursor_y_ = 0; // вертикальний "курсор" авто-компонування полів
    static constexpr int FIELD_H = 36;
    static constexpr int FIELD_GAP = 56;
    static constexpr int READONLY_ROW_GAP = 74;
    static constexpr int MARGIN = 16;

    TextField* add_text_field(const std::string& label, const std::string& initial,
                              std::function<void(const std::string&)> commit);
    NumberField* add_number_field(const std::string& label, float initial, float step,
                                  float min_v, float max_v, int decimals,
                                  std::function<void(float)> commit, float big_step = -1.f);
    void add_readonly_row(const std::string& label, const std::string& value);
    void add_section_title(const std::string& text);

    void build_common_size_row();
    void build_label_fields();
    void build_value_fields();
    void build_enum_fields();
    void build_bar_fields();
    void build_horizon_fields();

    void rebuild_case_rows(SDL_Rect list_area);
};

} // namespace osdedit
