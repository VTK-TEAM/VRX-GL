#include "screen_ui.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

namespace vrx::ui {
namespace {

// --- малювання картинок кодом ---
//
// Обидві потрібні маленькі: кнопка ~64 пікселі, курсор ~24. На такому
// розмірі будь-яка «краса» однаково зникає, тому фігури грубі й прості,
// зате читаються з відстані й не залежать від жодного файлу.

void put(std::vector<uint8_t>& px, int w, int x, int y,
         uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const size_t o = (size_t)(y * w + x) * 4;
    if (o + 3 >= px.size()) return;
    px[o + 0] = r; px[o + 1] = g; px[o + 2] = b; px[o + 3] = a;
}

void fill_rect(std::vector<uint8_t>& px, int w, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) put(px, w, x, y, r, g, b, a);
}

// Кнопка: темний квадрат із світлою рамкою, всередині три смужки різної
// довжини. Читається як "налаштування розкладки" — саме те, що за нею.
render::OverlayImage make_button(int size) {
    render::OverlayImage img;
    img.id = "ui:button";
    img.width = img.height = size;
    img.rgba.assign((size_t)size * size * 4, 0);

    const int r = size / 6;                       // радіус заокруглення
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Заокруглені кути: точка за межами кола в куті — прозора.
            const int cx = (x < r) ? r : (x >= size - r ? size - r - 1 : x);
            const int cy = (y < r) ? r : (y >= size - r ? size - r - 1 : y);
            const float d = std::hypot(float(x - cx), float(y - cy));
            if (d > r) continue;

            // БЕЗ РАМКИ: світлий обідець на заокругленому куті лягав
            // сходинками — згладжування тут нізвідки взятись, картинка
            // малюється кодом попіксельно. Форму тримають самі
            // заокруглення.
            put(img.rgba, size, x, y, 25, 28, 36, 200);
        }
    }

    // Три смужки: довга, коротка, середня.
    const int bar_h = std::max(2, size / 12);
    const int x0 = size / 4;
    const int lens[3] = {size / 2, size / 3, size * 5 / 12};
    for (int i = 0; i < 3; ++i) {
        const int y = size / 4 + i * (size / 5);
        fill_rect(img.rgba, size, x0, y, x0 + lens[i], y + bar_h, 235, 235, 240, 245);
    }
    return img;
}

// Кнопка в натиснутому стані. Та сама форма, інший колір: бурштиновий
// замість темного.
//
// Навіщо взагалі. Перехід у редактор не миттєвий — станція спершу
// коректно закриває запис (дозапис файлу, скидання кешів на флешку), і це
// секунди. Без відгуку виглядає так, наче клік не зарахувався: у лозі
// видно, як користувач тисне вісім разів поспіль, хоча ПЕРШИЙ клік уже
// був прийнятий. Кнопка, що змінила колір, відповідає на єдине питання,
// яке в цей момент є: "воно взагалі мене почуло?"
render::OverlayImage make_button_active(int size) {
    render::OverlayImage img = make_button(size);
    img.id = "ui:button_active";
    for (size_t o = 0; o + 3 < img.rgba.size(); o += 4) {
        if (img.rgba[o + 3] == 0) continue;                 // прозоре не чіпаємо
        const bool bright = img.rgba[o] > 128;              // рамка й смужки
        img.rgba[o + 0] = bright ? 255 : 200;
        img.rgba[o + 1] = bright ? 240 : 120;
        img.rgba[o + 2] = bright ? 190 : 20;
        img.rgba[o + 3] = 255;
    }
    return img;
}

// Курсор — класична стрілка, задана малюнком, а не арифметикою.
//
// Спершу я малював її циклом "довжина рядка росте" — виходив трикутник
// із хвостиком, який на екрані читався як щось невизначене. Форма
// вказівника впізнається саме обрисом, і описати її десятком рядків
// картинки чесніше й коротше, ніж формулою.
//
//   X — чорний контур,  . — біла заливка,  пробіл — прозоро
//
// Вістря — рівно в лівому верхньому пікселі: саме туди вказує позиція
// миші, і саме там має бути точка кліку.
const char* kCursorArt[] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "     X..X   ",
    "      X.X   ",
    "      XXX   ",
};
constexpr int kCursorW = 12;
constexpr int kCursorH = 18;

