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

// Перемикач метра: три стовпчики зростаючої висоти на тій самій
// заокругленій плитці, що й кнопки ряду. Показано — кольорові
// (зелений/жовтий/червоний, як сам метр), сховано — сірі.
render::OverlayImage make_meter_toggle(bool on) {
    const int size = 48;
    render::OverlayImage img;
    img.id = std::string("audio:tog") + (on ? "1" : "0");
    img.width = img.height = size;
    img.rgba.assign((size_t)size * size * 4, 0);

    const int rad = size / 6;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int cx = x < rad ? rad : (x >= size - rad ? size - rad - 1 : x);
            const int cy = y < rad ? rad : (y >= size - rad ? size - rad - 1 : y);
            if (std::hypot(float(x - cx), float(y - cy)) > rad) continue;
            put(img.rgba, size, x, y, 25, 28, 36, 200);
        }

    const uint8_t col[3][3] = {{90, 200, 110}, {235, 200, 70}, {235, 80, 70}};
    const int bw = size / 7, gap = size / 12;
    const int total = 3 * bw + 2 * gap;
    const int x0 = (size - total) / 2, base = size - size / 4;
    const float hk[3] = {0.30f, 0.48f, 0.66f};
    for (int k = 0; k < 3; ++k) {
        const int h = (int)(size * hk[k]);
        const uint8_t r = on ? col[k][0] : 120, g = on ? col[k][1] : 125, b = on ? col[k][2] : 135;
        for (int y = base - h; y < base; ++y)
            for (int x = x0 + k * (bw + gap); x < x0 + k * (bw + gap) + bw; ++x)
                put(img.rgba, size, x, y, r, g, b, on ? 255 : 210);
    }
    return img;
}

} // namespace

