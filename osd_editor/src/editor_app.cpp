#include <algorithm>
#include <cmath>
#include <ctime>
#include "editor_app.h"
#include "vt_telemetry_names.h"
#include <cstdio>

namespace osdedit {

namespace {
// Дві розкладки телеметрії: основний екран станції й додатковий.
constexpr const char* kPrimaryConfig   = "osd_config.json";
constexpr const char* kSecondaryConfig = "osd_config2.json";
} // namespace


EditorApp::~EditorApp() {
    if (telemetry_running_) {
        telemetry_listener_.stop();
        telemetry_running_ = false;
    }

    if (background_texture_) SDL_DestroyTexture(background_texture_);
    image_cache_.reset(); // явно перед знищенням renderer_
    atlas_font_.destroy();
    if (ui_font_) TTF_CloseFont(ui_font_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
}

bool EditorApp::init_sdl() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0) {
        std::fprintf(stderr, "IMG_Init: %s\n", IMG_GetError());
        return false;
    }
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        return false;
    }

    // ПОВНИЙ ЕКРАН БЕЗ EWMH.
    //
    // SDL_WINDOW_FULLSCREEN_DESKTOP просить віконного менеджера
    // розгорнути вікно через _NET_WM_STATE. На станції без робочого стола
    // менеджера немає — прохання нікому виконати, вікно лишається
    // 1280x800, а SDL при цьому рапортує бажаний розмір як справжній.
    //
    // Ловилось довго й неприємно: вікно 1366x768, рендерер 1366x768,
    // viewport 1366x768, полотно 1366x768 — усі числа правильні, а на
    // екрані 86 пікселів чорноти справа й обрізана права колонка OSD.
    // Малювалось у буфер 1280x800, бо саме таким лишилось вікно.
    //
    // Тому не просимо нікого: беремо розмір екрана й створюємо вікно рівно
    // таким, без рамки, у лівому верхньому куті. Без менеджера вікон це і
    // Є повний екран, і жодного посередника в цьому немає.
    Uint32 win_flags = SDL_WINDOW_RESIZABLE;
    const char* fs = std::getenv("VRX_EDITOR_FULLSCREEN");
    const bool want_fs = fs && *fs && *fs != '0';
    int win_x = SDL_WINDOWPOS_CENTERED, win_y = SDL_WINDOWPOS_CENTERED;
    if (want_fs) {
        SDL_Rect b{};
        if (SDL_GetDisplayBounds(0, &b) == 0 && b.w > 0 && b.h > 0) {
            window_w_ = b.w;
            window_h_ = b.h;
            win_x = b.x;
            win_y = b.y;
            win_flags = SDL_WINDOW_BORDERLESS;
        } else {
            // Не дізнались розміру — хай хоч менеджер спробує.
            win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
    }

    window_ = SDL_CreateWindow("OSD Editor for VRX", win_x, win_y,
                               window_w_, window_h_, win_flags);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    if (want_fs) {
        SDL_RaiseWindow(window_);
        // Фокус клавіатури без менеджера вікон не ставить ніхто.
        SDL_SetWindowInputFocus(window_);
    }

    // СОФТВЕРНИЙ РЕНДЕРЕР НАВМИСНО. Редактор — конфіг-інструмент, не гра,
    // і йому потрібні ПРАВИЛЬНІ кольори, а не швидкість. Акселерований
    // бекенд на релізній машині (libmali GLES2 під голим X) плутав порядок
    // каналів — картинка виходила з переставленими R/G. Софт рендерить
    // блітами поверхонь, формат яких SDL знає точно, тож кольори однакові
    // на будь-якому драйвері. Кадрів тут одиниці на дію — швидкості
    // вистачає з головою. Акселерований лишаємо запасним, якщо софту раптом
    // немає (не має статись).
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_RendererInfo ri;
    if (SDL_GetRendererInfo(renderer_, &ri) == 0)
        std::fprintf(stderr, "[редактор] рендерер: %s\n", ri.name);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    // UI-шрифт: спершу пробуємо той самий Inter, яким атлас-білдер
    // генерує самі OSD-гліфи (гарантовано лежить поруч у ~/VRX/OSD_ATLAS/
    // fonts/, якщо застосунок запущено звідти) — тематично узгоджено.
    // Якщо його нема (напр. тестовий запуск не з VRX) — пробуємо типові
    // системні шрифти як fallback.
    static const char* FONT_CANDIDATES[] = {
        "OSD_ATLAS/fonts/Inter_24pt-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (const char* path : FONT_CANDIDATES) {
        ui_font_ = TTF_OpenFont(path, 16);
        if (ui_font_) break;
    }
    if (!ui_font_) {
        std::fprintf(stderr, "Не знайдено жодного UI-шрифту (пробував OSD_ATLAS/fonts/Inter_24pt-Bold.ttf "
                             "та типові системні шляхи). Постав будь-який .ttf і поправ FONT_CANDIDATES.\n");
        return false;
    }
    return true;
}

