#pragma once

// СЦЕНА: де саме показувати кожне джерело — ОКРЕМО ДЛЯ КОЖНОГО ЕКРАНА.
//
// ЧОМУ РОЗМІЩЕННЯ ПІШЛО З ДЖЕРЕЛА. Раніше `Placement` лежав усередині
// VideoSource, і кадр ніс його з собою: джерело знало, де його малюють.
// З одним екраном це працювало й виглядало навіть зручно.
//
// З двома — неможливо за визначенням: одне джерело має ДВА різні місця, а
// поле в ньому одне. Тобто розміщення ніколи й не було властивістю
// камери; воно завжди належало сцені, просто до появи другого екрана це
// ні на чому не проявлялось.
//
// ДВА ШАРИ, І ЦЕ НЕ ПРО ЗРУЧНІСТЬ.
//
//   default   — розкладка "за замовчуванням", спільна для всіх екранів;
//   екранний  — те, що оператор налаштував саме цьому екрану.
//
// Шар за замовчуванням потрібен рівно для одного випадку, зате важливого:
// монітор, якого ще ніхто не налаштовував. Помер HDMI, DP став основним —
// у нього своєї розкладки може не бути взагалі, і без запасного шару
// пілот отримав би ЧОРНИЙ екран замість картинки в момент, коли він щойно
// втратив монітор. З ним — побачить розкладку за замовчуванням.
//
// КЛЮЧ — ВКАЗІВНИК НА ДЖЕРЕЛО. Джерела живуть від старту до зупинки
// станції й не перестворюються, тож вказівник тут стабільніший за будь-
// який індекс: список джерел може змінитись, а тотожність камери — ні.
//
// НОМЕР ЕКРАНА — це СЛОТ виводу (див. display::Target::screen), а не
// порядковий номер за роллю. Ролі переставляються на ходу, слоти — ні.

#include "../layout/layout.hpp"
#include "../source/frame_source.hpp"

#include <map>
#include <mutex>

namespace vrx::render {

class Scene {
public:
    using Key = const source::FrameSource*;

    // Розкладка за замовчуванням: діє на будь-якому екрані, який не має
    // власної. Ставиться при старті станції.
    void set_default(Key src, const layout::Placement& p) {
        std::lock_guard<std::mutex> lk(mtx_);
        defaults_[src] = p;
    }

    // Розкладка КОНКРЕТНОГО екрана. Перекриває замовчування.
    void set(int screen, Key src, const layout::Placement& p) {
        std::lock_guard<std::mutex> lk(mtx_);
        per_screen_[screen][src] = p;
    }

    // Забути все, що налаштовано цьому екрану — далі діють замовчування.
    void clear(int screen) {
        std::lock_guard<std::mutex> lk(mtx_);
        per_screen_.erase(screen);
    }

    // Де малювати це джерело на цьому екрані. false — ніде: ані власної
    // розкладки, ані замовчування немає.
    bool get(int screen, Key src, layout::Placement* out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto s = per_screen_.find(screen);
        if (s != per_screen_.end()) {
            const auto it = s->second.find(src);
            if (it != s->second.end()) { *out = it->second; return true; }
        }
        const auto d = defaults_.find(src);
        if (d != defaults_.end()) { *out = d->second; return true; }
        return false;
    }

private:
    mutable std::mutex mtx_;
    std::map<Key, layout::Placement> defaults_;
    std::map<int, std::map<Key, layout::Placement>> per_screen_;
};

} // namespace vrx::render
