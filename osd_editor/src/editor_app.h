// editor_app.h — верхній рівень застосунку: SDL-вікно, головний цикл,
// стейт-машина взаємодії (просто клік / перетягування елемента /
// відкритий діалог редагування / меню додавання / підтвердження).
//
// СВІДОМО без generic "UI framework" з фокус-менеджером тощо — станів
// небагато і вони чітко взаємовиключні, простий enum Mode + явні
// перевірки в handle_event() читабельніші за абстрактний стек екранів
// заради 6 екранів.
#pragma once

#include <SDL.h>
#include <SDL_ttf.h>
#include <cstdlib>
#include <string>
#include <memory>

#include "atlas_font.h"
#include "image_cache.h"
#include "canvas_renderer.h"
#include "osd_document.h"
#include "osd_catalog.h"
#include "osd_icon_catalog.h"
#include "widgets.h"
#include "edit_dialog.h"
#include "vt_telemetry_storage.h"
#include "vt_telemetry_listener.hpp"

namespace osdedit {

class EditorApp {
public:
    ~EditorApp();

    // Всі шляхи asset-ів — ВІДНОСНІ (CWD), бо застосунок мусить
    // запускатись з ~/VRX і тягнути ТІ САМІ atlas.png/osd_glyph_info.bin/
    // osd_config.json, що генерує білдер і споживає прошивка. Каталог —
    // окремий файл поруч (osd_catalog.json), не частина прошивкових
    // asset-ів.
    bool init(const std::string& background_image_path);
    void run();

private:
    enum class Mode {
        IDLE,             // нічого не вибрано
        SELECTED,         // елемент вибраний, показані кнопки Edit/Delete
        DRAGGING,         // перетягування вибраного елемента мишею
        EDIT_OPEN,        // відкритий діалог редагування
        ADD_MENU_OPEN,    // відкрито меню каталогу "+"
        CONFIRM_DELETE,
        CONFIRM_SAVE,
    };

    bool init_sdl();
    bool load_assets(const std::string& background_image_path);
    void layout_chrome();

    void handle_event(const SDL_Event& e);
    void handle_mouse_down(int x, int y);
    void render_frame();

    void select_element(const std::string& key);
    void deselect();
    void start_drag(int mouse_x, int mouse_y);
    void update_drag(int mouse_x, int mouse_y);
    void end_drag();

    void open_edit_dialog();
    void close_edit_dialog();
    void open_add_menu();
    void close_add_menu();
    void add_element_from_catalog(int catalog_index);

    void request_delete_confirm();
    void request_save_confirm();
    void update_screen_button();
    void show_confirm(const std::string& text, std::function<void()> on_yes, Mode confirm_mode);

    void show_keyboard_for(TextField* field, std::function<void(const std::string&)> commit);
    void hide_keyboard();

    void set_status(const std::string& text, bool is_error);

    // --- SDL core ---
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* ui_font_ = nullptr;
    // Курсор малюємо самі — системного на голій станції немає.
    void draw_own_cursor();

    int window_w_ = 1280, window_h_ = 800;

    // Діагностика вводу: VRX_EDITOR_DEBUG_INPUT=1. Потрібна тому, що
    // "миша не працює" має щонайменше три різні причини, і
    // розрізняє їх лише те, чи доходять події взагалі.
    bool dbg_input_ = getenv("VRX_EDITOR_DEBUG_INPUT") != nullptr;
    int dbg_motion_ = 0, dbg_click_ = 0, dbg_key_ = 0;
    unsigned dbg_last_ = 0;
    bool running_ = true;

    // --- Дані/рендер OSD ---
    AtlasFont atlas_font_;
    std::unique_ptr<ImageCache> image_cache_;
    // ДВІ РОЗКЛАДКИ ТЕЛЕМЕТРІЇ — окремо для основного й додаткового
    // екрана станції. Редагується та, що обрана перемикачем; збереження
    // пише ОБИДВІ, бо вони одна пара й розходитись їм нема сенсу.
    static constexpr int kRoles = 2;
    OsdDocument document_[kRoles];
    int edit_role_ = 0;                 // 0 основний, 1 додатковий

    OsdDocument& doc() { return document_[edit_role_]; }
    const OsdDocument& doc() const { return document_[edit_role_]; }
    OsdCatalog catalog_;
    IconCatalog icon_catalog_; // osd_icon_names.json — живить ICONS-режим keyboard_
    std::unique_ptr<CanvasRenderer> canvas_renderer_;
    SDL_Texture* background_texture_ = nullptr;
    std::vector<ElementHitBox> last_hits_;

    // --- Live телеметрія (ті самі класи, що у VRX runtime) ---
    VtTelemetryStorage telemetry_storage_;
    VtTelemetryListener telemetry_listener_;
    bool telemetry_running_ = false;

    // --- Стан взаємодії ---
    Mode mode_ = Mode::IDLE;
    Mode mode_before_confirm_ = Mode::IDLE;
    std::string selected_key_;
    int drag_offset_x_ = 0, drag_offset_y_ = 0; // зсув кліку відносно L,T елемента (в пікселях канви)

    // --- Хром верхнього рівня ---
    Button save_button_;
    Button screen_button_;
    Button exit_button_;
    Button add_button_;
    Button edit_button_;
    Button delete_button_;

    ElementEditDialog edit_dialog_;

    Panel add_menu_panel_;
    ListView* add_list_view_ = nullptr;

    Panel confirm_panel_;
    UiLabel* confirm_label_ = nullptr;
    Button* confirm_yes_ = nullptr;
    Button* confirm_no_ = nullptr;
    std::function<void()> confirm_action_;

    OnScreenKeyboard keyboard_;
    bool keyboard_visible_ = false;

    std::string status_text_;
    bool status_is_error_ = false;
    Uint32 status_until_ms_ = 0;
};

} // namespace osdedit
