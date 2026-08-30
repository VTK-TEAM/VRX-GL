#pragma once

// Екранне керування станції: курсор і кнопка переходу в редактор.
//
// ЧОМУ ЦЕ ОКРЕМИЙ ОВЕРЛЕЙ, А НЕ ЧАСТИНА OSD. OSD малює телеметрію за
// osd_config.json — його вміст задає користувач, і він же його редагує
// тим самим редактором, у який веде ця кнопка. Класти кнопку туди
// означало б дозволити випадково її пересунути, зменшити або видалити —
// і лишитися без єдиного способу відкрити редактор.
//
// Тому окремо: свої дві картинки, свої два квади, жодного конфігу.
//
// КАРТИНКИ МАЛЮЮТЬСЯ КОДОМ, а не читаються з файлів. Причина та сама:
// файл можна загубити при переносі папки, і станція лишиться без кнопки
// саме тоді, коли її нема як повернути. Тридцять рядків арифметики
// надійніші за зовнішній ресурс.

#include "../render/overlay.hpp"
#include "pointer.hpp"

#include <atomic>
#include <memory>

namespace vrx::ui {

class ScreenUi : public render::Overlay {
public:
    struct Config {
        // Розмір кнопки в частках ВИСОТИ екрана — щоб на будь-якій
        // роздільності вона лишалась однакового відносного розміру, як і
        // решта екранної графіки.
        //
        // 0.025 — це близько 19 пікселів на 768 і 27 на 1080. Маленька
        // навмисно: вона потрібна раз на політ, а видно її весь час.
        float button_size = 0.025f;


    };

    explicit ScreenUi(Config cfg);
    ~ScreenUi() override;

    // Джерело позиції курсора. Живе зовні: мишу читає окремий клас, а цей
    // лише малює те, що той намалював.
    void attach(Pointer* pointer);

    const char* name() const override { return "ui"; }
    bool start() override;
    void stop() override;
    void set_frame_size(int role, int width, int height) override;

    const std::vector<render::OverlayImage>& images() const override;
    bool acquire(int role, render::DrawList& out) override;

    // ЧИ ПОПРОСИЛИ РЕДАКТОР. Прапорець одноразовий: прочитали — згас.
    // Так натискання не може спрацювати двічі, навіть якщо його встигли
    // прочитати два різні місця.
    bool take_editor_request();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::ui
