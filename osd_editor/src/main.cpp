// main.cpp — точка входу osd_editor.
//
// ЗАПУСК: cd <корінь> && ./build/osd_editor   (без аргументів)
//
// УСІ ресурси — atlas.png, osd_glyph_info.bin, osd_config.json,
// osd_catalog.json і background.jpg — відносні шляхи від ПАПКИ ЗАПУСКУ
// (CWD), а не від місця бінарника. Той самий принцип, що й у станції:
// один каталог розгортання, усе поруч, жодних аргументів.
#include "editor_app.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <string>

namespace {

bool is_lightdm_active() {
    const int rc = std::system("systemctl is-active --quiet lightdm >/dev/null 2>&1");
    return rc == 0;
}

void ensure_lightdm_running() {
    if (is_lightdm_active()) {
        return;
    }

    std::fprintf(stderr, "lightdm не активний, пробую запустити...\n");

    // Спочатку безпарольний sudo (типовий сценарій на стенді),
    // далі прямий systemctl (якщо процес вже від root).
    std::system("sudo -n systemctl start lightdm >/dev/null 2>&1 || systemctl start lightdm >/dev/null 2>&1");

    for (int i = 0; i < 20; ++i) {
        if (is_lightdm_active()) {
            std::fprintf(stderr, "lightdm запущено\n");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::fprintf(stderr,
                 "Не вдалося автозапустити lightdm.\n"
                 "Спробуй вручну: sudo systemctl start lightdm\n");
}

} // namespace

int main(int argc, char** argv) {
    (void)argc; (void)argv;   // аргументів немає — усе з папки запуску
    // ГРАФІЧНУ СЕСІЮ ПІДНІМАЄМО ЛИШЕ ДЛЯ X11.
    //
    // Раніше вона піднімалась завжди й безумовно — редактор жив на
    // робочому столі, іншого способу показати вікно не було. Але станція
    // працює на голому Linux без GUI, і там це рівно навпаки шкідливо:
    // lightdm забирає DRM master, після чого вивід напряму (SDL_VIDEODRIVER
    // =kmsdrm) стає неможливим у принципі. Ловилось наживо: редактор сам
    // піднімав X і сам же після цього не міг створити вікно.
    // Умова саме така: чи Є ВЖЕ куди малювати.
    //
    //   DISPLAY заданий            -> ми всередині X-сесії, хай навіть
    //                                 голої, піднятої через xinit без
    //                                 жодного робочого стола;
    //   SDL_VIDEODRIVER=kmsdrm     -> вивід напряму в DRM, X не потрібен.
    //
    // Спершу я перевіряв лише друге — і редактор, запущений під xinit,
    // сумлінно піднімав lightdm поверх X-сервера, у якому вже працював.
    const char* drv = std::getenv("SDL_VIDEODRIVER");
    const char* disp = std::getenv("DISPLAY");
    const bool have_output = (disp && *disp) ||
                             (drv && (std::string(drv) == "kmsdrm" ||
                                      std::string(drv) == "KMSDRM"));
    if (!have_output) {
        ensure_lightdm_running();
    }

    // Фон — background.jpg з папки запуску. Немає його — редактор працює
    // без фону (суцільний колір), layout однаково редагується.
    osdedit::EditorApp app;
    if (!app.init("background.jpg")) {
        std::fprintf(stderr,
            "\nНе вдалося запустити редактор.\n"
            "Перевір, що поточна директорія — корінь розгортання і в ній є:\n"
            "  atlas.png, osd_glyph_info.bin, osd_config.json, osd_catalog.json\n");
        return 1;
    }
    app.run();
    return 0;
}
