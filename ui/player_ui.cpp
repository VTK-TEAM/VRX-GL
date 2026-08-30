#include "ui/player_ui.hpp"

#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <cstring>
#include <ctime>
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
constexpr int kGlyphs = 18;                 // 0..9 : - + . x I < >
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
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},   // -
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},   // +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},   // .
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},   // x
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},   // I
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02},   // <
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08},   // >
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

// Іконки транспорту. Малюються в коді з тієї ж причини, що й шрифт: їх
// чотири, вони прості, і файл на диску для них — зайва річ, яку ще й може
// не виявитись на місці.
//
// Колір скрізь однаковий, міняється лише прозорість — інакше згладжування
// підмішує до країв чорне, і знак обростає брудною облямівкою.
render::OverlayImage make_icon(const char* id, int kind,
                               uint8_t cr = 240, uint8_t cg = 240, uint8_t cb = 240) {
    const int S = 64;
    render::OverlayImage img;
    img.id = std::string("player:ic_") + id;
    img.width = img.height = S;
    img.rgba.assign((size_t)S * S * 4, 0);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i + 0] = cr; img.rgba[i + 1] = cg; img.rgba[i + 2] = cb;
        img.rgba[i + 3] = 0;
    }
    auto on = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= S || y >= S) return;
        img.rgba[((size_t)y * S + x) * 4 + 3] = 255;
    };
    // Трикутник вершиною вправо або вліво, у смузі [x0,x1).
    auto tri = [&](int x0, int x1, bool right) {
        const float h = float(x1 - x0);
        for (int x = x0; x < x1; ++x) {
            const float k = right ? float(x - x0) / h : float(x1 - x) / h;
            const int half = (int)((S * 0.34f) * (1.0f - k));
            for (int y = S / 2 - half; y <= S / 2 + half; ++y) on(x, y);
        }
    };
    auto bar = [&](int x0, int w) {
        for (int x = x0; x < x0 + w; ++x)
            for (int y = S / 2 - (int)(S * 0.34f); y <= S / 2 + (int)(S * 0.34f); ++y)
                on(x, y);
    };

    const int m = S / 5, bw = S / 9;
    switch (kind) {
        case 0: tri(m, S - m, true); break;                         // грати
        case 1: bar(m + S / 12, bw * 2); bar(S - m - S / 12 - bw * 2, bw * 2); break;
        case 2: tri(m, S - m - bw - 2, true); bar(S - m - bw, bw); break;  // кадр уперед
        case 3: bar(m, bw); tri(m + bw + 2, S - m, false); break;          // кадр назад
        case 4: {                                                          // знімок
            // Корпус із видошукачем і кружком об'єктива — знак, який
            // впізнають без підпису.
            const int top = S / 3, bot = S - S / 5;
            for (int y = top; y < bot; ++y)
                for (int x = m - 2; x < S - m + 2; ++x) on(x, y);
            for (int y = top - S / 10; y < top; ++y)
                for (int x = S / 2 - S / 10; x < S / 2 + S / 10; ++x) on(x, y);
            // Об'єктив вирізаємо, а не домальовуємо: на суцільному корпусі
            // видно саме отвір, і знак читається одразу.
            const float cx = S * 0.5f, cy = (top + bot) * 0.5f, r = S * 0.13f;
            for (int y = top; y < bot; ++y)
                for (int x = 0; x < S; ++x)
                    if (std::hypot(float(x) - cx, float(y) - cy) < r)
                        img.rgba[((size_t)y * S + x) * 4 + 3] = 0;
            break;
        }
        default: break;
    }
    return img;
}

int glyph_index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
        case ':': return 10;
        case '-': return 11;
        case '+': return 12;
        case '.': return 13;
        case 'x': return 14;
        case 'I': return 15;
        case '<': return 16;
        case '>': return 17;
        default:  return -1;
    }
}

