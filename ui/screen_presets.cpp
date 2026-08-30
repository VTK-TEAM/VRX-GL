#include "screen_presets.hpp"

#include "../osd/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>

namespace vrx::ui {
namespace {

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- малювання картинок кодом (як у ScreenUi: жодних зовнішніх файлів) ---

void put(std::vector<uint8_t>& px, int w, int x, int y,
         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0) return;
    const size_t o = (size_t)(y * w + x) * 4;
    if (o + 3 >= px.size()) return;
    px[o + 0] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
}

// Цифри 1..3 у матриці 3x5.
const char* kDigits[3][5] = {
    {".X.", "XX.", ".X.", ".X.", "XXX"},   // 1
    {"XXX", "..X", "XXX", "X..", "XXX"},   // 2
    {"XXX", "..X", "XXX", "..X", "XXX"},   // 3
};

// Кнопка-цифра: темний (чи бурштиновий, коли активна) квадрат із рамкою й
// цифрою по центру.
render::OverlayImage make_num_button(int digit /*1..3*/, bool active, int size) {
    render::OverlayImage img;
    img.id = std::string("presets:btn") + std::to_string(digit) + (active ? "a" : "");
    img.width = img.height = size;
    img.rgba.assign((size_t)size * size * 4, 0);

    const int r = size / 6;
    const uint8_t fr = active ? 200 : 25, fg = active ? 120 : 28, fb = active ? 20 : 36;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int cx = (x < r) ? r : (x >= size - r ? size - r - 1 : x);
            const int cy = (y < r) ? r : (y >= size - r ? size - r - 1 : y);
            if (std::hypot(float(x - cx), float(y - cy)) > r) continue;
            // БЕЗ РАМКИ. Світлий обідець у два пікселі на заокругленому
            // куті лягав сходинками — згладжування тут нізвідки взятись,
            // бо картинка малюється кодом попіксельно. Суцільна плитка
            // читається чистіше, а форму тримають самі заокруглення.
            put(img.rgba, size, x, y, fr, fg, fb, active ? 255 : 200);
        }
    }

    // Цифра по центру: масштаб 3x5 -> ~половина кнопки.
    const int gs = std::max(2, size / 7);               // піксель цифри
    const int gw = 3 * gs, gh = 5 * gs;
    const int ox = (size - gw) / 2, oy = (size - gh) / 2;
    const uint8_t dr = active ? 30 : 235, dg = active ? 30 : 235, db = active ? 30 : 240;
    for (int gy = 0; gy < 5; ++gy)
        for (int gx = 0; gx < 3; ++gx)
            if (kDigits[digit - 1][gy][gx] == 'X')
                for (int sy = 0; sy < gs; ++sy)
                    for (int sx = 0; sx < gs; ++sx)
                        put(img.rgba, size, ox + gx * gs + sx, oy + gy * gs + sy, dr, dg, db, 255);
    return img;
}

// Тло кнопки: та сама заокруглена плитка, що й у цифрових.
void draw_button_bg(render::OverlayImage& img, bool active, int size) {
    const int r = size / 6;
    const uint8_t fr = active ? 200 : 25, fg = active ? 120 : 28, fb = active ? 20 : 36;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int cx = (x < r) ? r : (x >= size - r ? size - r - 1 : x);
            const int cy = (y < r) ? r : (y >= size - r ? size - r - 1 : y);
            if (std::hypot(float(x - cx), float(y - cy)) > r) continue;
            // БЕЗ РАМКИ. Світлий обідець у два пікселі на заокругленому
            // куті лягав сходинками — згладжування тут нізвідки взятись,
            // бо картинка малюється кодом попіксельно. Суцільна плитка
            // читається чистіше, а форму тримають самі заокруглення.
            put(img.rgba, size, x, y, fr, fg, fb, active ? 255 : 200);
        }
    }
}

void fill_box(render::OverlayImage& img, int size, int x0, int y0, int w, int h,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            put(img.rgba, size, x, y, r, g, b, a);
}

// ПЕРЕМИКАЧ OSD: коло по центру й два промені вбоки — той самий знак,
// що й авіагоризонт у кабіні. Читається як "прилади", і саме приладами
// OSD і є; три рядки тексту, що стояли тут раніше, читались радше як
// "субтитри".
//
// Яскравий — телеметрію показуємо, тьмяний — ні.
// Кнопка плеєра. У режимі ефіру на ній трикутник "грати", у режимі
// плеєра — квадрат "стоп", тобто повернутись до ефіру. Знак показує, що
// СТАНЕТЬСЯ від натискання, а не що зараз, — так само як у кнопці OSD.
render::OverlayImage make_player_button(bool playing) {
    const int size = 48;
    render::OverlayImage img;
    img.id = std::string("presets:play") + (playing ? "1" : "0");
    img.width = img.height = size;
    img.rgba.assign((size_t)size * size * 4, 0);
    draw_button_bg(img, false, size);

    const uint8_t r = playing ? 255 : 120, g = playing ? 120 : 200, b = playing ? 90 : 130;
    const uint8_t a = playing ? 255 : 210;
    const int m = size / 3;                       // відступ від краю

    if (playing) {
        for (int y = m; y < size - m; ++y)
            for (int x = m; x < size - m; ++x)
                put(img.rgba, size, x, y, r, g, b, a);
    } else {
        // Трикутник вершиною вправо: висота зменшується до вершини.
        const float h = float(size - 2 * m);
        for (int x = m; x < size - m; ++x) {
            const float k = float(x - m) / h;      // 0 біля основи, 1 біля вершини
            const int half = (int)(h * 0.5f * (1.0f - k));
            for (int y = size / 2 - half; y <= size / 2 + half; ++y)
                put(img.rgba, size, x, y, r, g, b, a);
        }
    }
    return img;
}