bool EditorApp::load_assets(const std::string& background_image_path) {
    // ВСІ шляхи тут — ВІДНОСНІ (CWD), НАВМИСНО без /opt/vrx/... чи
    // окремої assets/-теки: застосунок мусить тягнути РІВНО ті самі
    // atlas.png/osd_glyph_info.bin/osd_config.json, що лежать поруч з
    // рештою VRX-репозиторію, а не власну копію. Тому редактор
    // ЗАПУСКАЄТЬСЯ З ~/VRX (той самий CWD-гачок, що і в основній
    // прошивці — див. build-скрипти).
    std::string err;
    if (!atlas_font_.load(renderer_, "atlas.png", "osd_glyph_info.bin", &err)) {
        std::fprintf(stderr, "Атлас: %s\n"
                             "  Підказка: редактор мусить запускатись з ~/VRX (cd ~/VRX && ./osd_editor ...),\n"
                             "  бо atlas.png і osd_glyph_info.bin шукаються ВІДНОСНО поточної директорії.\n",
                     err.c_str());
        return false;
    }

    // ОСНОВНА розкладка — той самий файл, що й був. Її наявність
    // обов'язкова: без неї редагувати нічого.
    try {
        document_[0].load(kPrimaryConfig);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", kPrimaryConfig, e.what());
        return false;
    }

    // ДОДАТКОВА — окремий файл. Якщо його ще немає (а на станціях, які
    // жили з одним екраном, його немає завжди), беремо КОПІЮ основної,
    // а не порожнечу: інакше оператор, увімкнувши OSD на другому екрані,
    // побачив би чисте поле й вирішив, що зламалось. Файл з'явиться на
    // диску при першому ж збереженні.
    try {
        document_[1].load(kSecondaryConfig);
    } catch (const std::exception&) {
        document_[1] = document_[0];
        document_[1].set_path(kSecondaryConfig);
        std::fprintf(stderr, "%s не знайдено — почав із копії основної\n", kSecondaryConfig);
    }

    try {
        catalog_.load("osd_catalog.json");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "osd_catalog.json: %s\n", e.what());
        return false;
    }

    background_texture_ = IMG_LoadTexture(renderer_, background_image_path.c_str());
    if (!background_texture_) {
        std::fprintf(stderr, "Фонове фото \"%s\" не завантажилось (%s) — продовжую БЕЗ фону "
                             "(буде суцільний колір замість фото польоту).\n",
                     background_image_path.c_str(), IMG_GetError());
        // НЕ фатально — редагувати layout можна і без реального фону.
    }

    image_cache_ = std::make_unique<ImageCache>(renderer_);
    canvas_renderer_ = std::make_unique<CanvasRenderer>(atlas_font_);
    canvas_renderer_->set_background(background_texture_);

    // osd_icon_names.json — НЕ фатально, якщо відсутній (генерує його
    // ще не всі версії osd_atlas_builder). Без нього ICONS-режим
    // клавіатури просто буде порожній з підказкою прямо в UI.
    std::string icon_err;
    if (!icon_catalog_.load("osd_icon_names.json", &icon_err)) {
        std::fprintf(stderr, "osd_icon_names.json: %s — ICONS-режим клавіатури буде порожній "
                             "(іконки все одно можна вписати вручну як <ім'я>)\n", icon_err.c_str());
    } else {
        std::fprintf(stderr, "Завантажено %zu іконок для клавіатури\n", icon_catalog_.entries().size());
    }
    // draw_icon() малює під ФІКСОВАНИЙ size_index (1 = "S") — без
    // довільного масштабування під розмір клітинки клавіатури, бо
    // AtlasFont зараз не вміє інакше (те саме обмеження, що й у
    // canvas_renderer для BAR/HORIZON плейсхолдерів). Досить для
    // впізнаваного прев'ю, не пікселpoints-perfect підгонки під cell.
    keyboard_.set_icon_source(&icon_catalog_.entries(),
        [this](SDL_Renderer* r, const std::string& name, const SDL_Rect& cell) {
            constexpr int kSizeIndex = 1;
            uint32_t size_offset = static_cast<uint32_t>(kSizeIndex) * SIZE_STEP;
            uint32_t code = CUSTOM_ICON_BASE + simple_name_hash(name) + size_offset;
            const GlyphInfo* g = atlas_font_.find_glyph(code);

            int x = cell.x + 4;
            int y = cell.y + 2;
            if (g) {
                int iw = static_cast<int>(g->width * atlas_font_.atlas_w() + 0.5f);
                int ih = static_cast<int>(g->height * atlas_font_.atlas_h() + 0.5f);
                if (iw < 1) iw = 1;
                if (ih < 1) ih = 1;
                x = cell.x + (cell.w - iw) / 2;
                y = cell.y + (cell.h - ih) / 2;
            }
            atlas_font_.draw_icon(r, name, x, y, /*size_index=*/kSizeIndex);
        });

    return true;
}

