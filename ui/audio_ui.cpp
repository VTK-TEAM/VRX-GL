#include "ui/audio_ui.hpp"

#include "ui/screen_presets.hpp"
#include "../osd/json.hpp"
#include "../osd/json_safe.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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

render::OverlayImage solid(const char* id, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    render::OverlayImage img;
    img.id = std::string("audio:") + id;
    img.width = img.height = 2;
    img.rgba.assign(2 * 2 * 4, 0);
    for (int i = 0; i < 4; ++i) {
        img.rgba[i * 4 + 0] = r; img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b; img.rgba[i * 4 + 3] = a;
    }
    return img;
}

void put(std::vector<uint8_t>& px, int w, int x, int y,
         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= w) return;
    const size_t o = (size_t)(y * w + x) * 4;
    if (o + 3 >= px.size()) return;
    px[o + 0] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
}

constexpr int kIcon = 48;

// Та сама заокруглена темна плитка, що й у кнопок ряду.
render::OverlayImage tile(const std::string& id) {
    render::OverlayImage img;
    img.id = "audio:" + id;
    img.width = img.height = kIcon;
    img.rgba.assign((size_t)kIcon * kIcon * 4, 0);
    const int rad = kIcon / 6;
    for (int y = 0; y < kIcon; ++y)
        for (int x = 0; x < kIcon; ++x) {
            const int cx = x < rad ? rad : (x >= kIcon - rad ? kIcon - rad - 1 : x);
            const int cy = y < rad ? rad : (y >= kIcon - rad ? kIcon - rad - 1 : y);
            if (std::hypot(float(x - cx), float(y - cy)) > rad) continue;
            put(img.rgba, kIcon, x, y, 25, 28, 36, 200);
        }
    return img;
}

// Перемикач метра: три стовпчики зростаючої висоти. Показано — кольорові
// (як сам метр), сховано — сірі.
render::OverlayImage make_meter_toggle(bool on) {
    render::OverlayImage img = tile(std::string("tog") + (on ? "1" : "0"));
    const uint8_t col[3][3] = {{90, 200, 110}, {235, 200, 70}, {235, 80, 70}};
    const int bw = kIcon / 7, gap = kIcon / 12;
    const int x0 = (kIcon - (3 * bw + 2 * gap)) / 2, base = kIcon - kIcon / 4;
    const float hk[3] = {0.30f, 0.48f, 0.66f};
    for (int k = 0; k < 3; ++k) {
        const int h = (int)(kIcon * hk[k]);
        const uint8_t r = on ? col[k][0] : 120, g = on ? col[k][1] : 125, b = on ? col[k][2] : 135;
        for (int y = base - h; y < base; ++y)
            for (int x = x0 + k * (bw + gap); x < x0 + k * (bw + gap) + bw; ++x)
                put(img.rgba, kIcon, x, y, r, g, b, on ? 255 : 210);
    }
    return img;
}

// Контроль тривоги: дзвоник. Увімкнено — червоний, вимкнено — сірий.
render::OverlayImage make_alarm_toggle(bool on) {
    render::OverlayImage img = tile(std::string("alarm") + (on ? "1" : "0"));
    const uint8_t r = on ? 240 : 120, g = on ? 80 : 125, b = on ? 70 : 135, a = on ? 255 : 210;
    const float cx = (kIcon - 1) * 0.5f;
    const int top = kIcon * 20 / 100, rim = kIcon * 66 / 100;
    for (int y = top; y < rim; ++y) {                       // купол, що розширюється донизу
        const float k = float(y - top) / float(rim - top);
        const float half = kIcon * (0.07f + 0.20f * std::sqrt(k));
        for (int x = (int)(cx - half); x <= (int)(cx + half); ++x) put(img.rgba, kIcon, x, y, r, g, b, a);
    }
    for (int y = rim; y < rim + kIcon / 14; ++y)            // обідок
        for (int x = (int)(cx - kIcon * 0.31f); x <= (int)(cx + kIcon * 0.31f); ++x)
            put(img.rgba, kIcon, x, y, r, g, b, a);
    const float ky = rim + kIcon * 0.12f, kr = kIcon * 0.065f;   // язичок
    for (int y = 0; y < kIcon; ++y)
        for (int x = 0; x < kIcon; ++x)
            if (std::hypot(float(x) - cx, float(y) - ky) <= kr) put(img.rgba, kIcon, x, y, r, g, b, a);
    return img;
}

