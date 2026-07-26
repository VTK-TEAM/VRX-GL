#pragma once

// Розкладка екрана: де і в якому порядку показувати відеоканали.
//
// Клас навмисно НЕ знає ні про роздільність екрана, ні про розмір
// джерел. Він описує лише БАЖАНЕ: прямокутник у частках 0..1, порядок
// за z, якір і два прапорці стану. Усе, що залежить від заліза —
// вписування за пропорцією, перерахунок у пікселі — робить рендерер,
// бо тільки він знає і розмір екрана, і фактичний розмір кадру.
//
// Чому все в частках 0..1, а не в пікселях: монітори різні, і розкладка
// має виглядати однаково на будь-якому. Ця ж конвенція вже діє в
// osd_config.json, де позиції задані частками (L/T).
//
// OSD сюди не входить: він завжди на весь екран і завжди зверху,
// параметризувати нічого.
//
// ПОТОКИ: пишуть керування (RC-канали з пульта) і відеоджерела
// (наявність потоку), читає рендерер — щокадру. Тому доступ під
// мьютексом, а рендерер бере snapshot() один раз на кадр: інакше можна
// прочитати x від старої розкладки, а w від нової, і картинка смикнеться.

#include <array>
#include <cstdint>
#include <mutex>

namespace vrx::layout {

// ---------------------------------------------------------------------
// Якір
// ---------------------------------------------------------------------

// Куди тулиться відео всередині заданого прямокутника після вписування
// за пропорцією. Відповідний кут (або середина сторони) відео торкається
// тієї самої точки прямокутника.
//
// Сітка 3x3, назви читаються по рядках:
//
//     TopLeft      TopCenter      TopRight
//     CenterLeft   Center         CenterRight
//     BottomLeft   BottomCenter   BottomRight
enum class Anchor {
    TopLeft,     TopCenter,     TopRight,
    CenterLeft,  Center,        CenterRight,
    BottomLeft,  BottomCenter,  BottomRight,
};

struct AnchorFactor { float x, y; };

// Частка вільного місця, що лишається ПЕРЕД відео по кожній осі.
// 0 — притиснуто до початку, 1 — до кінця, 0.5 — по центру.
constexpr AnchorFactor anchor_factor(Anchor a) {
    switch (a) {
        case Anchor::TopLeft:      return {0.0f, 0.0f};
        case Anchor::TopCenter:    return {0.5f, 0.0f};
        case Anchor::TopRight:     return {1.0f, 0.0f};
        case Anchor::CenterLeft:   return {0.0f, 0.5f};
        case Anchor::Center:       return {0.5f, 0.5f};
        case Anchor::CenterRight:  return {1.0f, 0.5f};
        case Anchor::BottomLeft:   return {0.0f, 1.0f};
        case Anchor::BottomCenter: return {0.5f, 1.0f};
        case Anchor::BottomRight:  return {1.0f, 1.0f};
    }
    return {0.5f, 0.5f};
}

// ---------------------------------------------------------------------
// Розміщення одного каналу
// ---------------------------------------------------------------------

struct Placement {
    // Прямокутник, У ЯКИЙ вписувати. Частки екрана від верхнього лівого.
    // Саме відео займе меншу площу, якщо його пропорція не збігається з
    // пропорцією цього прямокутника.
    float x = 0.0f, y = 0.0f, w = 1.0f, h = 1.0f;

    // Порядок малювання: більше — ближче до глядача. Обмін значеннями
    // між двома каналами і є swap у PiP, без окремої гілки в коді.
    int z = 0;

    Anchor anchor = Anchor::Center;

    // Розкладка хоче показувати цей канал. Керується з пульта.
    bool enabled = true;

    // Джерело реально є: потік піднявся і дає кадри. Виставляє той, хто
    // це знає — відеоджерело, а не розкладка.
    //
    // Два прапорці, а не один, бо це різні речі: "канал вимкнено
    // пілотом" і "зв'язок пропав" мають різні причини й різну реакцію.
    // Злиття їх в один позбавило б можливості показати, що саме сталося.
    bool source_present = false;

