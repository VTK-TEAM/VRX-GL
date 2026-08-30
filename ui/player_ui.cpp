#include "ui/player_ui.hpp"

#include <atomic>
#include <cstdio>
#include <vector>

namespace vrx::ui {
namespace {

constexpr int kRoles = 2;

// Однотонна картинка. Кольору в квадах немає, тож кожен відтінок — це
// власна текстура; вони крихітні й розтягуються на потрібний розмір.
render::OverlayImage solid(const char* id, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    render::OverlayImage img;
    img.id = std::string("player:") + id;
    img.width = img.height = 2;
    img.rgba.assign(2 * 2 * 4, 0);
    for (int i = 0; i < 4; ++i) {
        img.rgba[i * 4 + 0] = r; img.rgba[i * 4 + 1] = g;
        img.rgba[i * 4 + 2] = b; img.rgba[i * 4 + 3] = a;
    }
    return img;
}

} // namespace

struct PlayerUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    ScreenPresets* presets = nullptr;
    std::shared_ptr<source::PlayerSession> player[kRoles];

    std::vector<render::OverlayImage> images;
    int i_track = 0, i_fill = 1, i_knob = 2;

    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};

    uint64_t seen_clicks = 0;
    bool was_left = false;
    bool dragging = false;
    uint64_t serial = 0;

    Impl(Config c) : cfg(c) {
        images.push_back(solid("track", 30, 32, 38, 180));
        images.push_back(solid("fill", 240, 170, 60, 235));
        images.push_back(solid("knob", 250, 245, 235, 255));
    }

    // Смуга в частках екрана.
    void bar_rect(float* x, float* y, float* w, float* h) const {
        *x = cfg.side;
        *w = 1.0f - cfg.side * 2.0f;
        *h = cfg.bar_h;
        *y = 1.0f - cfg.bottom - cfg.bar_h;
    }
};

PlayerUi::PlayerUi(Config cfg) : impl_(std::make_unique<Impl>(cfg)) {}
PlayerUi::~PlayerUi() = default;

void PlayerUi::attach(Pointer* p) { impl_->pointer = p; }
void PlayerUi::attach_presets(ScreenPresets* sp) { impl_->presets = sp; }
void PlayerUi::attach_player(int role, std::shared_ptr<source::PlayerSession> s) {
    if (role >= 0 && role < kRoles) impl_->player[role] = std::move(s);
}

bool PlayerUi::start() { return true; }
void PlayerUi::stop() {}

void PlayerUi::set_frame_size(int role, int width, int height) {
    if (role < 0 || role >= kRoles) return;
    impl_->rw[role].store(width, std::memory_order_relaxed);
    impl_->rh[role].store(height, std::memory_order_relaxed);
}

const std::vector<render::OverlayImage>& PlayerUi::images() const { return impl_->images; }

bool PlayerUi::hit_bar(int role, float cx, float cy) const {
    Impl& d = *impl_;
    if (role < 0 || role >= kRoles || !d.player[role] || !d.presets) return false;
    if (d.presets->mode(role) != ScreenPresets::kPlayer) return false;
    if (d.player[role]->length_us() <= 0) return false;

    float bx, by, bw, bh;
    d.bar_rect(&bx, &by, &bw, &bh);
    return cx >= bx && cx <= bx + bw && cy >= by - bh && cy <= by + bh * 2.0f;
}

bool PlayerUi::acquire(int role, render::DrawList& out) {
    Impl& d = *impl_;
    out.quads.clear();
    out.serial = ++d.serial;

    if (role < 0 || role >= kRoles || !d.player[role] || !d.presets) return true;
    if (d.presets->mode(role) != ScreenPresets::kPlayer) return true;

    auto& pl = *d.player[role];
    const int64_t len = pl.length_us();
    if (len <= 0) return true;              // сеанс не відкритий

    float bx, by, bw, bh;
    d.bar_rect(&bx, &by, &bw, &bh);

    // --- керування: клік або перетяг по смузі -> перемотка ---
    if (d.pointer) {
        const PointerState p = d.pointer->state();
        const int W = d.rw[role].load(std::memory_order_relaxed);
        const int H = d.rh[role].load(std::memory_order_relaxed);

        if (p.present && p.screen == role && W > 0 && H > 0) {
            const float cx = float(p.x) / float(W);
            const float cy = float(p.y) / float(H);

            // Зона влучання вища за саму смугу: смуга тонка, а цілитись у
            // неї мишею на екрані з метра — задача не з приємних.
            const bool in_bar = hit_bar(role, cx, cy);

            if (p.left && !d.was_left && in_bar) d.dragging = true;
            if (!p.left) d.dragging = false;

            if (d.dragging) {
                float f = (cx - bx) / bw;
                f = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
                const int64_t want = (int64_t)(f * (double)len);

                // Перемотуємо не щокадру, а лише коли ціль справді
                // зрушила: кожна перемотка піднімає пайплайни трьох
                // каналів наново, і робити це шістдесят разів на секунду
                // означало б не давати їм навіть стартувати.
                if (llabs(want - pl.position_us()) > 400000) pl.seek(want);
            }
            d.was_left = p.left;
            if (p.clicks > d.seen_clicks) d.seen_clicks = p.clicks;
        }
    }

    auto push = [&out](int image, float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    };

    const double f = (double)pl.position_us() / (double)len;
    const float frac = f < 0 ? 0.f : (f > 1 ? 1.f : (float)f);

    push(d.i_track, bx, by, bw, bh);
    if (frac > 0.f) push(d.i_fill, bx, by, bw * frac, bh);

    // Повзунок — вужчий за висоту й трохи вищий за смугу, щоб його було
    // видно й на самому початку, де заповнення ще нульове.
    // Повзунок піднімається НАД смугою, а не звисає під неї: смуга стоїть
    // упритул до низу, і все, що нижче, просто не поміститься на екрані.
    const float kw = bh * 0.5f, kh = bh * 1.6f;
    push(d.i_knob, bx + bw * frac - kw * 0.5f, by + bh - kh, kw, kh);
    return true;
}

} // namespace vrx::ui