// М'ЮТ: динамік. Звук є — світлий із хвилями; заглушено — червоний хрест.
render::OverlayImage make_mute_toggle(bool muted) {
    render::OverlayImage img = tile(std::string("mute") + (muted ? "1" : "0"));
    const uint8_t r = 200, g = 210, b = 225, a = 235;
    const float cy = (kIcon - 1) * 0.5f;
    const int bx0 = kIcon * 16 / 100, bx1 = kIcon * 30 / 100;
    const int by0 = kIcon * 40 / 100, by1 = kIcon * 60 / 100;
    for (int y = by0; y < by1; ++y)
        for (int x = bx0; x < bx1; ++x) put(img.rgba, kIcon, x, y, r, g, b, a);
    const int cx0 = bx1, cx1 = kIcon * 46 / 100;
    for (int x = cx0; x < cx1; ++x) {
        const float k = float(x - cx0) / float(cx1 - cx0);
        const int half = (int)((by1 - by0) * 0.5f + k * kIcon * 0.16f);
        for (int y = (int)cy - half; y <= (int)cy + half; ++y) put(img.rgba, kIcon, x, y, r, g, b, a);
    }
    const int th = std::max(2, kIcon / 16);
    if (muted) {
        const float x0 = kIcon * 0.56f, x1 = kIcon * 0.84f, y0 = kIcon * 0.36f, y1 = kIcon * 0.64f;
        for (int y = 0; y < kIcon; ++y)
            for (int x = 0; x < kIcon; ++x) {
                if (x < x0 - th || x > x1 + th || y < y0 - th || y > y1 + th) continue;
                const float u = (x - x0) / (x1 - x0), v = (y - y0) / (y1 - y0);
                const float d1 = std::fabs(u - v), d2 = std::fabs(u + v - 1.f);
                if (std::min(d1, d2) * (x1 - x0) <= th * 0.75f && u >= -0.05f && u <= 1.05f)
                    put(img.rgba, kIcon, x, y, 240, 70, 60, 255);
            }
    } else {
        const float ax = cx1 - kIcon * 0.04f;
        for (int k = 0; k < 2; ++k) {
            const float rad = kIcon * (0.15f + 0.12f * k);
            for (int y = 0; y < kIcon; ++y)
                for (int x = (int)ax; x < kIcon; ++x) {
                    const float d = std::hypot(float(x) - ax, float(y) - cy);
                    if (d <= rad && d >= rad - th &&
                        std::fabs(std::atan2(float(y) - cy, float(x) - ax)) < 0.85f)
                        put(img.rgba, kIcon, x, y, r, g, b, a);
                }
        }
    }
    return img;
}

// ГОЛОВНИЙ ВИМИКАЧ: знак живлення (коло з розривом угорі й риска).
// Звук увімкнено — зелений, вимкнено — сірий.
render::OverlayImage make_power_toggle(bool on) {
    render::OverlayImage img = tile(std::string("power") + (on ? "1" : "0"));
    const uint8_t r = on ? 90 : 120, g = on ? 210 : 125, b = on ? 110 : 135, a = on ? 255 : 210;
    const float cx = (kIcon - 1) * 0.5f, cy = (kIcon - 1) * 0.54f;
    const float rad = kIcon * 0.26f;
    const int th = std::max(2, kIcon / 13);
    for (int y = 0; y < kIcon; ++y)
        for (int x = 0; x < kIcon; ++x) {
            const float d = std::hypot(float(x) - cx, float(y) - cy);
            if (d > rad || d < rad - th) continue;
            // розрив угорі, де стоїть риска
            const float ang = std::atan2(float(x) - cx, cy - float(y));   // 0 = вгору
            if (std::fabs(ang) < 0.55f) continue;
            put(img.rgba, kIcon, x, y, r, g, b, a);
        }
    for (int y = (int)(cy - rad - th * 0.5f); y < (int)cy; ++y)
        for (int x = (int)(cx - th * 0.5f); x < (int)(cx + th * 0.5f + 1); ++x)
            put(img.rgba, kIcon, x, y, r, g, b, a);
    return img;
}

} // namespace