// scale — цілочисельне збільшення. Ціле навмисно: дробове дало б
// розмиті краї, а стрілка з розмитим контуром губиться на строкатому
// відео рівно так само, як і без контуру.
render::OverlayImage make_cursor(int scale) {
    render::OverlayImage img;
    img.id = "ui:cursor";
    img.width = kCursorW * scale;
    img.height = kCursorH * scale;
    img.rgba.assign((size_t)img.width * img.height * 4, 0);

    for (int y = 0; y < kCursorH; ++y) {
        for (int x = 0; x < kCursorW; ++x) {
            const char c = kCursorArt[y][x];
            if (c == ' ') continue;
            const bool outline = (c == 'X');
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    if (outline) put(img.rgba, img.width, x * scale + sx, y * scale + sy,
                                     0, 0, 0, 235);
                    else         put(img.rgba, img.width, x * scale + sx, y * scale + sy,
                                     255, 255, 255, 255);
                }
            }
        }
    }
    return img;
}

} // namespace

struct ScreenUi::Impl {
    Config cfg;
    Pointer* pointer = nullptr;

    std::vector<render::OverlayImage> images;
    static constexpr int kButton = 0;
    static constexpr int kButtonActive = 1;
    static constexpr int kCursor = 2;

    // Геометрія КОЖНОГО екрана. Кнопка й курсор рахуються в частках, а
    // частка береться від висоти саме того екрана, на якому малюємо.
    static constexpr int kRoles = 2;
    std::atomic<int> rw[kRoles] = {};
    std::atomic<int> rh[kRoles] = {};
    std::atomic<int> fw{0}, fh{0};   // основний — для сумісності всередині
    std::atomic<bool> editor_requested{false};

    // Натиснуто — і назад уже не вимикається: станція від цієї миті
    // закривається, і кнопка має лишатись підсвіченою до самого кінця.
    std::atomic<bool> pressed{false};

    // Скільки кліків уже враховано. Порівнюємо з лічильником миші —
    // так натискання не загубиться між кадрами й не спрацює двічі.
    uint64_t seen_clicks = 0;

    explicit Impl(Config c) : cfg(c) {
        images.push_back(make_button(48));
        images.push_back(make_button_active(48));
        images.push_back(make_cursor(2));
    }

    // Кнопка в ЧАСТКАХ екрана, правий ВЕРХНІЙ кут.
    void button_rect(float* x, float* y, float* w, float* h) const {
        const int W = fw.load(), H = fh.load();
        const float aspect = (W > 0 && H > 0) ? float(H) / float(W) : 0.5625f;
        *h = cfg.button_size;
        *w = cfg.button_size * aspect;          // квадрат на екрані
        // У СПІЛЬНОМУ РЯДУ з кнопками пресетів, на своєму місці. Крок і
        // проміжок ті самі, що там (gap = чверть ширини), інакше ряд
        // розповзся б.
        const float gap = *w * 0.25f;
        *x = cfg.slot * (*w + gap);
        *y = 0.0f;
    }
};

// ---------------------------------------------------------------------

ScreenUi::ScreenUi(Config cfg) : impl_(new Impl(cfg)) {}
ScreenUi::~ScreenUi() = default;

void ScreenUi::attach(Pointer* pointer) { impl_->pointer = pointer; }

bool ScreenUi::start() { return true; }
void ScreenUi::stop() {}

