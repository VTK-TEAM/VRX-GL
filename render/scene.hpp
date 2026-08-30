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
// РОЗКЛАДКА НАЛЕЖИТЬ РОЛІ, А НЕ ЗАЛІЗЯЦІ. Розкладок рівно дві — основна
// й додаткова, — і вони їдуть за роллю разом із нею:
//
//   обидва монітори є   HDMI: основна   DP: додаткова
//   HDMI зник                           DP: ОСНОВНА
//   HDMI повернувся     HDMI: основна   DP: додаткова
//
// Тобто при промоції картинка на DP таки стрибне — і це потрібна
// поведінка, а не вада. Якщо основний монітор помер, пілот має побачити
// на вцілілому екрані саме ту розкладку, що потрібна для польоту, а не
// ту, що була налаштована другорядному.

#include "../layout/layout.hpp"
#include "../source/frame_source.hpp"

#include <map>
#include <mutex>

namespace vrx::render {

class Scene {
public:
    using Key = const source::FrameSource*;

    // Ролі екранів. Числа, а не enum class, бо це водночас індекс набору
    // розкладок і суфікс у назві файлу.
    static constexpr int kPrimary = 0;
    static constexpr int kSecondary = 1;

    // Роль екрана за його станом. Усе, що не основне, — додаткове.
    static int role_of(bool primary) { return primary ? kPrimary : kSecondary; }

    // Розкладка за замовчуванням: діє на будь-якому екрані, який не має
    // власної. Ставиться при старті станції.
    void set_default(Key src, const layout::Placement& p) {
        std::lock_guard<std::mutex> lk(mtx_);
        defaults_[src] = p;
    }

    // Розкладка КОНКРЕТНОЇ РОЛІ. Перекриває замовчування.
    void set(int role, Key src, const layout::Placement& p) {
        std::lock_guard<std::mutex> lk(mtx_);
        per_role_[role][src] = p;
    }

    // Забути все, що налаштовано цій ролі — далі діють замовчування.
    void clear(int role) {
        std::lock_guard<std::mutex> lk(mtx_);
        per_role_.erase(role);
    }

    // Де малювати це джерело в цій ролі. false — ніде: ані власної
    // розкладки, ані замовчування немає.
    bool get(int role, Key src, layout::Placement* out) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto s = per_role_.find(role);
        if (s != per_role_.end()) {
            const auto it = s->second.find(src);
            if (it != s->second.end()) { *out = it->second; return true; }
        }
        const auto d = defaults_.find(src);
        if (d != defaults_.end()) { *out = d->second; return true; }
        return false;
    }

    // ЧИ ПОКАЗУВАТИ OSD У ЦІЙ РОЛІ.
    //
    // Живе тут, поруч із розкладкою, бо це та сама річ — налаштування
    // ВИГЛЯДУ конкретного екрана, яке робить оператор і читає рендерер.
    // Типово: на основному так, на додатковому ні (ТЗ).
    void set_osd(int role, bool on) {
        std::lock_guard<std::mutex> lk(mtx_);
        osd_[role] = on;
    }

    bool osd(int role) const {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = osd_.find(role);
        return it != osd_.end() ? it->second : (role == kPrimary);
    }

private:
    mutable std::mutex mtx_;
    std::map<int, bool> osd_;
    std::map<Key, layout::Placement> defaults_;
    std::map<int, std::map<Key, layout::Placement>> per_role_;
};

} // namespace vrx::render