struct AudioUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    ScreenPresets* presets = nullptr;
    std::function<audio::AudioLevel()> level_cb;
    std::function<void(float)> volume_cb;
    std::function<void(bool)> enable_cb;

    std::vector<render::OverlayImage> images;
    int i_panel = 0, i_track = 0, i_fill = 0, i_knob = 0;
    int i_mbg = 0, i_dead = 0, i_green = 0, i_yellow = 0, i_red = 0, i_peak = 0;
    int i_thr_on = 0, i_thr_off = 0, i_frame = 0;
    int i_tog = 0, i_alarm = 0, i_mute = 0, i_power = 0;   // кожна: +0 вимк, +1 увімк
    int i_dim = 0;                                         // притлумити вимкнене

    static constexpr int kRoles = 2;
    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};

    struct R { float x = 0, y = 0, w = 0, h = 0; };
    static bool inside(const R& r, float cx, float cy) {
        return cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h;
    }

    // --- ввід (лише потік показу) ---
    uint64_t seen_clicks = 0;
    bool was_left = false;
    bool vol_drag = false;
    bool thr_drag = false;
    int thr_drag_role = -1;

    // ОБЛАСТІ З ПОПЕРЕДНЬОГО КАДРУ — для hit().
    //
    // hit() кличуть пресети зсередини СВОГО acquire(), тримаючи свій
    // м'ютекс. Спитати в них звідти будь-що означало б узяти той самий
    // м'ютекс удруге — дедлок. Тож відповідаємо тим, що бачили минулого
    // кадру: обидва шари живуть в одному потоці показу, і запізнення на
    // кадр для миші непомітне.
    bool pan_open[kRoles] = {false, false};
    R pan_rect[kRoles];
    bool hnd_valid[kRoles] = {false, false};
    R hnd_rect[kRoles];          // вушко порогу + рядок через метр
    R meter_in[kRoles];          // внутрішня шкала — для перерахунку миші в дБ

    // --- збережуваний стан ---
    std::mutex mtx;
    bool enabled = true;         // головний вимикач звуку
    float vol_s = 1.0f;          // повзунок 0..1; лінійна гучність = s*s
    bool muted = false;
    bool alarm_on = true;
    float thr_db = -12.0f;
    bool dirty = false;
    int64_t changed_ms = 0;

    std::thread saver;
    std::atomic<bool> running{false};

    // --- метр, окремо на кожен екран ---
    float bar_db[kRoles] = {-100.f, -100.f};
    float hold_db[kRoles] = {-100.f, -100.f};
    int64_t hold_ms[kRoles] = {0, 0};
    int64_t last_draw_ms[kRoles] = {0, 0};

    // --- тривога (одна на станцію) ---
    int64_t exceed_since = -1;   // відколи рівень безперервно над порогом
    int64_t below_since = -1;    // відколи під порогом (для провалів)
    int64_t alarm_until = 0;     // до якої миті мигає рамка
    int64_t last_eval = 0;
    bool alarm_logged = false;

    explicit Impl(Config c) : cfg(std::move(c)) {
        thr_db = cfg.threshold_db;
        auto add = [this](render::OverlayImage img) {
            images.push_back(std::move(img));
            return (int)images.size() - 1;
        };
        i_panel  = add(solid("panel", 38, 45, 61, 255));
        i_track  = add(solid("track", 70, 76, 90, 255));
        i_fill   = add(solid("fill", 255, 190, 60, 255));
        i_knob   = add(solid("knob", 250, 245, 235, 255));
        i_mbg    = add(solid("mbg", 0, 0, 0, 170));
        i_dead   = add(solid("dead", 60, 62, 70, 200));
        i_green  = add(solid("green", 90, 200, 110, 255));
        i_yellow = add(solid("yellow", 235, 200, 70, 255));
        i_red    = add(solid("red", 235, 80, 70, 255));
        i_peak   = add(solid("peak", 250, 250, 250, 255));
        i_thr_on = add(solid("thr1", 255, 70, 60, 255));
        i_thr_off= add(solid("thr0", 150, 150, 160, 220));
        i_frame  = add(solid("frame", 255, 0, 0, 255));
        i_tog    = add(make_meter_toggle(false));   add(make_meter_toggle(true));
        i_alarm  = add(make_alarm_toggle(false));   add(make_alarm_toggle(true));
        i_mute   = add(make_mute_toggle(false));    add(make_mute_toggle(true));
        i_power  = add(make_power_toggle(false));   add(make_power_toggle(true));
        i_dim    = add(solid("dim", 20, 22, 30, 170));
    }

    float to_frac(float db) const {
        const float f = (db - cfg.db_floor) / -cfg.db_floor;
        return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
    }

    // Панель під динаміком: [звук][метр][тривога][м'ют] і повзунок до кінця.
    static constexpr int kBtns = 4;
    bool panel(int role, R* pan, R btn[kBtns], R* sl) const {
        if (!presets || !presets->audio_popup(role)) return false;
        float bx, by, bw, bh;
        presets->audio_button_rect(role, &bx, &by, &bw, &bh);
        const int W = rw[role].load(), H = rh[role].load();
        const float padh = bh * 0.15f;
        const float padw = (W > 0 && H > 0) ? padh * float(H) / float(W) : padh * 0.5625f;
        const float gap = bw * 0.25f;
        pan->x = bx - padw;                       pan->y = by + bh + padh;
        pan->w = bw * cfg.panel_buttons + padw * 2.f;
        pan->h = bh + padh * 2.f;
        for (int k = 0; k < kBtns; ++k) {
            btn[k].x = bx + k * (bw + gap);       btn[k].y = pan->y + padh;
            btn[k].w = bw;                        btn[k].h = bh;
        }
        sl->x = btn[kBtns - 1].x + bw + padw * 3.f;  sl->y = btn[0].y;
        sl->w = (pan->x + pan->w - padw * 2.f) - sl->x;
        sl->h = bh;
        return sl->w > 0.f;
    }

    void touch() { dirty = true; changed_ms = now_ms(); }   // під mtx

    // Гучність, що реально йде на вихід: м'ют — нуль, інакше квадрат повзунка.
    void apply_volume() {
        float v;
        { std::lock_guard<std::mutex> lk(mtx); v = muted ? 0.f : vol_s * vol_s; }
        if (volume_cb) volume_cb(v);
    }

    void set_vol(float s) {
        s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (std::fabs(s - vol_s) < 0.002f) return;
            vol_s = s;
            touch();
        }
        apply_volume();
    }

    void set_thr(float db) {
        db = db < cfg.db_floor ? cfg.db_floor : (db > 0.f ? 0.f : db);
        std::lock_guard<std::mutex> lk(mtx);
        if (std::fabs(db - thr_db) < 0.05f) return;
        thr_db = db;
        touch();
    }

    void load_state() {
        std::ifstream f(cfg.file);
        if (!f.is_open()) return;
        nlohmann::json root;
        try { f >> root; } catch (...) { return; }
        const float s = json_get(root, "volume", 1.0f);
        vol_s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
        enabled = json_get(root, "enabled", true);
        muted = json_get(root, "muted", false);
        alarm_on = json_get(root, "alarm", true);
        const float t = json_get(root, "threshold_db", cfg.threshold_db);
        thr_db = t < cfg.db_floor ? cfg.db_floor : (t > 0.f ? 0.f : t);
    }

    void save_state(bool en, float s, bool mu, bool al, float t) {
        nlohmann::json root;
        root["enabled"] = en;
        root["volume"] = s;
        root["muted"] = mu;
        root["alarm"] = al;
        root["threshold_db"] = t;
        std::ofstream f(cfg.file);
        if (!f.is_open()) {
            std::fprintf(stderr, "[звук] не зберігся стан у %s\n", cfg.file.c_str());
            return;
        }
        f << root.dump(2) << "\n";
    }

    // I/O — не з потоку показу: запис файлу там коштував би vblank'ів.
    bool take_dirty(bool* en, float* s, bool* mu, bool* al, float* t, bool force) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!dirty || (!force && now_ms() - changed_ms <= 1000)) return false;
        *en = enabled; *s = vol_s; *mu = muted; *al = alarm_on; *t = thr_db;
        dirty = false;
        return true;
    }
    void saver_loop() {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            float s, t; bool en, mu, al;
            if (take_dirty(&en, &s, &mu, &al, &t, false)) save_state(en, s, mu, al, t);
        }
    }

    // ТРИВОГА. Перший раз — лише після alarm_hold_ms безперервного
    // перевищення. Коли рамка вже мигає, БУДЬ-ЯКЕ перевищення продовжує її
    // ще на alarm_tail_ms: "останнє перевищення" — це саме воно. Провал
    // коротший за alarm_gap_ms відлік не скидає.
    void eval_alarm(int64_t now) {
        if (now - last_eval < 10) return;          // раз на кадр, а не на кожен екран
        last_eval = now;
        bool on; float thr;
        { std::lock_guard<std::mutex> lk(mtx); on = alarm_on && enabled; thr = thr_db; }
        if (!on) { exceed_since = -1; alarm_until = 0; alarm_logged = false; return; }

        const audio::AudioLevel L = level_cb ? level_cb() : audio::AudioLevel{};
        const bool alive = L.age_ms >= 0 && L.age_ms < 1000;
        const bool above = alive && L.peak_db > thr;
        const bool active = now < alarm_until;

        if (above) {
            below_since = -1;
            if (exceed_since < 0) exceed_since = now;
            if (active || now - exceed_since >= cfg.alarm_hold_ms) {
                if (!active && !alarm_logged) {
                    std::fprintf(stderr, "[звук] ТРИВОГА: рівень %.1f дБ вище порогу %.1f дБ понад %d мс\n",
                                 L.peak_db, thr, cfg.alarm_hold_ms);
                    alarm_logged = true;
                }
                alarm_until = now + cfg.alarm_tail_ms;
            }
        } else {
            if (below_since < 0) below_since = now;
            if (now - below_since > cfg.alarm_gap_ms) exceed_since = -1;
        }
        if (alarm_logged && now >= alarm_until) {
            std::fprintf(stderr, "[звук] тривогу знято\n");
            alarm_logged = false;
        }
    }

    void push(render::DrawList& out, int image, float x, float y, float w, float h) const {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    }

    // МЕТР: стовпчик знизу вгору. Смуга — пік із миттєвим підйомом і
    // спадом 30 дБ/с, біла риска — утриманий пік. Червона (сіра, якщо
    // тривогу вимкнено) риска — поріг; за вушко зліва його тягнуть мишею.
    // Немає звуку довше секунди — порожня сіра шкала.
    void draw_meter(int role, render::DrawList& out, const R& m, int W, int H) {
        push(out, i_mbg, m.x, m.y, m.w, m.h);
        const float pw = m.w * 0.18f;
        const float ph = pw * float(W) / float(H);
        const R in{m.x + pw, m.y + ph, m.w - pw * 2.f, m.h - ph * 2.f};
        if (in.w <= 0.f || in.h <= 0.f) { hnd_valid[role] = false; return; }
        meter_in[role] = in;
        push(out, i_dead, in.x, in.y, in.w, in.h);

        const audio::AudioLevel L = level_cb ? level_cb() : audio::AudioLevel{};
        const bool alive = L.age_ms >= 0 && L.age_ms < 1000;

        const int64_t now = now_ms();
        float dt = last_draw_ms[role] ? float(now - last_draw_ms[role]) / 1000.f : 0.f;
        dt = dt < 0.f ? 0.f : (dt > 0.2f ? 0.2f : dt);
        last_draw_ms[role] = now;

        const float target = alive ? L.peak_db : -100.f;
        bar_db[role] = std::max(target, bar_db[role] - 30.f * dt);
        if (target >= hold_db[role]) { hold_db[role] = target; hold_ms[role] = now; }
        else if (now - hold_ms[role] > 1000) hold_db[role] -= 20.f * dt;

        if (alive) {
            const float fb = to_frac(bar_db[role]);
            const float fy = to_frac(cfg.db_yellow), fr = to_frac(cfg.db_red);
            auto seg = [&](int img, float a, float b) {
                if (b <= a) return;
                push(out, img, in.x, in.y + in.h * (1.f - b), in.w, in.h * (b - a));
            };
            seg(i_green, 0.f, std::min(fb, fy));
            seg(i_yellow, fy, std::min(fb, fr));
            seg(i_red, fr, fb);
            const float fh = to_frac(hold_db[role]);
            if (fh > 0.f) {
                const float th = std::max(in.h * 0.012f, 2.f / float(H));
                push(out, i_peak, in.x, in.y + in.h * (1.f - fh) - th * 0.5f, in.w, th);
            }
        }

        // поріг: риска через шкалу + вушко зліва від метра
        float thr; bool on;
        { std::lock_guard<std::mutex> lk(mtx); thr = thr_db; on = alarm_on; }
        const float ly = in.y + in.h * (1.f - to_frac(thr));
        const float lt = std::max(in.h * 0.012f, 3.f / float(H));
        const int img = on ? i_thr_on : i_thr_off;
        push(out, img, m.x, ly - lt * 0.5f, m.w, lt);
        // Вушко КВАДРАТНЕ в пікселях: висота задана, ширина — та сама в
        // пікселях (частки по осях різні, бо екран не квадратний).
        const float th = std::max(m.h * 0.045f, 12.f / float(H));
        const float tw = th * float(H) / float(W);
        push(out, img, m.x - tw, ly - th * 0.5f, tw, th);
        hnd_rect[role] = R{m.x - tw, ly - th * 0.5f, tw + m.w, th};
        hnd_valid[role] = true;
    }

    // Червона рамка по краях екрана, frame_px завтовшки.
    void draw_frame(render::DrawList& out, int W, int H) const {
        const float tx = float(cfg.frame_px) / float(W);
        const float ty = float(cfg.frame_px) / float(H);
        push(out, i_frame, 0.f, 0.f, 1.f, ty);
        push(out, i_frame, 0.f, 1.f - ty, 1.f, ty);
        push(out, i_frame, 0.f, 0.f, tx, 1.f);
        push(out, i_frame, 1.f - tx, 0.f, tx, 1.f);
    }
};