render::OverlayImage make_osd_button(bool on) {
    const int size = 48;
    render::OverlayImage img;
    img.id = std::string("presets:osd") + (on ? "1" : "0");
    img.width = img.height = size;
    img.rgba.assign((size_t)size * size * 4, 0);
    draw_button_bg(img, false, size);

    const uint8_t r = on ? 255 : 110, g = on ? 190 : 110, b = on ? 60 : 120;
    const uint8_t a = on ? 255 : 200;

    const float cx = (size - 1) * 0.5f, cy = (size - 1) * 0.5f;
    const float rad = size / 7.0f;
    const int th = std::max(2, size / 14);          // товщина променя й обода

    // Коло — обідцем, а не залите: залите на 48 пікселях перетворюється
    // на пляму, у якій форми не видно.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float d = std::hypot(float(x) - cx, float(y) - cy);
            if (d <= rad && d >= rad - th) put(img.rgba, size, x, y, r, g, b, a);
        }
    }

    // Два промені вбоки, на висоті центра: із зазором від кола і з
    // ВІДСТУПОМ ВІД КРАЮ плитки. Раніше довжина була задана числом, і
    // лівий промінь упирався рівно в край кнопки — знак виглядав
    // обрізаним, ніби не помістився.
    //
    // Тепер навпаки: задані відступ і зазор, а довжина — те, що між ними
    // лишилось. Так промені не дістануть краю за будь-якого розміру
    // кнопки.
    const int margin = std::max(3, size / 8);        // від краю плитки
    const int gap    = std::max(2, size / 16);       // від кола
    const int y0 = (int)(cy - th * 0.5f + 0.5f);
    const int len = (int)(cx - rad) - gap - margin;
    if (len > 0) {
        // Правий рахуємо дзеркально, а не своєю арифметикою: інакше
        // округлення дало б промені різної довжини.
        fill_box(img, size, margin, y0, len, th, r, g, b, a);
        fill_box(img, size, size - margin - len, y0, len, th, r, g, b, a);
    }
    return img;
}

// Суцільний бурштин для рамки виділеного вікна.
render::OverlayImage make_solid() {
    render::OverlayImage img;
    img.id = "presets:solid";
    img.width = img.height = 4;
    img.rgba.assign(4 * 4 * 4, 0);
    for (auto* p = img.rgba.data(); p < img.rgba.data() + img.rgba.size(); p += 4) {
        p[0] = 255; p[1] = 190; p[2] = 60; p[3] = 230;
    }
    return img;
}

const char* anchor_key(layout::Anchor a) { (void)a; return "anchor"; }

nlohmann::json to_json(const layout::Placement& p) {
    return nlohmann::json{{"x", p.x}, {"y", p.y}, {"w", p.w}, {"h", p.h},
                          {"z", p.z}, {anchor_key(p.anchor), (int)p.anchor}};
}
layout::Placement from_json(const nlohmann::json& j, layout::Placement fallback) {
    layout::Placement p = fallback;
    p.x = j.value("x", p.x); p.y = j.value("y", p.y);
    p.w = j.value("w", p.w); p.h = j.value("h", p.h);
    p.z = j.value("z", p.z);
    int a = j.value("anchor", (int)p.anchor);
    if (a >= 0 && a <= 8) p.anchor = (layout::Anchor)a;
    return layout::sanitize(p);
}

} // namespace

// ---------------------------------------------------------------------