void ScreenUi::set_frame_size(int role, int width, int height) {
    Impl& d = *impl_;
    if (role < 0 || role >= Impl::kRoles) return;
    d.rw[role].store(width);
    d.rh[role].store(height);
    if (role == 0) {
        d.fw.store(width);
        d.fh.store(height);
    }

    // КУРСОР ХОДИТЬ ПО ВСІХ ЕКРАНАХ, тож межі йому задаємо СПИСКОМ, а не
    // одним прямокутником. Робить це саме цей оверлей: він курсор і
    // малює, отже він і єдиний, хто зобов'язаний знати всю геометрію.
    std::vector<std::pair<int, int>> sizes;
    for (int r = 0; r < Impl::kRoles; ++r) {
        const int w = d.rw[r].load(), h = d.rh[r].load();
        if (w > 0 && h > 0) sizes.push_back({w, h});
    }
    if (d.pointer && !sizes.empty()) {
        d.pointer->set_bounds(sizes[0].first, sizes[0].second);
        d.pointer->set_screens(sizes);
    }
}

const std::vector<render::OverlayImage>& ScreenUi::images() const {
    return impl_->images;
}

bool ScreenUi::acquire(int role, render::DrawList& out) {
    Impl& d = *impl_;
    if (role < 0 || role >= Impl::kRoles) return false;
    const int W = d.rw[role].load(), H = d.rh[role].load();
    if (W <= 0 || H <= 0) return false;

    float bx, by, bw, bh;
    d.button_rect(&bx, &by, &bw, &bh);

    out.quads.clear();

    auto push = [&out](int image, float x, float y, float w, float h) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;
        q.x[1] = x + w; q.y[1] = y;
        q.x[2] = x;     q.y[2] = y + h;
        q.x[3] = x + w; q.y[3] = y + h;
        q.u[0] = 0.f; q.v[0] = 0.f;
        q.u[1] = 1.f; q.v[1] = 0.f;
        q.u[2] = 0.f; q.v[2] = 1.f;
        q.u[3] = 1.f; q.v[3] = 1.f;
        out.quads.push_back(q);
    };

    // КНОПКА РЕДАКТОРА — ЛИШЕ НА ОСНОВНОМУ. Редактор розкладки OSD це
    // окрема програма під X, вона одна й показується там; кнопка на
    // додатковому екрані вела б у те саме місце, тільки збивала б з
    // пантелику.
    if (role == 0) {
        push(d.pressed.load() ? Impl::kButtonActive : Impl::kButton, bx, by, bw, bh);
    }

    if (d.pointer) {
        const PointerState p = d.pointer->state();
        // КУРСОР МАЛЮЄ ЛИШЕ ТОЙ ЕКРАН, НА ЯКОМУ ВІН ЗАРАЗ. Інакше він
        // роздвоївся б: та сама позиція в частках існує на обох.
        if (p.present && p.screen == role) {
            // Курсор малюємо ПІСЛЯ кнопки — він завжди поверх.
            //
            // Висота в частках екрана, ширина з неї ж за пропорцією
            // малюнка: інакше на екрані з іншим співвідношенням сторін
            // стрілку розплющило б.
            const float ch = 0.035f;
            const float cw = ch * (float(kCursorW) / float(kCursorH))
                                * (float(H) / float(W));
            push(Impl::kCursor, float(p.x) / float(W), float(p.y) / float(H), cw, ch);

            // КЛІК ПО КНОПЦІ. Рахуємо за лічильником натискань, а не за
            // станом кнопки: між двома кадрами миша встигає і натиснути,
            // і відпустити, і стан у цей момент уже нічого не покаже.
            if (role == 0 && p.clicks > d.seen_clicks) {
                d.seen_clicks = p.clicks;
                const float px = float(p.x) / float(W);
                const float py = float(p.y) / float(H);
                if (px >= bx && px <= bx + bw && py >= by && py <= by + bh) {
                    d.editor_requested.store(true);
                    // Друкуємо лише перший: далі користувач може тиснути
                    // скільки завгодно, і десяток однакових рядків у лозі
                    // приховує те, що ПЕРШИЙ клік уже було прийнято.
                    if (!d.pressed.exchange(true)) {
                        std::fprintf(stderr, "[ui] натиснуто кнопку редактора,"
                                             " закриваю станцію\n");
                    }
                }
            }
        }
    }
    return true;
}

bool ScreenUi::take_editor_request() {
    return impl_->editor_requested.exchange(false);
}

} // namespace vrx::ui