bool EditorApp::init(const std::string& background_image_path) {
    if (!init_sdl()) return false;
    if (!load_assets(background_image_path)) return false;

    // Той самий новий протокол (broadcast UDP :50122), що й у runtime VRX —
    // POINT+STATION обидва шлють сюди, ніякого host/port конфігу більше
    // не треба (VtTelemetryListener).
    telemetry_running_ = telemetry_listener_.start(telemetry_storage_, /*port=*/50122);
    if (!telemetry_running_) {
        std::fprintf(stderr, "VtTelemetryListener: не вдалося запустити на порту 50122 "
                             "— редактор працюватиме без live-даних\n");
    }

    // Без цього видалення/додавання CASE в ENUM-діалозі, зроблене з
    // відкритою екранною клавіатурою (напр. клавіатура ще прив'язана
    // до текстового поля CASE-лейбла), лишало б keyboard_.target
    // висячим вказівником після rebuild — див. коментар біля
    // ElementEditDialog::on_before_rebuild.
    edit_dialog_.on_before_rebuild = [this]() { hide_keyboard(); };

    layout_chrome();
    return true;
}

void EditorApp::layout_chrome() {
    // ТРИ КНОПКИ КОЛОНКОЮ ЗЛІВА, ПОСЕРЕДИНІ ВИСОТИ, ПОВЕРХ УСЬОГО.
    //
    // Не смуга зверху, бо смуга з'їдає частину екрана — а екран тут і є
    // предмет роботи: розкладку роблять по тому, що видно, і будь-яка
    // рамка навколо неї означає, що бачиш не те, що буде в польоті.
    //
    // Кнопки маленькі й напівпрозорі за рахунок розміру: вони перекривають
    // край картинки, але цей край майже завжди порожній, а перекладати їх
    // кудись означало б знову забирати місце.
    const int bw = 92, bh = 34, gap = 8, x = 10;
    const int total = bh * 4 + gap * 3;
    int y = (window_h_ - total) / 2;
    if (y < 8) y = 8;

    add_button_.bounds = SDL_Rect{x, y, bw, bh};
    add_button_.label = "+ додати";
    add_button_.bg_color = ui_color::ACCENT;
    add_button_.on_click = [this]() { open_add_menu(); };

    save_button_.bounds = SDL_Rect{x, y + bh + gap, bw, bh};
    save_button_.label = "Зберегти";
    save_button_.bg_color = ui_color::SUCCESS;
    save_button_.text_color = SDL_Color{15, 15, 15, 255};
    save_button_.on_click = [this]() { request_save_confirm(); };

    // ВИХІД. Раніше його не було взагалі: редактор жив у вікні з рамкою,
    // і закривали його хрестиком віконного менеджера. На станції без
    // робочого стола ні рамки, ні менеджера немає — вийти було б нічим.
    // ПЕРЕМИКАЧ ЕКРАНА. Міняє редаговану розкладку НА ГАРЯЧУ: полотно
    // одразу показує телеметрію того екрана, який обрано. Обидві живуть
    // у пам'яті, тож перемикання нічого не читає з диска й не втрачає
    // незбережених правок.
    screen_button_.bounds = SDL_Rect{x, y + (bh + gap) * 2, bw, bh};
    screen_button_.on_click = [this]() {
        edit_role_ = (edit_role_ + 1) % kRoles;
        selected_key_.clear();          // вибір належав іншій розкладці
        update_screen_button();
        set_status(edit_role_ == 0 ? "Редагую ОСНОВНИЙ екран"
                                   : "Редагую ДОДАТКОВИЙ екран", false);
    };
    update_screen_button();

    exit_button_.bounds = SDL_Rect{x, y + (bh + gap) * 3, bw, bh};
    exit_button_.label = "Вихід";
    exit_button_.bg_color = ui_color::DANGER;
    exit_button_.on_click = [this]() { running_ = false; };

    edit_button_.label = "Змінити";
    edit_button_.bg_color = ui_color::ACCENT;
    edit_button_.on_click = [this]() { open_edit_dialog(); };

    delete_button_.label = "Видалити";
    delete_button_.bg_color = ui_color::DANGER;
    delete_button_.on_click = [this]() { request_delete_confirm(); };

    // ПОЛОТНО — УВЕСЬ ЕКРАН.
    //
    // Було: letterbox під пропорцію фонового фото, з полями навколо.
    // Сенс у цьому був, поки редактор жив у вікні довільної форми на
    // робочому столі. На станції він працює на повний екран того самого
    // монітора, на якому потім працює станція, — отже полотно і є екран,
    // один в один. Будь-яке поле навколо тут не уточнює картинку, а
    // спотворює її: елемент, поставлений у кут полотна, у польоті
    // опиниться не в куті екрана.
    if (canvas_renderer_) {
        canvas_renderer_->set_canvas_rect(SDL_Rect{0, 0, window_w_, window_h_});
    }
}