// Рядок сеансу: "ДД.ММ ГГ:ХХ:СС  Г:ХХ:СС" — коли ввімкнули станцію і
// скільки тривав запис. Літер немає навмисно: у шрифті лише цифри й кілька
// знаків, і цього рівно вистачає.
std::string session_row(int64_t power_on_us, int64_t len_us) {
    const time_t sec = (time_t)(power_on_us / 1000000);
    struct tm tm {};
    localtime_r(&sec, &tm);
    const int64_t s = len_us / 1000000;
    char b[48];
    std::snprintf(b, sizeof(b), "%02d.%02d %02d:%02d:%02d  %d:%02d:%02d",
                  tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    return b;
}

// Час доби з мікросекунд епохи — той самий, що показував годинник станції
// у мить запису.
int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

std::string clock_hms(int64_t us) {
    const time_t sec = (time_t)(us / 1000000);
    struct tm tm {};
    localtime_r(&sec, &tm);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return b;
}

} // namespace

// Кнопка керування. Підпис несе і зміст, і розмір: ширина рахується з
// нього, тож ряд сам розкладається під будь-який набір.
struct Btn {
    const char* label;      // порожній, якщо кнопка іконкова
    int kind;
    double arg;
    float x, y, w, h;
    int icon = -1;          // індекс картинки; -1 = підпис
};

enum { kJump = 0, kPlay, kStep, kSpeedDown, kSpeedUp, kSpeedShow, kShot };

// Сходинки швидкості. Не безперервний повзунок: у полі треба влучати, а
// не підбирати, і шість значень покривають усе — від розгляду по кадрах
// до швидкого прогону.
const double kSpeeds[] = {0.2, 0.5, 1.0, 2.0, 4.0, 10.0};
constexpr int kSpeedCount = 6;

