#include "screen_presets.hpp"

#include "../osd/json.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
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
    const uint8_t br = active ? 255 : 235, bg = active ? 240 : 235, bb = active ? 190 : 240;
    const uint8_t fr = active ? 200 : 25, fg = active ? 120 : 28, fb = active ? 20 : 36;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int cx = (x < r) ? r : (x >= size - r ? size - r - 1 : x);
            const int cy = (y < r) ? r : (y >= size - r ? size - r - 1 : y);
            if (std::hypot(float(x - cx), float(y - cy)) > r) continue;
            const bool edge = x < 2 || y < 2 || x >= size - 2 || y >= size - 2;
            if (edge) put(img.rgba, size, x, y, br, bg, bb, active ? 255 : 235);
            else      put(img.rgba, size, x, y, fr, fg, fb, active ? 255 : 200);
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

    std::vector<render::OverlayImage> images;   // [d*2+active] цифри, останній — solid
    int solid_idx = 0;
    int btn_px = 48;

    std::atomic<int> fw{0}, fh{0};

    // --- стан, що ділиться зі збережним потоком ---
    std::mutex mtx;
    std::vector<std::vector<layout::Placement>> presets;  // [preset][window]
    int active = 0;
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
    int selected = -1;           // обране вікно (реагує на колесо), -1 = нічого
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
    }

    void load_all() {
        presets.assign(cfg.preset_count, std::vector<layout::Placement>(windows.size()));
        for (int p = 0; p < cfg.preset_count; ++p) {
            nlohmann::json root;
            bool ok = false;
            std::ifstream f(file_of(p));
            if (f.is_open()) { try { f >> root; ok = true; } catch (...) { ok = false; } }
            for (size_t i = 0; i < windows.size(); ++i) {
                layout::Placement pl = windows[i].fallback;
                if (ok && root.contains("windows") &&
                    root["windows"].contains(windows[i].name)) {
                    pl = from_json(root["windows"][windows[i].name], windows[i].fallback);
                }
                presets[p][i] = pl;
            }
            std::fprintf(stderr, "[екрани] пресет %d %s\n", p + 1,
                         ok ? "завантажено" : "типовий (файлу немає)");
        }
    }

    void save_preset(int p, const std::vector<layout::Placement>& wins) {
        nlohmann::json root;
        for (size_t i = 0; i < windows.size(); ++i)
            root["windows"][windows[i].name] = to_json(wins[i]);
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
    Rect rect_of(size_t i) const {
        const int W = fw.load(), H = fh.load();
        const float sa = (W > 0 && H > 0) ? float(W) / float(H) : 1.777f;
        float va = windows[i].source ? windows[i].source->frame_aspect() : 0.f;
        if (va <= 0.f) va = sa;                       // ще не знаємо кадр — беремо як екран
        layout::Placement r = layout::fit_source(presets[active][i], va, sa);
        return {r.x, 1.0f - r.y - r.h, r.w, r.h};     // Y -> екранний (0=верх)
    }

    // Зсунути вікно на (dsx, dsy) в ЕКРАННИХ частках. По X прямо, по Y
    // інверсія (екранний низ = менший placement.y).
    void move(size_t i, float dsx, float dsy) {
        presets[active][i].x += dsx;
        presets[active][i].y -= dsy;
    }

    // Верхнє (за z) вікно під курсором, або -1.
    int window_at(float cx, float cy) const {
        int best = -1, bestz = -1000000;
        for (size_t i = 0; i < windows.size(); ++i) {
            Rect r = rect_of(i);
            if (cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h) {
                const int z = presets[active][i].z;
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
    void clamp_on_screen(size_t i) {
        Rect r = rect_of(i);
        // Скільки має лишитись видимим по кожній осі.
        const float keep_x = r.w * (1.f - kOffscreen);
        const float keep_y = r.h * (1.f - kOffscreen);

        float dsx = 0.f, dsy = 0.f;
        if (r.x + r.w < keep_x)      dsx = keep_x - (r.x + r.w);  // пішло вліво
        else if (r.x > 1.f - keep_x) dsx = (1.f - keep_x) - r.x;  // пішло вправо
        if (r.y + r.h < keep_y)      dsy = keep_y - (r.y + r.h);  // пішло вгору
        else if (r.y > 1.f - keep_y) dsy = (1.f - keep_y) - r.y;  // пішло вниз
        move(i, dsx, dsy);
    }

    // Розгорнути вікно рівно по краях екрана: коробка на всю площу,
    // прикріплена центром. Картинка вписується в неї за пропорцією, тож
    // упирається в краї тією віссю, якою дозволяє кадр.
    //
    // Це опорна точка для всього іншого: після довгого совання й зуму
    // повернутись до чогось передбачуваного інакше нічим.
    void fill_screen(size_t i) {
        layout::Placement& pl = presets[active][i];
        pl.x = 0.5f; pl.y = 0.5f;
        pl.w = 1.0f; pl.h = 1.0f;
        pl.anchor = layout::Anchor::Center;
        pl = layout::sanitize(pl);
    }

    void mark_dirty() {
        dirty_mask |= (1u << active);
        last_edit = now_ms();
    }

    // Провалити вікно wi на ОДИН рівень нижче в стеку: свап із тим, що
    // прямо під ним, і z переприсвоюються КОМПАКТНО (0..n-1). Так вони
    // завжди сусідні, а не розповзаються від повторних кліків.
    void lower_window(size_t wi) {
        auto& pv = presets[active];
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
    void button_rect(int b, float* x, float* y, float* w, float* h) const {
        const int W = fw.load(), H = fh.load();
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
            std::vector<layout::Placement> snap;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (dirty_mask && now_ms() - last_edit >= cfg.autosave_ms) {
                    for (int p = 0; p < cfg.preset_count; ++p)
                        if (dirty_mask & (1u << p)) { p_to_save = p; break; }
                    if (p_to_save >= 0) {
                        snap = presets[p_to_save];
                        dirty_mask &= ~(1u << p_to_save);
                    }
                }
            }
            if (p_to_save >= 0) save_preset(p_to_save, snap);   // I/O поза локом
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

void ScreenPresets::set_frame_size(int width, int height) {
    impl_->fw.store(width);
    impl_->fh.store(height);
}

const std::vector<render::OverlayImage>& ScreenPresets::images() const {
    return impl_->images;
}

bool ScreenPresets::acquire(render::DrawList& out) {
    Impl& d = *impl_;
    const int W = d.fw.load(), H = d.fh.load();
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
                if (pos < d.cfg.preset_count && pos != d.active) {
                    d.active = pos; d.selected = -1; d.apply_pending = true;
                }
            }
        }
    }

    float cx = 0.f, cy = 0.f;
    bool have_ptr = false;
    if (d.pointer) {
        const PointerState p = d.pointer->state();
        if (p.present) {
            have_ptr = true;
            cx = float(p.x) / float(W);
            cy = float(p.y) / float(H);

            // --- 2) кнопки 1..3: клік перемикає пресет ---
            bool on_button = false;
            for (int b = 0; b < d.cfg.preset_count; ++b) {
                float bx, by, bw, bh;
                d.button_rect(b, &bx, &by, &bw, &bh);
                if (cx >= bx && cx <= bx + bw && cy >= by && cy <= by + bh) {
                    on_button = true;
                    if (p.clicks > d.seen_clicks && b != d.active) {
                        d.active = b; d.selected = -1; d.apply_pending = true;
                    }
                }
            }
            if (p.clicks > d.seen_clicks) d.seen_clicks = p.clicks;

            // --- 3) ліва кнопка: ОБРАТИ вікно (і почати перетяг) ---
            if (p.left && !d.was_left) {                 // натиснули
                if (on_button) { d.drag_win = -1; }
                else {
                    const int wi = d.window_at(cx, cy);
                    if (wi >= 0) d.selected = wi;        // обрали
                    d.drag_win = wi;                     // і тягнемо його
                }
                d.prev_cx = cx; d.prev_cy = cy;
            }
            if (p.left && d.drag_win >= 0) {              // тягнемо обране
                d.move((size_t)d.drag_win, cx - d.prev_cx, cy - d.prev_cy);
                d.clamp_on_screen((size_t)d.drag_win);
                d.mark_dirty(); d.apply_pending = true;
            }
            if (!p.left) d.drag_win = -1;
            d.prev_cx = cx; d.prev_cy = cy;
            d.was_left = p.left;

            // --- 4) колесо: масштаб ОБРАНОГО вікна (не під курсором) ---
            const int64_t dwheel = p.wheel - d.seen_wheel;
            d.seen_wheel = p.wheel;
            if (dwheel != 0 && d.selected >= 0) {
                const size_t wi = (size_t)d.selected;
                Impl::Rect r0 = d.rect_of(wi);
                const float c0x = r0.x + r0.w * 0.5f, c0y = r0.y + r0.h * 0.5f;
                layout::Placement& pl = d.presets[d.active][wi];
                float f = std::pow(1.0f + d.cfg.wheel_step, (float)dwheel);
                pl.w *= f; pl.h *= f;
                // Стеля — kMaxSize площ екрана (див. layout.hpp). Міряємо
                // по ВПИСАНІЙ картинці, а не по коробці: коробка може бути
                // якої завгодно пропорції, а видно саме картинку, і
                // обмежувати треба те, що видно.
                Impl::Rect r = d.rect_of(wi);
                float s = 1.f;
                if (r.w > layout::kMaxSize) s = std::min(s, layout::kMaxSize / r.w);
                if (r.h > layout::kMaxSize) s = std::min(s, layout::kMaxSize / r.h);
                pl.w *= s; pl.h *= s;
                pl = layout::sanitize(pl);
                Impl::Rect r1 = d.rect_of(wi);           // тримати центр
                d.move(wi, c0x - (r1.x + r1.w * 0.5f), c0y - (r1.y + r1.h * 0.5f));
                d.clamp_on_screen(wi);
                d.mark_dirty(); d.apply_pending = true;
            }

            // --- 5) права кнопка: на ОБРАНОМУ — z−1; на іншому — зняти вибір ---
            if (p.rclicks > d.seen_rclicks) {
                d.seen_rclicks = p.rclicks;
                const int wi = on_button ? -1 : d.window_at(cx, cy);
                if (wi >= 0 && wi == d.selected) {
                    d.lower_window((size_t)wi);          // на один рівень вниз, z компактні
                    d.mark_dirty(); d.apply_pending = true;
                    std::fprintf(stderr, "[екрани] '%s' -> z %d\n",
                                 d.windows[wi].name.c_str(),
                                 d.presets[d.active][wi].z);
                } else {
                    d.selected = -1;                     // клац на іншому/порожньому — зняти
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
                const int wi = on_button ? -1 : d.window_at(cx, cy);
                if (wi >= 0) {
                    d.fill_screen((size_t)wi);
                    d.selected = wi;
                    d.mark_dirty(); d.apply_pending = true;
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
    // Пишемо поки в шар ЗАМОВЧУВАННЯ, тобто розкладка діє на всіх
    // екранах одразу — рівно як було до появи другого. Запис саме цьому
    // екрану з'явиться разом із перемикачем у редакторі.
    if (d.apply_pending && d.scene) {
        for (size_t i = 0; i < d.windows.size(); ++i)
            if (d.windows[i].source)
                d.scene->set_default(d.windows[i].source.get(), d.presets[d.active][i]);
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
    if (d.selected >= 0) {
        Impl::Rect r = d.rect_of((size_t)d.selected);
        const float t = 0.004f;                          // товщина рамки
        const float ta = t * float(H) / float(W);        // однакова в пікселях
        push(d.solid_idx, r.x, r.y, r.w, ta);            // верх
        push(d.solid_idx, r.x, r.y + r.h - ta, r.w, ta); // низ
        push(d.solid_idx, r.x, r.y, t, r.h);             // ліво
        push(d.solid_idx, r.x + r.w - t, r.y, t, r.h);   // право
    }

    for (int b = 0; b < d.cfg.preset_count; ++b) {
        float bx, by, bw, bh;
        d.button_rect(b, &bx, &by, &bw, &bh);
        const int img = b * 2 + (b == d.active ? 1 : 0);
        push(img, bx, by, bw, bh);
    }
    return true;
}

} // namespace vrx::ui