void EditorApp::run() {
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (dbg_input_) {
                if (e.type == SDL_MOUSEMOTION) dbg_motion_++;
                else if (e.type == SDL_MOUSEBUTTONDOWN) dbg_click_++;
                else if (e.type == SDL_KEYDOWN) dbg_key_++;
            }
            handle_event(e);
        }
        if (dbg_input_ && SDL_GetTicks() - dbg_last_ > 2000) {
            dbg_last_ = SDL_GetTicks();
            int mx = 0, my = 0;
            const Uint32 btn = SDL_GetMouseState(&mx, &my);
            int ww = 0, wh = 0, rw = 0, rh = 0, lw = 0, lh = 0;
            SDL_Rect vp{};
            SDL_RenderGetViewport(renderer_, &vp);
            SDL_RenderGetLogicalSize(renderer_, &lw, &lh);
            SDL_GetWindowSize(window_, &ww, &wh);
            SDL_GetRendererOutputSize(renderer_, &rw, &rh);
            std::fprintf(stderr,
                "[ввід] рух %d, кліків %d, клавіш %d | миша (%d,%d) кнопки %u"
                " | вікно %dx%d, полотно рендера %dx%d, розкладку рахували під %dx%d"
                " | полотно %d,%d %dx%d | viewport %d,%d %dx%d | логічний %dx%d | курсор %s\n",
                dbg_motion_, dbg_click_, dbg_key_, mx, my, btn,
                ww, wh, rw, rh, window_w_, window_h_,
                canvas_renderer_ ? canvas_renderer_->canvas_rect().x : -1,
                canvas_renderer_ ? canvas_renderer_->canvas_rect().y : -1,
                canvas_renderer_ ? canvas_renderer_->canvas_rect().w : -1,
                canvas_renderer_ ? canvas_renderer_->canvas_rect().h : -1,
                vp.x, vp.y, vp.w, vp.h, lw, lh,
                SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE ? "увімкнено" : "ВИМКНЕНО");
        }
        render_frame();
        SDL_Delay(10);
    }
}

void EditorApp::handle_event(const SDL_Event& e) {
    switch (e.type) {
    case SDL_QUIT:
        running_ = false;
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT) handle_mouse_down(e.button.x, e.button.y);
        break;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT && mode_ == Mode::DRAGGING) end_drag();
        break;
    case SDL_MOUSEMOTION:
        if (mode_ == Mode::DRAGGING) update_drag(e.motion.x, e.motion.y);
        break;
    case SDL_MOUSEWHEEL: {
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        if (mode_ == Mode::ADD_MENU_OPEN) add_menu_panel_.handle_wheel(mx, my, e.wheel.y);
        else if (mode_ == Mode::EDIT_OPEN) edit_dialog_.handle_wheel(mx, my, e.wheel.y);
        break;
    }
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_RESIZED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            window_w_ = e.window.data1;
            window_h_ = e.window.data2;
            layout_chrome();
        }
        break;
    default:
        break;
    }
}

void EditorApp::handle_mouse_down(int x, int y) {
    if (keyboard_visible_) {
        if (keyboard_.handle_mouse_down(x, y)) {
            return;
        }
        hide_keyboard();
    }
    if (mode_ == Mode::EDIT_OPEN) { edit_dialog_.handle_mouse_down(x, y); return; }
    if (mode_ == Mode::ADD_MENU_OPEN) {
        if (!add_menu_panel_.handle_mouse_down(x, y)) close_add_menu();
        return;
    }
    if (mode_ == Mode::CONFIRM_DELETE || mode_ == Mode::CONFIRM_SAVE) {
        confirm_panel_.handle_mouse_down(x, y);
        return;
    }

    if (save_button_.handle_mouse_down(x, y)) return;
    if (add_button_.handle_mouse_down(x, y)) return;
    if (screen_button_.handle_mouse_down(x, y)) return;
    if (exit_button_.handle_mouse_down(x, y)) return;

    if (mode_ == Mode::SELECTED) {
        if (edit_button_.handle_mouse_down(x, y)) return;
        if (delete_button_.handle_mouse_down(x, y)) return;
    }

    // Хіт-тест по канві: йдемо з КІНЦЯ списку (останній намальований =
    // візуально зверху), щоб клікалось на те, що видно, а не на те, що
    // намальовано першим і потім перекрито.
    for (auto it = last_hits_.rbegin(); it != last_hits_.rend(); ++it) {
        if (x >= it->rect.x && x < it->rect.x + it->rect.w &&
            y >= it->rect.y && y < it->rect.y + it->rect.h) {
            select_element(it->key);
            start_drag(x, y);
            return;
        }
    }
    deselect();
}

void EditorApp::select_element(const std::string& key) {
    selected_key_ = key;
    mode_ = Mode::SELECTED;
}

void EditorApp::deselect() {
    selected_key_.clear();
    mode_ = Mode::IDLE;
}