struct PlayerUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;
    ScreenPresets* presets = nullptr;
    std::shared_ptr<source::PlayerSession> player[kRoles];

    std::vector<render::OverlayImage> images;
    int i_track = 0, i_fill = 1, i_knob = 2, i_font = 3, i_pad = 4;
    int i_play = 5, i_pause = 6, i_next = 7, i_prev = 8, i_shot = 9, i_shot_ok = 10;

    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};

    uint64_t seen_clicks = 0;
    uint64_t seen_btn_clicks = 0;
    uint64_t seen_list_clicks = 0;
    std::function<std::vector<record::SessionBrief>()> sessions_cb;
    std::function<void(int)> opened_cb;
    std::function<int(int)> shot_cb;
    std::vector<record::SessionBrief> list;
    int64_t list_at_ns = 0;
    int scroll = 0;
    int64_t seen_wheel = 0;
    int64_t shot_flash_ns[kRoles] = {0, 0};
    bool was_open[kRoles] = {false, false};
    double resume_speed[kRoles] = {1.0, 1.0};   // куди повертатись із паузи
    char speed_lbl[kRoles][8] = {};             // підпис швидкості, живе між кадрами
    bool was_left = false;
    bool dragging = false;
    uint64_t serial = 0;

    Impl(Config c) : cfg(c) {
        images.push_back(solid("track", 58, 61, 68, 175));
        images.push_back(solid("fill", 240, 170, 60, 235));
        images.push_back(solid("knob", 250, 245, 235, 255));
        images.push_back(make_font());
        // Підкладка СІРА, а не чорна: чорний прямокутник поверх відео
        // читається як діра в картинці, сірий — як накладка.
        images.push_back(solid("pad", 62, 65, 72, 200));
        images.push_back(make_icon("play", 0));
        images.push_back(make_icon("pause", 1));
        images.push_back(make_icon("next", 2));
        images.push_back(make_icon("prev", 3));
        images.push_back(make_icon("shot", 4));
        // Зелений двійник тієї ж іконки: єдиний спосіб показати натискання,
        // коли колір у прямокутника задається текстурою, а не окремо.
        images.push_back(make_icon("shot_ok", 4, 90, 230, 120));
    }

    // Розкладка двох рядів кнопок. Один код і для малювання, і для
    // влучання — інакше вони неминуче розійшлись би.
    void build_buttons(std::vector<Btn>& out, float bar_y, float sa,
                       double speed, int role) {
        // ОДИН РЯД. Стрибки обабіч транспорту: рука тягнеться від центру
        // в потрібний бік, і думати, у якому ряду шукати, не доводиться.
        std::snprintf(speed_lbl[role], sizeof(speed_lbl[role]), "%.1fx", speed);
        const Btn row[] = {
            {"-10", kJump, -10000000, 0,0,0,0}, {"-5", kJump, -5000000, 0,0,0,0},
            {"-1",  kJump,  -1000000, 0,0,0,0}, {"-.5", kJump, -500000, 0,0,0,0},
            {"", kStep, -1, 0,0,0,0, i_prev},
            {"", kPlay,  0, 0,0,0,0, speed > 0.0 ? i_pause : i_play},
            {"", kStep,  1, 0,0,0,0, i_next},
            {"+.5", kJump,   500000, 0,0,0,0},  {"+1",  kJump, 1000000, 0,0,0,0},
            {"+5",  kJump,  5000000, 0,0,0,0},  {"+10", kJump, 10000000, 0,0,0,0},
            {"-",   kSpeedDown, 0, 0,0,0,0},
            {speed_lbl[role], kSpeedShow, 0, 0,0,0,0},
            {"+",   kSpeedUp, 0, 0,0,0,0},
            {"", kShot, 0, 0,0,0,0, i_shot},
        };

        const float th = cfg.btn_text;
        const float cw = th * (float(kCellW) / float(kCellH)) * sa;
        const float h = th + cfg.btn_pad;

        auto lay = [&](const Btn* src, int n, float y) {
            float total = 0;
            for (int i = 0; i < n; ++i)
                total += (src[i].icon >= 0 ? h
                                           : cw * (float)std::strlen(src[i].label) + cfg.btn_pad)
                         + (i ? cfg.btn_gap : 0.f);
            float x = 0.5f - total * 0.5f;
            for (int i = 0; i < n; ++i) {
                Btn b = src[i];
                b.w = src[i].icon >= 0 ? h
                                       : cw * (float)std::strlen(src[i].label) + cfg.btn_pad;
                b.h = h; b.x = x; b.y = y;
                out.push_back(b);
                x += b.w + cfg.btn_gap;
            }
        };
        lay(row, (int)(sizeof(row) / sizeof(row[0])), bar_y - cfg.row_gap - h);
    }

    static void quad(render::DrawList& out, int image,
                     float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;     q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h; q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0; q.v[0] = 0; q.u[1] = 1; q.v[1] = 0;
        q.u[2] = 0; q.v[2] = 1; q.u[3] = 1; q.v[3] = 1;
        out.quads.push_back(q);
    }

    void glyphs(render::DrawList& out, const std::string& t,
                float x, float y, float h, float cw) const {
        for (size_t i = 0; i < t.size(); ++i) {
            const int g = glyph_index(t[i]);
            if (g < 0) continue;
            render::OverlayQuad q;
            q.image = i_font;
            const float x0 = x + cw * float(i), x1 = x0 + cw;
            q.x[0] = x0; q.y[0] = y;     q.x[1] = x1; q.y[1] = y;
            q.x[2] = x0; q.y[2] = y + h; q.x[3] = x1; q.y[3] = y + h;
            const float u0 = float(g * kCellW) / float(kCellW * kGlyphs);
            const float u1 = float((g + 1) * kCellW) / float(kCellW * kGlyphs);
            q.u[0] = u0; q.v[0] = 0; q.u[1] = u1; q.v[1] = 0;
            q.u[2] = u0; q.v[2] = 1; q.u[3] = u1; q.v[3] = 1;
            out.quads.push_back(q);
        }
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

void PlayerUi::draw_list(int role, render::DrawList& out, float sa) {
    Impl& d = *impl_;

    // Перелік оновлюємо не частіше ніж раз на дві секунди: живий сеанс
    // росте, а перечитувати журнали щокадру ні до чого.
    const int64_t now = mono_ns();
    if (d.sessions_cb && (d.list_at_ns == 0 || now - d.list_at_ns > 2000000000LL)) {
        d.list_at_ns = now;
        d.list = d.sessions_cb();
        // Межа саме n-vis, а не n-1: читаємо list[i + scroll], де i < vis,
        // тож надто велике scroll вивело б індекс за межі вектора. Ловилось
        // би це лише тоді, коли сеансів було більше, ніж влазить, і перелік
        // потім скоротився.
        const int fit0 = (int)(d.cfg.list_fill / d.cfg.list_row);
        const int room = (int)d.list.size() - (fit0 > 0 ? fit0 : 1);
        if (d.scroll > room) d.scroll = room > 0 ? room : 0;
    }

    const float th = d.cfg.list_text;
    const float cw = th * (float(kCellW) / float(kCellH)) * sa;
    const float rowh = d.cfg.list_row;
    const int n = (int)d.list.size();
    const int fit = (int)(d.cfg.list_fill / rowh);
    const int vis = n < fit ? n : (fit > 0 ? fit : 1);

    const float w = cw * 22.0f + 0.04f;
    const float x = 0.5f - w * 0.5f;
    const float total = rowh * (vis > 0 ? vis : 1);
    const float y0 = 0.5f - total * 0.5f;

    Impl::quad(out, d.i_pad, x - 0.012f, y0 - 0.018f, w + 0.024f, total + 0.036f);

    // --- ввід: колесо гортає, клік обирає ---
    if (d.pointer) {
        const PointerState p = d.pointer->state();
        const int W = d.rw[role].load(std::memory_order_relaxed);
        const int H = d.rh[role].load(std::memory_order_relaxed);
        if (p.present && p.screen == role && W > 0 && H > 0) {
            if (d.seen_wheel == 0) d.seen_wheel = p.wheel;
            const int64_t dw = p.wheel - d.seen_wheel;
            if (dw != 0) {
                d.seen_wheel = p.wheel;
                d.scroll -= (int)dw;
                if (d.scroll < 0) d.scroll = 0;
                if (d.scroll > n - vis) d.scroll = n - vis < 0 ? 0 : n - vis;
            }
            if (p.clicks > d.seen_list_clicks) {
                d.seen_list_clicks = p.clicks;
                const float cx = float(p.x) / float(W);
                const float cy = float(p.y) / float(H);
                for (int i = 0; i < vis; ++i) {
                    const float ry = y0 + rowh * i;
                    if (cx < x || cx > x + w || cy < ry || cy > ry + rowh) continue;
                    const auto& b = d.list[i + d.scroll];
                    std::fprintf(stderr, "[плеєр] екран %d -> сеанс %s\n",
                                 role, b.id.c_str());
                    d.player[role]->request_open(b.journal);
                    break;
                }
            }
        }
    }

    for (int i = 0; i < vis; ++i) {
        const auto& b = d.list[i + d.scroll];
        const float ry = y0 + rowh * i;
        // Живий сеанс підсвічено: у полі саме він потрібен найчастіше.
        Impl::quad(out, b.live ? d.i_fill : d.i_track,
                   x, ry + rowh * 0.08f, w, rowh * 0.84f);
        d.glyphs(out, session_row(b.power_on_us, b.length_us),
                 x + 0.012f, ry + (rowh - th) * 0.5f, th, cw);
    }
}

void PlayerUi::set_on_shot(std::function<int(int role)> cb) {
    impl_->shot_cb = std::move(cb);
}

void PlayerUi::set_on_opened(std::function<void(int)> cb) {
    impl_->opened_cb = std::move(cb);
}

void PlayerUi::set_sessions(std::function<std::vector<record::SessionBrief>()> cb) {
    impl_->sessions_cb = std::move(cb);
}

bool PlayerUi::hit_bar(int role, float cx, float cy) const {
    Impl& d = *impl_;
    if (role < 0 || role >= kRoles || !d.player[role] || !d.presets) return false;
    if (d.presets->mode(role) != ScreenPresets::kPlayer) return false;
    if (d.player[role]->length_us() <= 0) return false;

    float bx, by, bw, bh;
    d.bar_rect(&bx, &by, &bw, &bh);

    // Уся смуга керування, а не лише сама доріжка: кнопки теж лежать
    // поверх відео, і клац по них не має доходити до вікна під ними.
    const float h = d.cfg.btn_text + d.cfg.btn_pad;
    const float top = by - d.cfg.row_gap - h;
    return cy >= top - bh && cx >= 0.0f && cx <= 1.0f;
}

bool PlayerUi::acquire(int role, render::DrawList& out) {
    Impl& d = *impl_;
    out.quads.clear();
    out.serial = ++d.serial;

    if (role < 0 || role >= kRoles || !d.player[role] || !d.presets) return true;
    if (d.presets->mode(role) != ScreenPresets::kPlayer) return true;

    auto& pl = *d.player[role];

    // Сеанс відкриває потік годинника, і момент цей нам не підвладний —
    // тому ловимо його ПЕРЕХОДОМ, а не в місці натискання.
    const bool now_open = pl.length_us() > 0;
    if (now_open != d.was_open[role]) {
        d.was_open[role] = now_open;
        if (now_open && d.opened_cb) d.opened_cb(role);
    }

    const int Wp0 = d.rw[role].load(std::memory_order_relaxed);
    const int Hp0 = d.rh[role].load(std::memory_order_relaxed);
    const float sa0 = (Wp0 > 0 && Hp0 > 0) ? float(Hp0) / float(Wp0) : 0.5625f;

    // СЕАНС ЩЕ НЕ ОБРАНО — показуємо список. Це і є вхід у плеєр: спершу
    // вибір, і лише потім усе інше. Відкривати найсвіжіший мовчки було
    // тимчасовим рішенням і виглядало як свавілля.
    const int64_t len = pl.timeline_len_us();
    if (len <= 0) {
        draw_list(role, out, sa0);
        return true;
    }

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
            // Тільки сама доріжка, а не вся смуга керування: інакше клац
            // по кнопці читався б як перемотка, і кнопки "не працювали б",
            // а таймлайн стрибав би туди, де їх натиснули.
            const bool in_bar = cx >= bx && cx <= bx + bw &&
                                cy >= by - bh && cy <= by + bh * 2.0f;

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
                if (llabs(want - pl.position_us()) > 400000) pl.request_seek(want);
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

    // --- кнопки керування ---
    std::vector<Btn> btns;
    d.build_buttons(btns, by, sa, pl.speed(), role);

    if (d.pointer) {
        const PointerState p = d.pointer->state();
        const int W2 = d.rw[role].load(std::memory_order_relaxed);
        const int H2 = d.rh[role].load(std::memory_order_relaxed);
        if (p.present && p.screen == role && W2 > 0 && H2 > 0 &&
            p.clicks > d.seen_btn_clicks) {
            const float cx = float(p.x) / float(W2);
            const float cy = float(p.y) / float(H2);
            for (const Btn& b : btns) {
                if (!(cx >= b.x && cx <= b.x + b.w && cy >= b.y && cy <= b.y + b.h))
                    continue;
                d.seen_btn_clicks = p.clicks;
                switch (b.kind) {
                    case kJump: pl.request_jump((int64_t)b.arg); break;
                    case kStep:
                        // Паузу ставить сам сеанс, коли виконує крок; тут
                        // лише запам'ятовуємо, куди повертатись.
                        if (pl.speed() > 0.0) d.resume_speed[role] = pl.speed();
                        pl.request_step(b.arg < 0 ? -1 : 1);
                        break;
                    case kPlay:
                        if (pl.speed() > 0.0) {
                            d.resume_speed[role] = pl.speed();
                            pl.set_speed(0.0);
                        } else {
                            pl.set_speed(d.resume_speed[role]);
                        }
                        break;
                    case kSpeedDown:
                    case kSpeedUp: {
                        // Від поточної швидкості шукаємо найближчу сходинку
                        // й рухаємось по списку. На паузі міняємо ту, до
                        // якої повернемось, — інакше кнопка мовчала б.
                        double cur = pl.speed() > 0.0 ? pl.speed() : d.resume_speed[role];
                        int idx = 0;
                        for (int i = 0; i < kSpeedCount; ++i)
                            if (std::fabs(kSpeeds[i] - cur) < std::fabs(kSpeeds[idx] - cur))
                                idx = i;
                        idx += (b.kind == kSpeedUp) ? 1 : -1;
                        if (idx < 0) idx = 0;
                        if (idx >= kSpeedCount) idx = kSpeedCount - 1;
                        if (pl.speed() > 0.0) pl.set_speed(kSpeeds[idx]);
                        else d.resume_speed[role] = kSpeeds[idx];
                        break;
                    }
                    case kShot: {
                        const int n = d.shot_cb ? d.shot_cb(role) : 0;
                        if (n > 0) d.shot_flash_ns[role] = mono_ns();
                        std::fprintf(stderr, "[знімок] екран %d: каналів %d\n", role, n);
                        break;
                    }
                    default: break;
                }
                break;
            }
        }
    }

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

    // Кнопки: підкладка й підпис. Активна швидкість підсвічена самим
    // підписом, окремого стану кнопкам не треба.
    const float btn_cw = d.cfg.btn_text * (float(kCellW) / float(kCellH)) * sa;
    for (const Btn& b : btns) {
        push(b.kind == kSpeedShow ? d.i_track : d.i_pad, b.x, b.y, b.w, b.h);
        int icon = b.icon;
        if (b.kind == kShot &&
            mono_ns() - d.shot_flash_ns[role] < 700000000LL) icon = d.i_shot_ok;
        if (icon >= 0) {
            // Іконка квадратна, тож по горизонталі стискаємо її під
            // пропорцію екрана — інакше знак поїде вшир.
            const float iw = b.h * sa, ix = b.x + (b.w - iw) * 0.5f;
            push(icon, ix, b.y, iw, b.h);
            continue;
        }
        const float tw = btn_cw * (float)std::strlen(b.label);
        const float tx = b.x + (b.w - tw) * 0.5f;
        const float tyb = b.y + (b.h - d.cfg.btn_text) * 0.5f;
        for (size_t i = 0; b.label[i]; ++i) {
            const int g = glyph_index(b.label[i]);
            if (g < 0) continue;
            render::OverlayQuad q;
            q.image = d.i_font;
            const float x0 = tx + btn_cw * float(i), x1 = x0 + btn_cw;
            const float y0 = tyb, y1 = tyb + d.cfg.btn_text;
            q.x[0] = x0; q.y[0] = y0; q.x[1] = x1; q.y[1] = y0;
            q.x[2] = x0; q.y[2] = y1; q.x[3] = x1; q.y[3] = y1;
            const float u0 = float(g * kCellW) / float(kCellW * kGlyphs);
            const float u1 = float((g + 1) * kCellW) / float(kCellW * kGlyphs);
            q.u[0] = u0; q.v[0] = 0; q.u[1] = u1; q.v[1] = 0;
            q.u[2] = u0; q.v[2] = 1; q.u[3] = u1; q.v[3] = 1;
            out.quads.push_back(q);
        }
    }
    return true;
}

} // namespace vrx::ui