    // Малювати тільки якщо і хочемо, і є що.
    bool drawable() const { return enabled && source_present; }
};

// ---------------------------------------------------------------------
// Розкладка
// ---------------------------------------------------------------------

class Layout {
public:
    static constexpr int kChannels = 2;   // 0 — основний, 1 — другий

    // Узгоджений зріз усієї розкладки. Рендерер бере його один раз на
    // кадр і далі працює лише з ним.
    struct Snapshot {
        std::array<Placement, kChannels> ch{};
        uint32_t revision = 0;

        // Індекси каналів, які треба малювати, вже впорядковані за z
        // (від дальнього до ближчого). Недоступні й вимкнені сюди не
        // потрапляють — рендереру лишається просто пройти список.
        std::array<int, kChannels> order{};
        int order_count = 0;
    };

    Layout() {
        // Типово: основний на весь екран, другий вимкнений.
        p_[0] = Placement{0.0f, 0.0f, 1.0f, 1.0f, 0, Anchor::Center, true, false};
        p_[1] = Placement{0.0f, 0.0f, 1.0f, 1.0f, 1, Anchor::Center, false, false};
    }

    // --- читання ---

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mtx_);
        Snapshot s;
        s.revision = rev_;
        for (int i = 0; i < kChannels; ++i) s.ch[i] = p_[i];

        for (int i = 0; i < kChannels; ++i) {
            if (s.ch[i].drawable()) s.order[s.order_count++] = i;
        }
        // Сортування вставками: каналів одиниці, складніше не треба.
        for (int i = 1; i < s.order_count; ++i) {
            int v = s.order[i];
            int j = i - 1;
            while (j >= 0 && s.ch[s.order[j]].z > s.ch[v].z) {
                s.order[j + 1] = s.order[j];
                --j;
            }
            s.order[j + 1] = v;
        }
        return s;
    }

    uint32_t revision() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return rev_;
    }

    Placement placement(int ch) const {
        std::lock_guard<std::mutex> lk(mtx_);
        return valid(ch) ? p_[ch] : Placement{};
    }

    // --- запис ---

    void set_placement(int ch, const Placement& p) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(ch)) return;
        // source_present зберігаємо: ним володіє джерело, а не той, хто
        // міняє розкладку. Інакше зміна розкладки збивала б стан потоку.
        bool present = p_[ch].source_present;
        p_[ch] = p;
        p_[ch].source_present = present;
        ++rev_;
    }

    // Викликає відеоджерело, коли потік з'явився або пропав.
    void set_source_present(int ch, bool present) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(ch) || p_[ch].source_present == present) return;
        p_[ch].source_present = present;
        ++rev_;
    }

    void set_enabled(int ch, bool enabled) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(ch) || p_[ch].enabled == enabled) return;
        p_[ch].enabled = enabled;
        ++rev_;
    }

    void set_anchor(int ch, Anchor a) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(ch)) return;
        p_[ch].anchor = a;
        ++rev_;
    }

    // --- готові розкладки ---

    // Один канал на весь екран, другий вимкнено.
    void set_fullscreen(int ch) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(ch)) return;
        for (int i = 0; i < kChannels; ++i) {
            p_[i].x = 0.0f; p_[i].y = 0.0f; p_[i].w = 1.0f; p_[i].h = 1.0f;
            p_[i].anchor = Anchor::Center;
            p_[i].enabled = (i == ch);
            p_[i].z = (i == ch) ? 0 : 1;
        }
        ++rev_;
    }

    // Великий канал на весь екран, малий — у кут поверх нього.
    //
    // size — частка ЕКРАНА, у яку вписується маленький канал. Реальний
    // його розмір буде меншим по одній з осей, бо він вписується за
    // власною пропорцією і торкається кута, заданого якорем.
    void set_pip(int big_ch, float size = 0.30f,
                 Anchor corner = Anchor::TopRight, float margin = 0.02f) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!valid(big_ch)) return;
        const int small_ch = 1 - big_ch;

        p_[big_ch].x = 0.0f; p_[big_ch].y = 0.0f;
        p_[big_ch].w = 1.0f; p_[big_ch].h = 1.0f;
        p_[big_ch].anchor = Anchor::Center;
        p_[big_ch].enabled = true;
        p_[big_ch].z = 0;

        const AnchorFactor k = anchor_factor(corner);
        // Якір визначає і кут, і бік, з якого рахувати відступ: при
        // k.x==0 бокс тулиться вліво, при 1 — вправо.
        p_[small_ch].w = size;
        p_[small_ch].h = size;
        p_[small_ch].x = margin + (1.0f - size - 2.0f * margin) * k.x;
        p_[small_ch].y = margin + (1.0f - size - 2.0f * margin) * k.y;
        p_[small_ch].anchor = corner;
        p_[small_ch].enabled = true;
        p_[small_ch].z = 1;   // поверх великого

        ++rev_;
    }

    // Обмін порядком: те, що було зверху, йде вниз. Геометрії не чіпає.
    void swap_z() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::swap(p_[0].z, p_[1].z);
        ++rev_;
    }

    // Повна заміна ролей у PiP: великий стає маленьким і навпаки.
    void swap_channels() {
        std::lock_guard<std::mutex> lk(mtx_);
        // source_present лишається за СВОЇМ каналом: це властивість
        // джерела, а не місця на екрані. Міняються тільки геометрія,
        // порядок, якір і enabled.
        const bool present0 = p_[0].source_present;
        const bool present1 = p_[1].source_present;
        std::swap(p_[0], p_[1]);
        p_[0].source_present = present0;
        p_[1].source_present = present1;
        ++rev_;
    }

    // Два канали поруч, кожен у своїй половині.
    void set_split() {
        std::lock_guard<std::mutex> lk(mtx_);
        p_[0].x = 0.0f;  p_[0].y = 0.0f; p_[0].w = 0.5f; p_[0].h = 1.0f;
        p_[1].x = 0.5f;  p_[1].y = 0.0f; p_[1].w = 0.5f; p_[1].h = 1.0f;
        for (int i = 0; i < kChannels; ++i) {
            p_[i].anchor = Anchor::Center;
            p_[i].enabled = true;
            p_[i].z = i;
        }
        ++rev_;
    }