void EditorApp::start_drag(int mouse_x, int mouse_y) {
    OsdElement* el = doc().find_by_key(selected_key_);
    if (!el) { mode_ = Mode::IDLE; return; }
    int ex, ey;
    canvas_renderer_->canvas_norm_to_screen(el->l(), el->t(), &ex, &ey);
    drag_offset_x_ = mouse_x - ex;
    drag_offset_y_ = mouse_y - ey;
    mode_ = Mode::DRAGGING;
}

void EditorApp::update_drag(int mouse_x, int mouse_y) {
    if (mode_ != Mode::DRAGGING) return;
    OsdElement* el = doc().find_by_key(selected_key_);
    if (!el) return;
    SDL_Rect cr = canvas_renderer_->canvas_rect();
    if (cr.w <= 0 || cr.h <= 0) return;
    // НЕ через screen_to_canvas_norm (та відхиляє точки поза канвою) —
    // під час перетягування миша легко на мить вилазить за край, а
    // set_position() і так клемпить у 0..1, тож рахуємо напряму.
    float nx = static_cast<float>((mouse_x - drag_offset_x_) - cr.x) / static_cast<float>(cr.w);
    float ny = static_cast<float>((mouse_y - drag_offset_y_) - cr.y) / static_cast<float>(cr.h);

    // ПРИВʼЯЗКА ДО СІТКИ В 1%. Позиція нормована, тож крок однаковий по
    // обох осях у частках екрана — а не в пікселях, які на 1920 і на 1366
    // різні. Без неї два елементи в один стовпчик мишею не поставити:
    // піксель канви це десь 0.0007 частки, і "рівно" виходило лише
    // випадково. 1% на FullHD — 19 пікселів по горизонталі; дрібніше
    // мишею однаково не поціляють, а рядок гліфів заввишки вдвічі більший.
    nx = std::round(nx * 100.f) / 100.f;
    ny = std::round(ny * 100.f) / 100.f;
    el->set_position(nx, ny);
}

void EditorApp::end_drag() {
    if (mode_ == Mode::DRAGGING) mode_ = Mode::SELECTED;
}

void EditorApp::open_edit_dialog() {
    OsdElement* el = doc().find_by_key(selected_key_);
    if (!el) { deselect(); return; }
    SDL_Rect area{window_w_ / 2 - 320, 50, 640, window_h_ - 100};
    edit_dialog_.build(
        area, el,
        [this](TextField* f, std::function<void(const std::string&)> commit) {
            show_keyboard_for(f, commit);
        },
        [this]() { close_edit_dialog(); });
    mode_ = Mode::EDIT_OPEN;
}

void EditorApp::close_edit_dialog() {
    hide_keyboard();
    mode_ = Mode::SELECTED;
}

void EditorApp::open_add_menu() {
    add_menu_panel_.clear_children();
    SDL_Rect area{window_w_ / 2 - 280, 50, 560, window_h_ - 100};
    add_menu_panel_.bounds = area;
    add_menu_panel_.bg_color = ui_color::BG;

    UiLabel* title = add_menu_panel_.add<UiLabel>();
    title->bounds = SDL_Rect{area.x + 16, area.y + 12, area.w - 32, 24};
    title->text = "Додати елемент з каталогу:";
    title->color = ui_color::TEXT;

    Button* close_btn = add_menu_panel_.add<Button>();
    close_btn->bounds = SDL_Rect{area.x + area.w - 90, area.y + 8, 78, 30};
    close_btn->label = "Закрити";
    close_btn->on_click = [this]() { close_add_menu(); };

    add_list_view_ = add_menu_panel_.add<ListView>();
    add_list_view_->bounds = SDL_Rect{area.x + 16, area.y + 52, area.w - 32, area.h - 70};
    add_list_view_->row_height = 46;
    const auto& entries = catalog_.entries();

    // СПИСОК ЗА ГРУПАМИ. Півтори сотні елементів суцільним рядком — це
    // прокрутка навмання: щоб знайти "причину блокування арму", треба або
    // пам'ятати її назву, або перебрати все. Заголовок групи дає точку
    // опори, а порядок груп задає сам каталог.
    //
    // visible_to_catalog зіставляє рядок списку з елементом каталогу:
    // заголовки теж займають рядки, тож пряма відповідність індексів тут
    // не працює. Для заголовка ставимо -1.
    std::vector<int> visible_to_catalog;
    visible_to_catalog.reserve(entries.size() + 16);

    auto add_row = [&](const CatalogEntry& e, int catalog_idx) {
        ListItem item;
        item.text = e.display_name;
        item.subtitle = vt_telemetry_channel_label(e.tpl.value("DATACHANNEL", -1));
        add_list_view_->items.push_back(item);
        visible_to_catalog.push_back(catalog_idx);
    };

    // Простий напис — завжди першим рядком, поза групами: він потрібен
    // найчастіше й не належить жодній телеметрії.
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (entries[static_cast<size_t>(i)].key_prefix == "custom_label") {
            add_row(entries[static_cast<size_t>(i)], i);
            break;
        }
    }

    std::vector<std::string> order = catalog_.groups();
    if (order.empty()) {                       // каталог без "groups"
        for (const auto& e : entries) {
            if (!e.group.empty() &&
                std::find(order.begin(), order.end(), e.group) == order.end()) {
                order.push_back(e.group);
            }
        }
    }

    for (const std::string& g : order) {
        bool header_added = false;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            const auto& e = entries[static_cast<size_t>(i)];
            if (e.group != g) continue;
            if (e.key_prefix == "custom_icon" || e.key_prefix == "custom_label") continue;
            if (!header_added) {
                ListItem h;
                h.text = g;
                h.header = true;
                add_list_view_->items.push_back(h);
                visible_to_catalog.push_back(-1);
                header_added = true;
            }
            add_row(e, i);
        }
    }

    add_list_view_->on_select = [this, visible_to_catalog](int idx) {
        if (idx < 0 || idx >= static_cast<int>(visible_to_catalog.size())) {
            return;
        }
        if (visible_to_catalog[static_cast<size_t>(idx)] < 0) return;   // заголовок
        // ВАЖЛИВО: спершу закрити меню (воно виставляє mode_=IDLE), а
        // ПОТІМ додати елемент (виставляє mode_=SELECTED) — інакше
        // close_add_menu() безумовно затирає щойно встановлений
        // SELECTED назад на IDLE, і кнопки Edit/Delete для щойно
        // доданого елемента не з'являються.
        close_add_menu();
        add_element_from_catalog(visible_to_catalog[static_cast<size_t>(idx)]);
    };

    mode_ = Mode::ADD_MENU_OPEN;
}