AudioUi::AudioUi(Config cfg) : impl_(new Impl(std::move(cfg))) {}
AudioUi::~AudioUi() { stop(); }

void AudioUi::attach(Pointer* p) { impl_->pointer = p; }
void AudioUi::attach_presets(ScreenPresets* sp) { impl_->presets = sp; }
void AudioUi::set_level(std::function<audio::AudioLevel()> cb) { impl_->level_cb = std::move(cb); }
void AudioUi::set_volume(std::function<void(float)> cb) { impl_->volume_cb = std::move(cb); }
void AudioUi::set_enabled_cb(std::function<void(bool)> cb) { impl_->enable_cb = std::move(cb); }

bool AudioUi::hit(int role, float cx, float cy) const {
    if (role < 0 || role >= Impl::kRoles) return false;
    const Impl& d = *impl_;
    if (d.pan_open[role] && Impl::inside(d.pan_rect[role], cx, cy)) return true;
    if (d.hnd_valid[role] && Impl::inside(d.hnd_rect[role], cx, cy)) return true;
    return false;
}

bool AudioUi::start() {
    Impl& d = *impl_;
    if (d.running.load()) return true;
    d.load_state();
    if (d.enable_cb) d.enable_cb(d.enabled);
    d.apply_volume();
    std::fprintf(stderr, "[звук] %s, гучність %.0f%%%s, тривога %s, поріг %.1f дБ\n",
                 d.enabled ? "увімкнено" : "ВИМКНЕНО (камера без мікрофона?)",
                 d.vol_s * 100.f, d.muted ? " (М'ЮТ)" : "",
                 d.alarm_on ? "увімкнена" : "вимкнена", d.thr_db);
    d.running.store(true);
    d.saver = std::thread([&d] { d.saver_loop(); });
    return true;
}