struct ScreenPresets::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    VtTelemetryStorage* tlm = nullptr;
    std::vector<Window> windows;

    std::vector<render::OverlayImage> images;   // [d*2+active] цифри, далі solid і перемикачі
    int solid_idx = 0;
    int osd_idx = 0;       // +0 OSD вимкнено, +1 увімкнено
    int play_idx = 0;      // +0 ефір (знак "грати"), +1 плеєр (знак "стоп")
    int btn_px = 48;

    // Геометрія КОЖНОГО екрана: частки рахуються від свого.
    static constexpr int kRoles = 2;
    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};

    // --- стан, що ділиться зі збережним потоком ---
    std::mutex mtx;

    // [пресет][роль][вікно].
    //
    // ПРЕСЕТ — це повна конфігурація СТАНЦІЇ, а не одного екрана: у ньому
    // лежать обидві розкладки одразу. Тому перемикання 1/2/3 міняє
    // картинку на обох екранах разом, а перемикач ролі лише каже, чию
    // саме розкладку зараз тягає миша.
    std::vector<std::vector<std::vector<layout::Placement>>> presets;
    // [пресет][роль] — чи показувати OSD. Типово: на основному так, на
    // додатковому ні (ТЗ).
    std::vector<std::array<bool, 2>> osd_on;
    // АКТИВНИЙ ПРЕСЕТ — СВІЙ НА КОЖНОМУ ЕКРАНІ.
    //
    // Спершу я зробив пресет спільним, міркуючи, що це "конфігурація
    // станції". Хибно: екрани незалежні за ТЗ, і оператор має право
    // тримати на основному одну розкладку, а на додатковому іншу. Тому
    // кнопка 1/2/3 діє лише на той екран, на якому її натиснули.
    //
    // Файл пресету при цьому лишається спільним: у ньому обидві секції,
    // просто екрани можуть читати РІЗНІ файли одночасно.
    // Режим кожної ролі й СВІЙ активний пресет на кожен режим.
    int mode[kRoles] = {ScreenPresets::kLive, ScreenPresets::kLive};
    int active_by[ScreenPresets::kModes][kRoles] = {{0, 0}, {0, 0}};

    int& active_of(int role) { return active_by[mode[role]][role]; }
    int  active_of(int role) const { return active_by[mode[role]][role]; }

    // Вікно належить набору, який зараз показує ця роль.
    bool in_mode(int role, size_t i) const {
        if (windows[i].mode != mode[role]) return false;
        return windows[i].role < 0 || windows[i].role == role;
    }

    // Кого сповістити про перемикання режиму. Пресети самі про плеєр
    // нічого не знають і знати не мусять: їхня справа — вікна й розкладка,
    // а відкрити сеанс і завести декодери має той, хто ними володіє.
    std::function<void(int role, int mode)> on_mode;
    std::function<bool(int role, float cx, float cy)> blocked;
    uint32_t dirty_mask = 0;
    int64_t last_edit = 0;

    // --- стан вводу (лише потік показу) ---
    uint64_t seen_clicks = 0;
    uint64_t seen_rclicks = 0;
    uint64_t seen_mclicks = 0;

    render::Scene* scene = nullptr;
    int64_t seen_wheel = 0;
    bool was_left = false;
    int drag_win = -1;
    float prev_cx = 0.f, prev_cy = 0.f;
    int last_switch_pos = -1;
    // ОБРАНЕ ВІКНО — СВОЄ НА КОЖНОМУ ЕКРАНІ, -1 = нічого.
    //
    // Спільне поле давало дві біди одразу. Видиму: виділив вікно на
    // одному екрані — рамка з'являлась і на другому, бо малювання не
    // питало, де зараз курсор. І невидиму, гіршу: колесо крутило вікно
    // ТОГО екрана, над яким миша, хоч обирали зовсім інше.
    //
    // Вибір належить розкладці, а розкладка — екрану.
    int selected[kRoles] = {-1, -1};
    bool apply_pending = true;   // застосувати пресет до джерел (лише при зміні)

    std::thread saver;
    std::atomic<bool> running{false};

    explicit Impl(Config c) : cfg(std::move(c)) {}

    std::string file_of(int preset) const {
        return cfg.file_prefix + std::to_string(preset + 1) + ".json";
    }

    void build_images() {
        for (int d = 1; d <= cfg.preset_count; ++d) {
            images.push_back(make_num_button(d, false, btn_px));
            images.push_back(make_num_button(d, true, btn_px));
        }
        solid_idx = (int)images.size();
        images.push_back(make_solid());
        osd_idx = (int)images.size();
        images.push_back(make_osd_button(false));
        images.push_back(make_osd_button(true));
        play_idx = (int)images.size();
        images.push_back(make_player_button(false));
        images.push_back(make_player_button(true));
    }

    // Розкладка однієї ролі з уже розібраного json.
    void load_role(const nlohmann::json& node, int p, int role) {
        for (size_t i = 0; i < windows.size(); ++i) {
            layout::Placement pl = windows[i].fallback;
            if (node.contains("windows") && node["windows"].contains(windows[i].name)) {
                pl = from_json(node["windows"][windows[i].name], windows[i].fallback);
            }
            presets[p][role][i] = pl;
        }
    }

    void load_all() {
        presets.assign(cfg.preset_count,
            std::vector<std::vector<layout::Placement>>(
                2, std::vector<layout::Placement>(windows.size())));
        osd_on.assign(cfg.preset_count, {true, false});

        for (int p = 0; p < cfg.preset_count; ++p) {
            nlohmann::json root;
            bool ok = false;
            std::ifstream f(file_of(p));
            if (f.is_open()) { try { f >> root; ok = true; } catch (...) { ok = false; } }

            // СУМІСНІСТЬ ЗІ СТАРИМ ФОРМАТОМ, і вона тут не з ввічливості.
            // Файли з одним екраном мають "windows" на верхньому рівні —
            // це вже налаштована кимось розкладка, і вона за змістом саме
            // ОСНОВНА. Читаємо її туди, а додатковий стартує з типової.
            // Так нічого не губиться й переносити руками нічого не треба.
            const bool legacy = ok && root.contains("windows");

            for (int r = 0; r < 2; ++r) {
                const char* key = (r == render::Scene::kPrimary) ? "primary" : "secondary";
                if (ok && root.contains(key)) {
                    load_role(root[key], p, r);
                } else if (legacy && r == render::Scene::kPrimary) {
                    load_role(root, p, r);
                } else {
                    load_role(nlohmann::json::object(), p, r);
                }
            }
            if (ok && root.contains("osd")) {
                osd_on[p][render::Scene::kPrimary]   = root["osd"].value("primary", true);
                osd_on[p][render::Scene::kSecondary] = root["osd"].value("secondary", false);
            }

            std::fprintf(stderr, "[екрани] пресет %d %s%s\n", p + 1,
                         ok ? "завантажено" : "типовий (файлу немає)",
                         legacy ? " (старий формат -> основний екран)" : "");
        }
    }

    void save_preset(int p, const std::vector<std::vector<layout::Placement>>& roles,
                     const std::array<bool, 2>& osd) {
        nlohmann::json root;
        for (int r = 0; r < 2; ++r) {
            const char* key = (r == render::Scene::kPrimary) ? "primary" : "secondary";
            for (size_t i = 0; i < windows.size(); ++i)
                root[key]["windows"][windows[i].name] = to_json(roles[r][i]);
        }
        root["osd"]["primary"]   = osd[render::Scene::kPrimary];
        root["osd"]["secondary"] = osd[render::Scene::kSecondary];
        std::ofstream f(file_of(p));
        if (!f.is_open()) {
            std::fprintf(stderr, "[екрани] не зберігся %s\n", file_of(p).c_str());
            return;
        }
        f << root.dump(2) << "\n";
        std::fprintf(stderr, "[екрани] пресет %d збережено\n", p + 1);
    }

    // Екранний прямокутник вікна i (частки, 0=ВЕРХ), з урахуванням аспекту
    // й якоря.
    //
    // ВІСЬ Y ВІДЕО ПЕРЕВЕРНУТА відносно оверлея: відео малюється з
    // y0 = 1-2*p.y (gl_quad.hpp), а оверлей/курсор — з 2*q.y-1. Тобто те
    // саме число y дає ДЗЕРКАЛЬНІ місця. Повертаємо тут одразу в екранних
    // (оверлейних) координатах, щоб рамка, hit-тест і курсор жили в одній
    // системі; назад у placement переводить move() з інверсією.
    struct Rect { float x, y, w, h; };
    Rect rect_of(int role, size_t i) const {
        const int W = rw[role].load(), H = rh[role].load();
        const float sa = (W > 0 && H > 0) ? float(W) / float(H) : 1.777f;
        float va = windows[i].source ? windows[i].source->frame_aspect() : 0.f;
        if (va <= 0.f) va = sa;                       // ще не знаємо кадр — беремо як екран
        layout::Placement r = layout::fit_source(presets[active_of(role)][role][i], va, sa);
        return {r.x, 1.0f - r.y - r.h, r.w, r.h};     // Y -> екранний (0=верх)
    }

    // Зсунути вікно на (dsx, dsy) в ЕКРАННИХ частках. По X прямо, по Y
    // інверсія (екранний низ = менший placement.y).
    void move(int role, size_t i, float dsx, float dsy) {
        presets[active_of(role)][role][i].x += dsx;
        presets[active_of(role)][role][i].y -= dsy;
    }

    // Верхнє (за z) вікно під курсором, або -1.
    int window_at(int role, float cx, float cy) const {
        int best = -1, bestz = -1000000;
        for (size_t i = 0; i < windows.size(); ++i) {
            if (!in_mode(role, i)) continue;      // чуже вікно, не цього набору
            Rect r = rect_of(role, i);
            if (cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h) {
                const int z = presets[active_of(role)][role][i].z;
                if (z >= bestz) { bestz = z; best = (int)i; }
            }
        }
        return best;
    }

    // Не дати вікну ПОВНІСТЮ піти за екран.
    //
    // Раніше прямокутник тримався в межах екрана цілком, і засунути його
    // частину за край було неможливо. Це заважає рівно там, де воно
    // потрібне: збільшену картинку доводиться зсувати, щоб дивитись на її
    // край, а вікно, яке заступає потрібне, — прибирати вбік.
    //
    // Тепер за край можна засунути до kOffscreen від розміру вікна, і на
    // екрані лишається принаймні решта. Межа саме на розмір ВІКНА, а не
    // екрана: інакше велике вікно ховалося б повністю, а маленьке не
    // зсувалося б майже нікуди.
    static constexpr float kOffscreen = 0.70f;   // 70% дозволено за краєм
    void clamp_on_screen(int role, size_t i) {
        Rect r = rect_of(role, i);
        // Скільки має лишитись видимим по кожній осі.
        const float keep_x = r.w * (1.f - kOffscreen);
        const float keep_y = r.h * (1.f - kOffscreen);

        float dsx = 0.f, dsy = 0.f;
        if (r.x + r.w < keep_x)      dsx = keep_x - (r.x + r.w);  // пішло вліво
        else if (r.x > 1.f - keep_x) dsx = (1.f - keep_x) - r.x;  // пішло вправо
        if (r.y + r.h < keep_y)      dsy = keep_y - (r.y + r.h);  // пішло вгору
        else if (r.y > 1.f - keep_y) dsy = (1.f - keep_y) - r.y;  // пішло вниз
        move(role, i, dsx, dsy);
    }

    // Розгорнути вікно рівно по краях екрана: коробка на всю площу,
    // прикріплена центром. Картинка вписується в неї за пропорцією, тож
    // упирається в краї тією віссю, якою дозволяє кадр.
    //
    // Це опорна точка для всього іншого: після довгого совання й зуму
    // повернутись до чогось передбачуваного інакше нічим.
    void fill_screen(int role, size_t i) {
        layout::Placement& pl = presets[active_of(role)][role][i];
        pl.x = 0.5f; pl.y = 0.5f;
        pl.w = 1.0f; pl.h = 1.0f;
        pl.anchor = layout::Anchor::Center;
        pl = layout::sanitize(pl);
    }

    void mark_dirty(int role) {
        dirty_mask |= (1u << active_of(role));
        last_edit = now_ms();
    }

    // Провалити вікно wi на ОДИН рівень нижче в стеку: свап із тим, що
    // прямо під ним, і z переприсвоюються КОМПАКТНО (0..n-1). Так вони
    // завжди сусідні, а не розповзаються від повторних кліків.
    void lower_window(int role, size_t wi) {
        auto& pv = presets[active_of(role)][role];
        std::vector<size_t> order(pv.size());
        for (size_t i = 0; i < pv.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return pv[a].z < pv[b].z; });
        size_t pos = 0;
        for (size_t k = 0; k < order.size(); ++k) if (order[k] == wi) { pos = k; break; }
        if (pos > 0) std::swap(order[pos], order[pos - 1]);   // на один вниз
        for (size_t k = 0; k < order.size(); ++k) pv[order[k]].z = (int)k;  // 0 = найнижче
    }

    // Кнопки 1..3 зліва вгорі; повертає прямокутник кнопки b (частки).
    // Кнопки: [0..preset_count) — пресети, далі ПРОМІЖОК і два
    // перемикачі: екран, чию розкладку тягаємо, і OSD на ньому.
    // Проміжок навмисний: це кнопки іншого призначення, і ставити їх
    // упритул до цифр означало б запрошувати на випадковий клік.
    // Кнопки одним рядом, без проміжків: [1][2][3][OSD]. Пресети й
    // перемикач телеметрії — речі одного роду (що показувати на ЦЬОМУ
    // екрані), тож і стоять разом. Проміжок лишається далі, перед
    // кнопкою редактора, яка веде зовсім в інше місце — в окрему
    // програму (див. ScreenUi::Config::slot).
    int osd_button() const { return cfg.preset_count; }
    int play_button() const { return cfg.preset_count + 1; }
    int button_count() const { return cfg.preset_count + 2; }

    void button_rect(int role, int b, float* x, float* y, float* w, float* h) const {
        const int W = rw[role].load(), H = rh[role].load();
        const float aspect = (W > 0 && H > 0) ? float(H) / float(W) : 0.5625f;
        *h = cfg.button_size;
        *w = cfg.button_size * aspect;               // квадрат на екрані
        const float gap = *w * 0.25f;
        *x = b * (*w + gap);
        *y = 0.f;
    }

    void saver_loop() {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            int p_to_save = -1;
            std::vector<std::vector<layout::Placement>> snap;
            std::array<bool, 2> snap_osd{true, false};
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (dirty_mask && now_ms() - last_edit >= cfg.autosave_ms) {
                    for (int p = 0; p < cfg.preset_count; ++p)
                        if (dirty_mask & (1u << p)) { p_to_save = p; break; }
                    if (p_to_save >= 0) {
                        snap = presets[p_to_save];
                        snap_osd = osd_on[p_to_save];
                        dirty_mask &= ~(1u << p_to_save);
                    }
                }
            }
            if (p_to_save >= 0) save_preset(p_to_save, snap, snap_osd);   // I/O поза локом
        }
    }
};