void EditorApp::close_add_menu() { mode_ = Mode::IDLE; }

void EditorApp::add_element_from_catalog(int catalog_index) {
    const auto& entries = catalog_.entries();
    if (catalog_index < 0 || catalog_index >= static_cast<int>(entries.size())) return;
    const auto& entry = entries[static_cast<size_t>(catalog_index)];
    std::string key = doc().make_unique_key(entry.key_prefix);
    // Новий елемент завжди в центрі канви (0.5, 0.5) — користувач одразу
    // перетягне його мишею, куди треба; так простіше, ніж вгадувати
    // позицію кліку по кнопці "+" (та в кутку, а не на канві).
    OsdElement el = OsdElement::create_from_template(key, entry.tpl, 0.5f, 0.5f);
    doc().add(std::move(el));
    select_element(key);
    set_status("Додано: " + key, false);
}

void EditorApp::request_delete_confirm() {
    std::string key = selected_key_;
    show_confirm("Точно видалити елемент \"" + key + "\"?",
                 [this, key]() {
                     doc().remove_by_key(key);
                     deselect();
                     set_status("Видалено: " + key, false);
                 },
                 Mode::CONFIRM_DELETE);
}

// Підпис і колір кнопки показують, ЯКУ розкладку зараз видно на полотні.
void EditorApp::update_screen_button() {
    // Підписи однакової довжини з сусідніми кнопками ("+ додати",
    // "Зберегти") — довше в 92 пікселі не влазить. "Додатковий" не
    // вмістився, тож пара стала "Основний / Другий": кнопка має рівно два
    // стани, і сплутати їх нема з чим.
    screen_button_.label = (edit_role_ == 0) ? "Основний" : "Другий";
    screen_button_.bg_color = (edit_role_ == 0) ? ui_color::ACCENT : ui_color::WARNING;
}

void EditorApp::request_save_confirm() {
    // ЗБЕРІГАЄМО ОБИДВІ РОЗКЛАДКИ ОДРАЗУ.
    //
    // Вони одна пара: перемикач між екранами діє на гарячу, і людина
    // цілком може поправити основний, перемкнутись, поправити додатковий
    // — і натиснути "Зберегти" один раз. Питати окремо про кожен файл
    // означало б або губити половину правок, або двічі перепитувати.
    show_confirm("Зберегти обидві розкладки — основну й додаткову?",
                 [this]() {
                     std::string done, failed;
                     for (int r = 0; r < kRoles; ++r) {
                         try {
                             document_[r].save();
                             if (!done.empty()) done += ", ";
                             done += document_[r].path();
                         } catch (const std::exception& e) {
                             if (!failed.empty()) failed += "; ";
                             failed += document_[r].path() + ": " + e.what();
                         }
                     }
                     if (failed.empty()) {
                         set_status("Збережено: " + done, false);
                     } else {
                         set_status("Помилка збереження: " + failed, true);
                     }
                 },
                 Mode::CONFIRM_SAVE);
}