struct AudioUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    ScreenPresets* presets = nullptr;
    std::function<audio::AudioLevel()> level_cb;
    std::function<void(float)> volume_cb;

    std::vector<render::OverlayImage> images;
    int i_panel = 0, i_track = 0, i_fill = 0, i_knob = 0;
    int i_mbg = 0, i_dead = 0, i_green = 0, i_yellow = 0, i_red = 0, i_peak = 0;
    int i_tog = 0;   // +0 сховано, +1 показано

    static constexpr int kRoles = 2;
    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};

    // --- ввід (лише потік показу) ---
    uint64_t seen_clicks = 0;
    bool was_left = false;
    bool dragging = false;

    // ПРЯМОКУТНИК ПАНЕЛІ З ПОПЕРЕДНЬОГО КАДРУ — для hit().
    //
    // hit() кличуть пресети зсередини СВОГО acquire(), тримаючи свій
    // м'ютекс. Спитати в них звідти "чи відкрита панель" означало б узяти
    // той самий м'ютекс удруге — дедлок. Тож відповідаємо тим, що бачили
    // минулого кадру: обидва шари живуть в одному потоці показу, і
    // запізнення на кадр для миші непомітне.
    struct R { float x = 0, y = 0, w = 0, h = 0; };
    bool pan_open[kRoles] = {false, false};
    R pan_rect[kRoles];

    // --- гучність (повзунок 0..1; лінійна = s*s) ---
    std::mutex mtx;
    float vol_s = 1.0f;
    bool vol_dirty = false;
    int64_t vol_changed_ms = 0;

    std::thread saver;
    std::atomic<bool> running{false};

    // --- згладжування метра, окремо на кожен екран ---
    float bar_db[kRoles] = {-100.f, -100.f};
    float hold_db[kRoles] = {-100.f, -100.f};
    int64_t hold_ms[kRoles] = {0, 0};
    int64_t last_draw_ms[kRoles] = {0, 0};

    explicit Impl(Config c) : cfg(std::move(c)) {
        auto add = [this](render::OverlayImage img) {
            images.push_back(std::move(img));
            return (int)images.size() - 1;
        };
        // Панель — того ж графітового тону, що й ряд кнопок.
        i_panel  = add(solid("panel", 38, 45, 61, 255));
        i_track  = add(solid("track", 70, 76, 90, 255));
        i_fill   = add(solid("fill", 255, 190, 60, 255));
        i_knob   = add(solid("knob", 250, 245, 235, 255));
        // Метр: напівпрозорий чорний фон, щоб читався на будь-якому кадрі.
        i_mbg    = add(solid("mbg", 0, 0, 0, 170));
        i_dead   = add(solid("dead", 60, 62, 70, 200));
        i_green  = add(solid("green", 90, 200, 110, 255));
        i_yellow = add(solid("yellow", 235, 200, 70, 255));
        i_red    = add(solid("red", 235, 80, 70, 255));
        i_peak   = add(solid("peak", 250, 250, 250, 255));
        i_tog    = add(make_meter_toggle(false));
        add(make_meter_toggle(true));
    }

    float to_frac(float db) const {
        const float f = (db - cfg.db_floor) / -cfg.db_floor;
        return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
    }

    // Геометрія панелі під кнопкою-динаміком: перемикач метра зліва, далі
    // повзунок до кінця панелі.
    bool panel(int role, R* pan, R* tog, R* sl) const {
        if (!presets || !presets->audio_popup(role)) return false;
        float bx, by, bw, bh;
        presets->audio_button_rect(role, &bx, &by, &bw, &bh);
        const int W = rw[role].load(), H = rh[role].load();
        const float padh = bh * 0.15f;
        const float padw = (W > 0 && H > 0) ? padh * float(H) / float(W) : padh * 0.5625f;
        pan->x = bx - padw;            pan->y = by + bh + padh;
        pan->w = bw * cfg.panel_buttons + padw * 2.f;
        pan->h = bh + padh * 2.f;
        tog->x = bx;                   tog->y = pan->y + padh;
        tog->w = bw;                   tog->h = bh;
        sl->x = bx + bw + padw * 3.f;  sl->y = tog->y;
        sl->w = (pan->x + pan->w - padw * 2.f) - sl->x;
        sl->h = bh;
        return sl->w > 0.f;
    }

    void set_vol(float s) {
        s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (std::fabs(s - vol_s) < 0.002f) return;
            vol_s = s;
            vol_dirty = true;
            vol_changed_ms = now_ms();
        }
        if (volume_cb) volume_cb(s * s);
    }

    void load_volume() {
        std::ifstream f(cfg.file);
        if (!f.is_open()) return;
        nlohmann::json root;
        try { f >> root; } catch (...) { return; }
        float s = json_get(root, "volume", 1.0f);
        vol_s = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
    }

    void save_volume(float s) {
        nlohmann::json root;
        root["volume"] = s;
        std::ofstream f(cfg.file);
        if (!f.is_open()) {
            std::fprintf(stderr, "[звук] не збереглась гучність у %s\n", cfg.file.c_str());
            return;
        }
        f << root.dump(2) << "\n";
    }

    // I/O — не з потоку показу: запис файлу там коштував би vblank'ів.
    void saver_loop() {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            float s = 0.f;
            bool go = false;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (vol_dirty && now_ms() - vol_changed_ms > 1000) {
                    s = vol_s; vol_dirty = false; go = true;
                }
            }
            if (go) save_volume(s);
        }
    }

    // МЕТР: стовпчик знизу вгору. Смуга — пік із миттєвим підйомом і
    // спадом 30 дБ/с (щоб око встигало), біла риска — утриманий пік
    // (секунду стоїть, потім сповзає). Зони: зелена до db_yellow, жовта
    // до db_red, далі червона. Немає звуку довше секунди — порожня сіра
    // шкала: "камера мовчить" видно одразу і не плутається з тишею.
    void draw_meter(int role, render::DrawList& out, const R& m, int W, int H) {
        auto push = [&out](int image, float x, float y, float w, float h) {
            render::OverlayQuad q;
            q.image = image;
            q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
            q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
            q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
            q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
            out.quads.push_back(q);
        };

        push(i_mbg, m.x, m.y, m.w, m.h);
        const float pw = m.w * 0.18f;
        const float ph = pw * float(W) / float(H);         // те саме поле в пікселях
        const R in{m.x + pw, m.y + ph, m.w - pw * 2.f, m.h - ph * 2.f};
        if (in.w <= 0.f || in.h <= 0.f) return;
        push(i_dead, in.x, in.y, in.w, in.h);

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
        if (!alive) return;

        const float fb = to_frac(bar_db[role]);
        const float fy = to_frac(cfg.db_yellow), fr = to_frac(cfg.db_red);
        auto seg = [&](int img, float a, float b) {
            if (b <= a) return;
            push(img, in.x, in.y + in.h * (1.f - b), in.w, in.h * (b - a));
        };
        seg(i_green, 0.f, std::min(fb, fy));
        seg(i_yellow, fy, std::min(fb, fr));
        seg(i_red, fr, fb);

        const float fh = to_frac(hold_db[role]);
        if (fh > 0.f) {
            const float th = std::max(in.h * 0.012f, 2.f / float(H));
            push(i_peak, in.x, in.y + in.h * (1.f - fh) - th * 0.5f, in.w, th);
        }
    }
};