// ---------------------------------------------------------------------

ScreenPresets::ScreenPresets(Config cfg) : impl_(new Impl(std::move(cfg))) {}
ScreenPresets::~ScreenPresets() { stop(); }

void ScreenPresets::attach(Pointer* pointer) { impl_->pointer = pointer; }
void ScreenPresets::add_window(Window w) { impl_->windows.push_back(std::move(w)); }
void ScreenPresets::set_telemetry(VtTelemetryStorage* tlm) { impl_->tlm = tlm; }

void ScreenPresets::attach_scene(render::Scene* s) { impl_->scene = s; }

void ScreenPresets::set_blocked_area(std::function<bool(int, float, float)> cb) {
    impl_->blocked = std::move(cb);
}

void ScreenPresets::set_on_mode(std::function<void(int, int)> cb) {
    impl_->on_mode = std::move(cb);
}

void ScreenPresets::set_mode(int role, int m) {
    if (role < 0 || role >= Impl::kRoles || m < 0 || m >= kModes) return;
    if (impl_->mode[role] == m) return;
    impl_->mode[role] = m;
    impl_->selected[role] = -1;          // вибір належав іншому набору вікон
    impl_->apply_pending = true;
}

int ScreenPresets::mode(int role) const {
    return (role >= 0 && role < Impl::kRoles) ? impl_->mode[role] : kLive;
}