void EditorApp::show_confirm(const std::string& text, std::function<void()> on_yes, Mode confirm_mode) {
    mode_before_confirm_ = mode_;
    confirm_action_ = on_yes;

    confirm_panel_.clear_children();
    confirm_panel_.bounds = SDL_Rect{window_w_ / 2 - 230, window_h_ / 2 - 80, 460, 160};
    confirm_panel_.bg_color = ui_color::BG;

    confirm_label_ = confirm_panel_.add<UiLabel>();
    confirm_label_->text = text;
    confirm_label_->bounds = SDL_Rect{confirm_panel_.bounds.x + 16, confirm_panel_.bounds.y + 20,
                                      confirm_panel_.bounds.w - 32, 60};
    confirm_label_->color = ui_color::TEXT;

    confirm_yes_ = confirm_panel_.add<Button>();
    confirm_yes_->label = "Так";
    confirm_yes_->bg_color = ui_color::DANGER;
    confirm_yes_->bounds = SDL_Rect{confirm_panel_.bounds.x + 40, confirm_panel_.bounds.y + 100, 180, 42};
    confirm_yes_->on_click = [this]() {
        if (confirm_action_) confirm_action_();
        mode_ = Mode::IDLE;
    };

    confirm_no_ = confirm_panel_.add<Button>();
    confirm_no_->label = "Ні";
    confirm_no_->bg_color = ui_color::BG_LIGHT;
    confirm_no_->bounds = SDL_Rect{confirm_panel_.bounds.x + 240, confirm_panel_.bounds.y + 100, 180, 42};
    confirm_no_->on_click = [this]() { mode_ = mode_before_confirm_; };

    mode_ = confirm_mode;
}

void EditorApp::show_keyboard_for(TextField* field, std::function<void(const std::string&)> commit) {
    // Знімаємо фокус з усіх текстових полів поточного діалогу — лишень
    // одне активне поле має підсвічений курсор одночасно.
    for (auto& child : edit_dialog_.children()) {
        if (auto* tf = dynamic_cast<TextField*>(child.get())) {
            tf->focused = (tf == field);
        }
    }
    field->focused = true;

    keyboard_.bind(&field->value);
    keyboard_.on_change = [field, commit]() { commit(field->value); };
    keyboard_.on_done = [this]() { hide_keyboard(); };
    keyboard_.bounds = SDL_Rect{30, window_h_ - 280, window_w_ - 60, 250};
    keyboard_.visible = true;
    keyboard_visible_ = true;
}

void EditorApp::hide_keyboard() {
    keyboard_visible_ = false;
    keyboard_.visible = false;
    keyboard_.target = nullptr;
    for (auto& child : edit_dialog_.children()) {
        if (auto* tf = dynamic_cast<TextField*>(child.get())) {
            tf->focused = false;
        }
    }
}

void EditorApp::set_status(const std::string& text, bool is_error) {
    status_text_ = text;
    status_is_error_ = is_error;
    status_until_ms_ = SDL_GetTicks() + 4000;
}