AudioUi::AudioUi(Config cfg) : impl_(new Impl(std::move(cfg))) {}
AudioUi::~AudioUi() { stop(); }

void AudioUi::attach(Pointer* p) { impl_->pointer = p; }
void AudioUi::attach_presets(ScreenPresets* sp) { impl_->presets = sp; }
void AudioUi::set_level(std::function<audio::AudioLevel()> cb) { impl_->level_cb = std::move(cb); }
void AudioUi::set_volume(std::function<void(float)> cb) { impl_->volume_cb = std::move(cb); }

bool AudioUi::hit(int role, float cx, float cy) const {
    if (role < 0 || role >= Impl::kRoles || !impl_->pan_open[role]) return false;
    const Impl::R& r = impl_->pan_rect[role];
    return cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h;
}

bool AudioUi::start() {
    Impl& d = *impl_;
    if (d.running.load()) return true;
    d.load_volume();
    if (d.volume_cb) d.volume_cb(d.vol_s * d.vol_s);
    std::fprintf(stderr, "[звук] гучність %.0f%% (повзунок)\n", d.vol_s * 100.f);
    d.running.store(true);
    d.saver = std::thread([&d] { d.saver_loop(); });
    return true;
}

void AudioUi::stop() {
    Impl& d = *impl_;
    if (!d.running.exchange(false)) return;
    if (d.saver.joinable()) d.saver.join();
    float s = 0.f;
    bool go = false;
    {
        std::lock_guard<std::mutex> lk(d.mtx);
        if (d.vol_dirty) { s = d.vol_s; d.vol_dirty = false; go = true; }
    }
    if (go) d.save_volume(s);     // остання зміна не має загубитись на виході
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

    auto push = [&out](int image, float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    };

    // 1) метр — там, де його поставили пресети
    float mx, my, mw, mh;
    if (d.presets && d.presets->meter_rect(role, &mx, &my, &mw, &mh))
        d.draw_meter(role, out, Impl::R{mx, my, mw, mh}, W, H);

    // 2) панель звуку під динаміком
    Impl::R pan, tog, sl;
    const bool open = d.panel(role, &pan, &tog, &sl);
    d.pan_open[role] = open;
    if (open) d.pan_rect[role] = pan;
    if (!open && d.dragging) d.dragging = false;

    float s = 0.f;
    { std::lock_guard<std::mutex> lk(d.mtx); s = d.vol_s; }

    if (d.pointer) {
        const PointerState p = d.pointer->state();
        if (p.present && p.screen == role) {
            const float cx = float(p.x) / float(W);
            const float cy = float(p.y) / float(H);
            auto in = [&](const Impl::R& r) {
                return cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h;
            };
            if (open) {
                if (p.clicks > d.seen_clicks && in(tog) && d.presets) d.presets->toggle_meter(role);
                // Зона повзунка — уся висота кнопки, а не тонка доріжка:
                // цілитись мишею в кілька пікселів на моніторі незручно.
                if (p.left && !d.was_left && in(sl)) d.dragging = true;
                if (!p.left) d.dragging = false;
                if (d.dragging && sl.w > 0.f) {
                    d.set_vol((cx - sl.x) / sl.w);
                    std::lock_guard<std::mutex> lk(d.mtx);
                    s = d.vol_s;
                }
            }
            d.was_left = p.left;
            d.seen_clicks = p.clicks;
        }
    }

    if (open) {
        push(d.i_panel, pan.x, pan.y, pan.w, pan.h);
        const bool shown = d.presets && d.presets->meter_visible(role);
        push(d.i_tog + (shown ? 1 : 0), tog.x, tog.y, tog.w, tog.h);

        const float th = sl.h * 0.22f;
        const float ty = sl.y + (sl.h - th) * 0.5f;
        push(d.i_track, sl.x, ty, sl.w, th);
        if (s > 0.f) push(d.i_fill, sl.x, ty, sl.w * s, th);
        const float kh = sl.h * 0.70f;
        const float kw = kh * 0.45f * float(H) / float(W);
        push(d.i_knob, sl.x + sl.w * s - kw * 0.5f, sl.y + (sl.h - kh) * 0.5f, kw, kh);
    }
    return true;
}

} // namespace vrx::ui