bool ScreenPresets::start() {
    Impl& d = *impl_;
    d.build_images();
    d.load_all();
    d.running.store(true);
    d.saver = std::thread([&d] { d.saver_loop(); });
    std::fprintf(stderr, "[екрани] %d пресетів, %zu вікон, перемикач канал %u\n",
                 d.cfg.preset_count, d.windows.size(), d.cfg.switch_channel);
    return true;
}

void ScreenPresets::stop() {
    Impl& d = *impl_;
    if (d.running.exchange(false)) {
        if (d.saver.joinable()) d.saver.join();
    }
}

void ScreenPresets::set_frame_size(int role, int width, int height) {
    if (role < 0 || role >= Impl::kRoles) return;
    impl_->rw[role].store(width);
    impl_->rh[role].store(height);
}

const std::vector<render::OverlayImage>& ScreenPresets::images() const {
    return impl_->images;
}

bool ScreenPresets::acquire(int role, render::DrawList& out) {
    Impl& d = *impl_;
    if (role < 0 || role >= Impl::kRoles) return false;
    const int W = d.rw[role].load(), H = d.rh[role].load();
    if (W <= 0 || H <= 0 || d.windows.empty()) return false;

    std::lock_guard<std::mutex> lk(d.mtx);

    // --- 1) перемикач каналом 15 (ПО ЗМІНІ позиції) ---
    if (d.tlm) {
        float raw = 0.f;
        if (d.tlm->get_value(d.cfg.switch_channel, &raw)) {
            const int v = (int)std::max(0.f, raw);
            const int pos = (v < d.cfg.thr_low) ? 0 : (v <= d.cfg.thr_high ? 1 : 2);
            if (pos != d.last_switch_pos) {
                d.last_switch_pos = pos;
                // ПЕРЕМИКАЧ НА ПУЛЬТІ ВЕДЕ ОСНОВНИЙ ЕКРАН. Це рука
                // пілота, і стосується вона того, на що він дивиться в
                // польоті. Додатковий налаштовує оператор мишею.
                const int r = render::Scene::kPrimary;
                if (pos < d.cfg.preset_count && pos != d.active_of(r)) {
                    d.active_of(r) = pos; d.selected[r] = -1; d.apply_pending = true;
                }
            }
        }
    }

    // ВВІД ОБРОБЛЯЄТЬСЯ РІВНО ОДИН РАЗ ЗА КАДР.
    //
    // Рендерер кличе цей оверлей по разу на кожен екран, а всередині ми
    // і малюємо, і читаємо мишу. Якби ввід читався в обох викликах,
    // кожен клік порахувався б двічі, а перетяг рухав би вікно вдвічі
    // швидше. Тому вся робота з мишею — лише на тому екрані, де курсор
    // ЗАРАЗ фізично є; він завжди рівно один.
    //
    // Заразом це прибирає потребу в перемикачі ролі: редагується той
    // екран, на якому курсор.
    float cx = 0.f, cy = 0.f;
    bool have_ptr = false;
    if (d.pointer) {
        const PointerState p = d.pointer->state();
        if (p.present && p.screen == role) {
            have_ptr = true;
            cx = float(p.x) / float(W);
            cy = float(p.y) / float(H);

            // --- 2) кнопки 1..3: клік перемикає пресет ---
            bool on_button = false;
            const bool clicked = p.clicks > d.seen_clicks;
            for (int b = 0; b < d.button_count(); ++b) {
                float bx, by, bw, bh;
                d.button_rect(role, b, &bx, &by, &bw, &bh);
                if (!(cx >= bx && cx <= bx + bw && cy >= by && cy <= by + bh)) continue;
                on_button = true;
                if (!clicked) continue;

                if (b < d.cfg.preset_count) {
                    // Пресет — конфігурація ВСІЄЇ станції, тож міняє
                    // картинку на обох екранах разом.
                    if (b != d.active_of(role)) {
                        d.active_of(role) = b; d.selected[role] = -1; d.apply_pending = true;
                        std::fprintf(stderr, "[екрани] %s екран -> пресет %d\n",
                                     role == render::Scene::kPrimary ? "основний" : "додатковий",
                                     b + 1);
                    }
                } else if (b == d.play_button()) {
                    // ПЛЕЄР НА ЦЬОМУ ЕКРАНІ. Другий екран лишається на
                    // ефірі — або має свій плеєр, це дозволено.
                    const int m = d.mode[role] == ScreenPresets::kPlayer
                                      ? ScreenPresets::kLive : ScreenPresets::kPlayer;
                    d.mode[role] = m;
                    d.selected[role] = -1;      // вибір належав іншому набору
                    d.apply_pending = true;
                    if (d.on_mode) d.on_mode(role, m);
                    std::fprintf(stderr, "[екрани] %s екран -> %s\n",
                                 role == render::Scene::kPrimary ? "основний" : "додатковий",
                                 m == ScreenPresets::kPlayer ? "плеєр" : "ефір");
                } else {
                    // OSD САМЕ ЦЬОГО ЕКРАНА — того, на якому кнопку
                    // натиснули. Своя кнопка на кожному, перемикати нічого
                    // не треба.
                    bool& on = d.osd_on[d.active_of(role)][role];
                    on = !on;
                    d.mark_dirty(role);
                    d.apply_pending = true;
                    std::fprintf(stderr, "[екрани] OSD на %s екрані: %s\n",
                                 role == render::Scene::kPrimary ? "основному" : "додатковому",
                                 on ? "увімкнено" : "вимкнено");
                }
            }
            if (clicked) d.seen_clicks = p.clicks;

            // Чужий шар (таймлайн плеєра) забирає натискання собі.
            if (d.blocked && d.blocked(role, cx, cy)) on_button = true;

            // --- 3) ліва кнопка: ОБРАТИ вікно (і почати перетяг) ---
            if (p.left && !d.was_left) {                 // натиснули
                if (on_button) { d.drag_win = -1; }
                else {
                    const int wi = d.window_at(role, cx, cy);
                    if (wi >= 0) d.selected[role] = wi;  // обрали
                    d.drag_win = wi;                     // і тягнемо його
                }
                d.prev_cx = cx; d.prev_cy = cy;
            }
            if (p.left && d.drag_win >= 0) {              // тягнемо обране
                d.move(role, (size_t)d.drag_win, cx - d.prev_cx, cy - d.prev_cy);
                d.clamp_on_screen(role, (size_t)d.drag_win);
                d.mark_dirty(role); d.apply_pending = true;
            }
            if (!p.left) d.drag_win = -1;
            d.prev_cx = cx; d.prev_cy = cy;
            d.was_left = p.left;

            // --- 4) колесо: масштаб ОБРАНОГО вікна (не під курсором) ---
            const int64_t dwheel = p.wheel - d.seen_wheel;
            d.seen_wheel = p.wheel;
            if (dwheel != 0 && d.selected[role] >= 0) {
                const size_t wi = (size_t)d.selected[role];
                Impl::Rect r0 = d.rect_of(role, wi);
                const float c0x = r0.x + r0.w * 0.5f, c0y = r0.y + r0.h * 0.5f;
                layout::Placement& pl = d.presets[d.active_of(role)][role][wi];
                float f = std::pow(1.0f + d.cfg.wheel_step, (float)dwheel);
                pl.w *= f; pl.h *= f;
                // Стеля — kMaxSize площ екрана (див. layout.hpp). Міряємо
                // по ВПИСАНІЙ картинці, а не по коробці: коробка може бути
                // якої завгодно пропорції, а видно саме картинку, і
                // обмежувати треба те, що видно.
                Impl::Rect r = d.rect_of(role, wi);
                float s = 1.f;
                if (r.w > layout::kMaxSize) s = std::min(s, layout::kMaxSize / r.w);
                if (r.h > layout::kMaxSize) s = std::min(s, layout::kMaxSize / r.h);
                pl.w *= s; pl.h *= s;
                pl = layout::sanitize(pl);
                Impl::Rect r1 = d.rect_of(role, wi);           // тримати центр
                d.move(role, wi, c0x - (r1.x + r1.w * 0.5f), c0y - (r1.y + r1.h * 0.5f));
                d.clamp_on_screen(role, wi);
                d.mark_dirty(role); d.apply_pending = true;
            }

            // --- 5) права кнопка: на ОБРАНОМУ — z−1; на іншому — зняти вибір ---
            if (p.rclicks > d.seen_rclicks) {
                d.seen_rclicks = p.rclicks;
                const int wi = on_button ? -1 : d.window_at(role, cx, cy);
                if (wi >= 0 && wi == d.selected[role]) {
                    d.lower_window(role, (size_t)wi);          // на один рівень вниз, z компактні
                    d.mark_dirty(role); d.apply_pending = true;
                    std::fprintf(stderr, "[екрани] '%s' -> z %d\n",
                                 d.windows[wi].name.c_str(),
                                 d.presets[d.active_of(role)][role][wi].z);
                } else {
                    d.selected[role] = -1;               // клац на іншому/порожньому — зняти
                }
            }

            // --- 6) натиснуте колесо: розгорнути по краях екрана ---
            //
            // Береться вікно ПІД КУРСОРОМ, а не обране: після того як
            // вікно засунули за край, влучити по ньому мишею буває нічим —
            // на екрані лишилась третина, і та може бути під іншим. Клац
            // колесом по видимій частині повертає його на всю площу.
            if (p.mclicks > d.seen_mclicks) {
                d.seen_mclicks = p.mclicks;
                const int wi = on_button ? -1 : d.window_at(role, cx, cy);
                if (wi >= 0) {
                    d.fill_screen(role, (size_t)wi);
                    d.selected[role] = wi;
                    d.mark_dirty(role); d.apply_pending = true;
                    std::fprintf(stderr, "[екрани] '%s' -> на весь екран\n",
                                 d.windows[wi].name.c_str());
                }
            }
        }
    }

    // --- 6) застосувати пресет до джерел ЛИШЕ ПРИ ЗМІНІ ---
    // Робиться лише при ЗМІНІ, а не щокадру: запис у сцену бере її
    // м'ютекс, і смикати його з потоку показу шістдесят разів на секунду
    // означало б платити локом за роботу, якої немає.
    //
    // Пишемо ОБИДВІ ролі, а не лише ту, що редагується: пресет — це повна
    // конфігурація станції, і перемикання 1/2/3 має міняти картинку на
    // обох екранах одразу.
    if (d.apply_pending && d.scene) {
        for (int r = 0; r < 2; ++r) {
            for (size_t i = 0; i < d.windows.size(); ++i) {
                if (!d.windows[i].source) continue;
                // Вікна чужого набору не ховаються десь окремо — вони
                // просто вимкнені в сцені, і рендерер їх пропускає.
                layout::Placement pl = d.presets[d.active_of(r)][r][i];
                if (!d.in_mode(r, i)) pl.enabled = false;
                d.scene->set(r, d.windows[i].source.get(), pl);
            }
            d.scene->set_osd(r, d.osd_on[d.active_of(r)][r]);
        }
        d.apply_pending = false;
    }

    // --- 6) малювання: рамка активного вікна + три кнопки ---
    out.quads.clear();
    auto push = [&out](int image, float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    };

    // рамка ОБРАНОГО вікна — бурштинова
    (void)have_ptr;
    // Рамка — лише для вікна, обраного НА ЦЬОМУ екрані.
    if (d.selected[role] >= 0 && (size_t)d.selected[role] < d.windows.size()) {
        Impl::Rect r = d.rect_of(role, (size_t)d.selected[role]);
        const float t = 0.004f;                          // товщина рамки
        const float ta = t * float(H) / float(W);        // однакова в пікселях
        push(d.solid_idx, r.x, r.y, r.w, ta);            // верх
        push(d.solid_idx, r.x, r.y + r.h - ta, r.w, ta); // низ
        push(d.solid_idx, r.x, r.y, t, r.h);             // ліво
        push(d.solid_idx, r.x + r.w - t, r.y, t, r.h);   // право
    }

    for (int b = 0; b < d.cfg.preset_count; ++b) {
        float bx, by, bw, bh;
        d.button_rect(role, b, &bx, &by, &bw, &bh);
        const int img = b * 2 + (b == d.active_of(role) ? 1 : 0);
        push(img, bx, by, bw, bh);
    }

    // Кнопка OSD — СВОЯ на кожному екрані, і показує СТАН, а не дію:
    // рядки яскраві, коли телеметрію тут показуємо.
    {
        float bx, by, bw, bh;
        d.button_rect(role, d.osd_button(), &bx, &by, &bw, &bh);
        push(d.osd_idx + (d.osd_on[d.active_of(role)][role] ? 1 : 0), bx, by, bw, bh);

        d.button_rect(role, d.play_button(), &bx, &by, &bw, &bh);
        push(d.play_idx + (d.mode[role] == ScreenPresets::kPlayer ? 1 : 0), bx, by, bw, bh);
    }
    return true;
}

} // namespace vrx::ui