void EditorApp::render_frame() {
    // ЛОКАЛЬНІ КАНАЛИ СТАНЦІЇ, ЯКІ РЕДАКТОР ЗНАЄ САМ.
    //
    // 211/212 наповнює станція, а вона під час роботи редактора зупинена:
    // екран один, DRM master ексклюзивний. Тому в превʼю вони показувались
    // як "--" — тобто саме ті два елементи, заради яких у діалозі є
    // виставлення часу, у самому превʼю не працювали.
    //
    // Годинник у редактора той самий, системний, і підставити його тут
    // чесно: превʼю показує рівно те, що покаже станція.
    {
        const std::time_t now = std::time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        telemetry_storage_.set_value(211,
            (float)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec));
        telemetry_storage_.set_value(212,
            (float)((lt.tm_year % 100) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday));
    }

    // РОЗМІР БЕРЕМО В РЕНДЕРЕРА, І ЩОКАДРУ.
    //
    // SDL_GetWindowSize одразу після створення вікна віддає ЗАПРОШЕНИЙ
    // розмір, а не справжній: вікно ще не змаповане. У повноекранному
    // режимі різниця виявляється фатальною — розкладку порахували під
    // 1280x800, а малювали в 1366x768, і 86 пікселів праворуч лишалися
    // чорною смугою, об яку обрізалася вся права колонка OSD.
    //
    // Подія зміни розміру теж не рятує: без віконного менеджера її може
    // не бути взагалі — нікому переводити вікно в повний екран через
    // EWMH. Тому питаємо того, хто напевно знає, скільки пікселів у цілі,
    // і перекладаємо все, щойно число змінилось. Це три порівняння на
    // кадр проти цілого класу невидимих розбіжностей.
    {
        int rw = 0, rh = 0;
        SDL_GetRendererOutputSize(renderer_, &rw, &rh);
        if (rw > 0 && rh > 0 && (rw != window_w_ || rh != window_h_)) {
            window_w_ = rw;
            window_h_ = rh;
            layout_chrome();
        }
    }

    last_hits_ = canvas_renderer_->render(renderer_, doc().elements(), selected_key_, *image_cache_, &telemetry_storage_);

    if ((mode_ == Mode::SELECTED || mode_ == Mode::DRAGGING) && !selected_key_.empty()) {
        for (auto& h : last_hits_) {
            if (h.key == selected_key_) {
                // КНОПКИ ЗАВЖДИ В МЕЖАХ ЕКРАНА.
                //
                // Стояли просто під елементом. Для нижнього ряду розкладки
                // (а там TIME, DIST, SPD, ALT — тобто половина всього) вони
                // опинялись за краєм екрана: елемент вибирається, кнопки
                // існують, натиснути їх нічим. Те саме збоку для елементів
                // біля правого краю.
                //
                // Тепер: не влазить знизу — стають НАД елементом; не влазить
                // справа — зсуваються вліво. Прив'язка до елемента лишається
                // (кнопки біля того, чим керують), але край екрана її
                // перебиває.
                const int bw = 90, bh = 32, gap = 6;
                int bx = h.rect.x;
                int by = h.rect.y + h.rect.h + gap;
                if (by + bh > window_h_) by = h.rect.y - gap - bh;
                if (by < 0) by = 0;
                if (by + bh > window_h_) by = window_h_ - bh;
                const int pair_w = bw * 2 + gap;
                if (bx + pair_w > window_w_) bx = window_w_ - pair_w;
                if (bx < 0) bx = 0;

                edit_button_.bounds = SDL_Rect{bx, by, bw, bh};
                delete_button_.bounds = SDL_Rect{bx + bw + gap, by, bw, bh};
                break;
            }
        }
    }

    save_button_.draw(renderer_, ui_font_);
    add_button_.draw(renderer_, ui_font_);
    screen_button_.draw(renderer_, ui_font_);
    exit_button_.draw(renderer_, ui_font_);

    if (mode_ == Mode::SELECTED) {
        edit_button_.draw(renderer_, ui_font_);
        delete_button_.draw(renderer_, ui_font_);
    }

    if (mode_ == Mode::EDIT_OPEN) edit_dialog_.draw(renderer_, ui_font_);
    if (mode_ == Mode::ADD_MENU_OPEN) add_menu_panel_.draw(renderer_, ui_font_);
    if (mode_ == Mode::CONFIRM_DELETE || mode_ == Mode::CONFIRM_SAVE) confirm_panel_.draw(renderer_, ui_font_);
    if (keyboard_visible_) keyboard_.draw(renderer_, ui_font_);

    if (!status_text_.empty() && SDL_GetTicks() < status_until_ms_) {
        SDL_Color c = status_is_error_ ? ui_color::DANGER : ui_color::SUCCESS;
        draw_ui_text(renderer_, ui_font_, status_text_, 16, window_h_ - 28, c);
    }

    draw_own_cursor();

    // ЗНІМОК КАДРУ: VRX_EDITOR_SHOT=/шлях.png
    //
    // Потрібен рівно для одного — щоб не з'ясовувати вигляд екрана
    // переказом. "Картинка зсунута вниз і обрізана" має з десяток різних
    // причин, і розрізняє їх тільки сам кадр.
    if (const char* shot = std::getenv("VRX_EDITOR_SHOT")) {
        static int frames = 0;
        if (++frames == 30) {
            int rw = 0, rh = 0;
            SDL_GetRendererOutputSize(renderer_, &rw, &rh);
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, rw, rh, 32,
                                                            SDL_PIXELFORMAT_ARGB8888);
            if (s && SDL_RenderReadPixels(renderer_, nullptr, s->format->format,
                                          s->pixels, s->pitch) == 0) {
                IMG_SavePNG(s, shot);
                std::fprintf(stderr, "[знімок] %s (%dx%d)\n", shot, rw, rh);
            }
            if (s) SDL_FreeSurface(s);
        }
    }

    SDL_RenderPresent(renderer_);
}

// ВЛАСНИЙ КУРСОР, а не системний.
//
// На станції без робочого стола системного курсора немає: віконного
// менеджера, який ставить тему й курсор кореневого вікна, там ніхто не
// запускає, а апаратний курсорний плейн на цьому VOP2 X не показує.
// Заміряно наживо: SDL віддає координати миші правильно, кліки доходять
// і елементи реагують — але тицяти доводиться наосліп, бо на екрані не
// видно нічого.
//
// Тому малюємо самі. Це п'ятнадцять рядків, вони не залежать ні від X, ні
// від теми, ні від драйвера — і переживуть перехід на вивід без X, якщо
// до нього дійде.
//
// Стрілка біла з чорною облямівкою: на світлому тлі полотна її видно так
// само, як на темному тлі панелей.
void EditorApp::draw_own_cursor() {
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);

    // Проста стрілка: рядок за рядком, ширина росте — виходить трикутник
    // із хвостиком. Малюємо двічі, зі зсувом, щоб дати темний контур.
    const int kLen = 14;
    for (int pass = 0; pass < 2; ++pass) {
        const int off = (pass == 0) ? 1 : 0;
        if (pass == 0) SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 220);
        else           SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        for (int i = 0; i < kLen; ++i) {
            const int w = (i < 10) ? (i / 2 + 1) : (kLen - i + 2);
            SDL_Rect r{mx + off, my + i + off, w, 1};
            SDL_RenderFillRect(renderer_, &r);
        }
    }
}

} // namespace osdedit