private:
    static constexpr bool valid(int ch) { return ch >= 0 && ch < kChannels; }

    mutable std::mutex mtx_;
    std::array<Placement, kChannels> p_{};
    uint32_t rev_ = 0;
};

// ---------------------------------------------------------------------
// Вписування — потрібне рендереру, тримаємо поруч із Placement
// ---------------------------------------------------------------------

// Зменшує прямокутник до пропорції джерела і притискає за якорем.
// Чорні поля НЕ виникають: замість того щоб малювати їх, ми просто
// малюємо менший прямокутник.
//
//   src_aspect    — пропорція ВИДИМОЇ частини кадру (crop, не розмір
//                   буфера) з урахуванням неквадратних пікселів
//   screen_aspect — width / height екрана в пікселях
inline Placement fit_source(const Placement& r, float src_aspect, float screen_aspect) {
    Placement out = r;
    if (r.w <= 0.0f || r.h <= 0.0f || src_aspect <= 0.0f || screen_aspect <= 0.0f) {
        return out;
    }

    // Пропорція самого прямокутника в пікселях: у частках він
    // "квадратний", але на екрані розтягується разом з екраном.
    const float rect_aspect = (r.w / r.h) * screen_aspect;

    if (src_aspect > rect_aspect) {
        out.h = r.w * screen_aspect / src_aspect;   // впираємось у ширину
    } else {
        out.w = r.h * src_aspect / screen_aspect;   // впираємось у висоту
    }

    const AnchorFactor k = anchor_factor(r.anchor);
    out.x = r.x + (r.w - out.w) * k.x;
    out.y = r.y + (r.h - out.h) * k.y;
    return out;
}

} // namespace vrx::layout