void AudioUi::stop() {
    Impl& d = *impl_;
    if (!d.running.exchange(false)) return;
    if (d.saver.joinable()) d.saver.join();
    float s, t; bool en, mu, al;
    if (d.take_dirty(&en, &s, &mu, &al, &t, true)) d.save_state(en, s, mu, al, t);   // остання зміна не губиться
}

void AudioUi::set_frame_size(int role, int width, int height) {
    if (role < 0 || role >= Impl::kRoles) return;
    impl_->rw[role].store(width);
    impl_->rh[role].store(height);
}

const std::vector<render::OverlayImage>& AudioUi::images() const { return impl_->images; }

bool AudioUi::acquire(int role, render::DrawList& out) {
    Impl& d = *impl_;
    out.quads.clear();
    if (role < 0 || role >= Impl::kRoles) return true;
    const int W = d.rw[role].load(), H = d.rh[role].load();
    if (W <= 0 || H <= 0) return true;

    const int64_t now = now_ms();
    bool en;
    { std::lock_guard<std::mutex> lk(d.mtx); en = d.enabled; }
    d.eval_alarm(now);

    // 1) метр — там, де його поставили пресети. Звук вимкнено — метра нема:
    // вимірювати нічого, і порожня шкала лише займала б екран.
    float mx, my, mw, mh;
    if (en && d.presets && d.presets->meter_rect(role, &mx, &my, &mw, &mh))
        d.draw_meter(role, out, Impl::R{mx, my, mw, mh}, W, H);
    else
        d.hnd_valid[role] = false;

    // 2) панель звуку під динаміком
    Impl::R pan, btn[Impl::kBtns], sl;
    const bool open = d.panel(role, &pan, btn, &sl);
    d.pan_open[role] = open;
    if (open) d.pan_rect[role] = pan;
    if (!open) d.vol_drag = false;

    if (d.pointer) {
        const PointerState p = d.pointer->state();
        if (p.present && p.screen == role) {
            const float cx = float(p.x) / float(W);
            const float cy = float(p.y) / float(H);
            const bool clicked = p.clicks > d.seen_clicks;
            const bool pressed = p.left && !d.was_left;

            if (open && clicked && Impl::inside(btn[0], cx, cy)) {
                // ГОЛОВНИЙ ВИМИКАЧ — діє завжди, решта лише при ввімкненому звуці.
                { std::lock_guard<std::mutex> lk(d.mtx); d.enabled = !d.enabled; en = d.enabled; d.touch(); }
                if (!en) d.alarm_until = 0;
                if (d.enable_cb) d.enable_cb(en);
                std::fprintf(stderr, "[звук] звук %s\n", en ? "увімкнено" : "ВИМКНЕНО — не пишеться й не грає");
            } else if (open && clicked && en) {
                if (Impl::inside(btn[1], cx, cy) && d.presets) {
                    d.presets->toggle_meter(role);
                } else if (Impl::inside(btn[2], cx, cy)) {
                    std::lock_guard<std::mutex> lk(d.mtx);
                    d.alarm_on = !d.alarm_on;
                    d.touch();
                    std::fprintf(stderr, "[звук] контроль тривоги: %s\n", d.alarm_on ? "увімкнено" : "вимкнено");
                } else if (Impl::inside(btn[3], cx, cy)) {
                    { std::lock_guard<std::mutex> lk(d.mtx); d.muted = !d.muted; d.touch(); }
                    d.apply_volume();
                    std::fprintf(stderr, "[звук] %s\n", d.muted ? "М'ЮТ" : "звук увімкнено");
                }
            }

            // Повзунок: зона — уся висота кнопки, а не тонка доріжка.
            if (pressed && open && en && Impl::inside(sl, cx, cy)) d.vol_drag = true;
            // Вушко порогу: тягнеш його — міняєш поріг, решту метра — рухаєш вікно.
            if (pressed && en && d.hnd_valid[role] && Impl::inside(d.hnd_rect[role], cx, cy)) {
                d.thr_drag = true; d.thr_drag_role = role;
            }
            if (!p.left) { d.vol_drag = false; d.thr_drag = false; }

            if (d.vol_drag && open && sl.w > 0.f) d.set_vol((cx - sl.x) / sl.w);
            if (d.thr_drag && d.thr_drag_role == role && d.hnd_valid[role]) {
                const Impl::R& in = d.meter_in[role];
                const float f = 1.f - (cy - in.y) / in.h;
                d.set_thr(d.cfg.db_floor + f * -d.cfg.db_floor);
            }
            d.was_left = p.left;
            d.seen_clicks = p.clicks;
        }
    }

    if (open) {
        float s; bool mu, al;
        { std::lock_guard<std::mutex> lk(d.mtx); s = d.vol_s; mu = d.muted; al = d.alarm_on; }
        d.push(out, d.i_panel, pan.x, pan.y, pan.w, pan.h);
        const bool shown = d.presets && d.presets->meter_visible(role);
        d.push(out, d.i_power + (en ? 1 : 0), btn[0].x, btn[0].y, btn[0].w, btn[0].h);
        d.push(out, d.i_tog + (shown ? 1 : 0), btn[1].x, btn[1].y, btn[1].w, btn[1].h);
        d.push(out, d.i_alarm + (al ? 1 : 0), btn[2].x, btn[2].y, btn[2].w, btn[2].h);
        d.push(out, d.i_mute + (mu ? 1 : 0), btn[3].x, btn[3].y, btn[3].w, btn[3].h);

        const float th = sl.h * 0.22f;
        const float ty = sl.y + (sl.h - th) * 0.5f;
        d.push(out, d.i_track, sl.x, ty, sl.w, th);
        // При м'юті заповнення сіре: положення збережене, але звуку немає.
        if (s > 0.f) d.push(out, mu ? d.i_dead : d.i_fill, sl.x, ty, sl.w * s, th);
        const float kh = sl.h * 0.70f;
        const float kw = kh * 0.45f * float(H) / float(W);
        d.push(out, d.i_knob, sl.x + sl.w * s - kw * 0.5f, sl.y + (sl.h - kh) * 0.5f, kw, kh);

        // Звук вимкнено — усе, крім вимикача, притлумлене: видно, що там є,
        // але воно зараз не діє.
        if (!en) {
            const float x0 = btn[1].x;
            d.push(out, d.i_dim, x0, btn[1].y, (sl.x + sl.w) - x0, btn[1].h);
        }
    }

    // 3) рамка тривоги — поверх усього, на обох екранах, мигає
    if (now < d.alarm_until && ((now / d.cfg.blink_ms) % 2) == 0) d.draw_frame(out, W, H);
    return true;
}

} // namespace vrx::ui
