#include "ui/player_ui.hpp"

#include <atomic>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <string>
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

// ЦИФРИ ВБУДОВАНІ, а не з атласа OSD.
//
// Потрібні рівно одинадцять знаків — десять цифр і двокрапка, — і тягти
// заради них залежність від шрифтової підсистеми OSD означало б зв'язати
// плеєр із нею назавжди. Растр 5x7 старий як світ, малюється в коді й
// важить кілька рядків.
constexpr int kGlyphs = 11;                 // 0..9 та ':'
constexpr int kGW = 5, kGH = 7, kScale = 3;
constexpr int kCellW = kGW * kScale + 2;    // +2: поле, щоб сусід не підмішувався
constexpr int kCellH = kGH * kScale + 2;

const uint8_t kFont[kGlyphs][kGH] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},   // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},   // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},   // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},   // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},   // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},   // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},   // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},   // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},   // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},   // 9
    {0x00,0x04,0x00,0x00,0x04,0x00,0x00},   // :
};

render::OverlayImage make_font() {
    render::OverlayImage img;
    img.id = "player:font";
    img.width = kCellW * kGlyphs;
    img.height = kCellH;
    img.rgba.assign((size_t)img.width * img.height * 4, 0);

    // КОЛІР СКРІЗЬ ОДНАКОВИЙ, МІНЯЄТЬСЯ ЛИШЕ ПРОЗОРІСТЬ.
    //
    // Інакше прозорі пікселі лишаються чорними, і при згладжуванні
    // текстури вони підмішуються до країв цифр — знак обростає темною
    // облямівкою, а на дрібному розмірі це виглядає як чорна підкладка.
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i + 0] = img.rgba[i + 1] = img.rgba[i + 2] = 245;
        img.rgba[i + 3] = 0;
    }

    for (int g = 0; g < kGlyphs; ++g)
        for (int row = 0; row < kGH; ++row)
            for (int col = 0; col < kGW; ++col) {
                if (!(kFont[g][row] & (1 << (kGW - 1 - col)))) continue;
                for (int dy = 0; dy < kScale; ++dy)
                    for (int dx = 0; dx < kScale; ++dx) {
                        const int x = g * kCellW + 1 + col * kScale + dx;
                        const int y = 1 + row * kScale + dy;
                        img.rgba[((size_t)y * img.width + x) * 4 + 3] = 255;
                    }
            }
    return img;
}

int glyph_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    return -1;
}

// Час доби з мікросекунд епохи — той самий, що показував годинник станції
// у мить запису.
std::string clock_hms(int64_t us) {
    const time_t sec = (time_t)(us / 1000000);
    struct tm tm {};
    localtime_r(&sec, &tm);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

} // namespace

struct PlayerUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    ScreenPresets* presets = nullptr;
    std::shared_ptr<source::PlayerSession> player[kRoles];

    std::vector<render::OverlayImage> images;
    int i_track = 0, i_fill = 1, i_knob = 2, i_font = 3, i_pad = 4;

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
        images.push_back(make_font());
        images.push_back(solid("pad", 0, 0, 0, 200));   // підкладка під цифри
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
    const int64_t len = pl.timeline_len_us();
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

    const int Wp = d.rw[role].load(std::memory_order_relaxed);
    const int Hp = d.rh[role].load(std::memory_order_relaxed);

    // Пропорція екрана потрібна, щоб цифри не розтягувало: квади задані в
    // частках, а екран не квадратний.
    const float sa = (Wp > 0 && Hp > 0) ? float(Hp) / float(Wp) : 0.5625f;
    const float th = d.cfg.text_h;
    const float cw = th * (float(kCellW) / float(kCellH)) * sa;

    auto push = [&out](int image, float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    };

    // Підпис із підкладкою. Без неї цифри губляться на світлій картинці —
    // а таймлайн лежить поверх відео, і яким воно буде, ми не знаємо.
    auto text = [&](const std::string& t, float x, float y) {
        const float w = cw * (float)t.size();
        push(d.i_pad, x - cw * 0.25f, y - th * 0.2f,
             w + cw * 0.5f, th * 1.4f);
        for (size_t i = 0; i < t.size(); ++i) {
            const int g = glyph_index(t[i]);
            if (g < 0) continue;
            render::OverlayQuad q;
            q.image = d.i_font;
            const float x0 = x + cw * float(i), x1 = x0 + cw;
            q.x[0] = x0; q.y[0] = y;      q.x[1] = x1; q.y[1] = y;
            q.x[2] = x0; q.y[2] = y + th; q.x[3] = x1; q.y[3] = y + th;
            const float u0 = float(g * kCellW) / float(kCellW * kGlyphs);
            const float u1 = float((g + 1) * kCellW) / float(kCellW * kGlyphs);
            q.u[0] = u0; q.v[0] = 0; q.u[1] = u1; q.v[1] = 0;
            q.u[2] = u0; q.v[2] = 1; q.u[3] = u1; q.v[3] = 1;
            out.quads.push_back(q);
        }
    };

    const double f = (double)pl.position_us() / (double)len;
    const float frac = f < 0 ? 0.f : (f > 1 ? 1.f : (float)f);

    push(d.i_track, bx, by, bw, bh);
    if (frac > 0.f) push(d.i_fill, bx, by, bw * frac, bh);

    // Повзунок — вужчий за висоту й трохи вищий за смугу, щоб його було
    // видно й на самому початку, де заповнення ще нульове.
    // Повзунок піднімається НАД смугою, а не звисає під неї: смуга стоїть
    // упритул до низу, і все, що нижче, просто не поміститься на екрані.
    // Повзунок рівно у висоту смуги: усе, що вище або нижче, або лізе на
    // картинку, або не міститься на екрані.
    const float kw = bh * 0.55f;
    const float knob_x = bx + bw * frac;
    push(d.i_knob, knob_x - kw * 0.5f, by, kw, bh);

    // Час НА РЕЄСТРАТОРІ, а не від початку сеансу: людина шукає момент за
    // годинником, а не за секундою запису. Початок сеансу — це настінний
    // час першого кадру, він є в журналі.
    const int64_t t0 = pl.start_wall_us();
    const std::string a_lbl = clock_hms(t0);
    const std::string b_lbl = clock_hms(t0 + len);

    // Краї — у бічних полях, по центру висоти смуги. Дрібний шрифт туди
    // влазить, і смуга від цього не коротшає.
    const float ty = by + (bh - th) * 0.5f;
    text(a_lbl, 0.004f, ty);
    text(b_lbl, 1.0f - 0.004f - cw * (float)b_lbl.size(), ty);

    // Поточний час — ПРЯМО НА СМУЗІ, поруч із повзунком, і з того боку,
    // де є місце: у правій половині підпис ішов би за край екрана.
    const std::string cur = clock_hms(t0 + pl.position_us());
    const float gap = cw * 0.8f;
    const float cx_lbl = frac > 0.5f ? knob_x - gap - cw * (float)cur.size()
                                     : knob_x + gap;
    text(cur, cx_lbl, ty);
    return true;
}

} // namespace vrx::ui
