#pragma once

// Екрани-пресети: ТРИ збережені розкладки відео-вікон, редаговані мишею.
//
// Окремий оверлей і ЄДИНИЙ власник геометрії вікон. Приймає рішення сам:
//   - який пресет активний — 3-позиційний перемикач на пульті (канал 15,
//     ПО ЗМІНІ) або три кнопки 1/2/3 зліва вгорі (мишею);
//   - де стоять вікна в активному пресеті — перетяг лівою кнопкою і
//     масштаб колесом (аспект зберігається, впираємось лише в краї
//     екрана, накладання дозволено);
//   - автозбереження кожного пресета у свій файл через 5 с без дій.
//
// Логіка вводу живе в acquire() (потік показу — дешево, без блокувань), а
// запис у файли виносить окремий потік: I/O в потоці показу коштував би
// vblank'ів.
//
// Замінює керування розкладкою з телеметрії (LayoutControl): тепер
// авторитет один. (Edge-приведення телеметрії 150..164 у пресети —
// наступним кроком.)

#include "../layout/layout.hpp"
#include "../osd/telemetry/vt_telemetry_storage.h"
#include "../render/overlay.hpp"
#include "../render/scene.hpp"
#include "../source/frame_source.hpp"
#include "pointer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace vrx::ui {

class ScreenPresets : public render::Overlay {
public:
    struct Config {
        // Файли пресетів: <prefix>1.json, <prefix>2.json, <prefix>3.json.
        std::string file_prefix = "screen";
        int preset_count = 3;

        float button_size = 0.03f;   // висота кнопки, частка висоти екрана
        float wheel_step = 0.06f;    // масштаб за один тік колеса
        int autosave_ms = 5000;

        // Перемикач екранів: канал + пороги 3 позицій (сирий CRSF 0..2047,
        // ті самі значення, що у VRX monitor_control_service).
        uint8_t switch_channel = 65;         // VT_TLM_RC_CH15
        int thr_low = 400, thr_high = 1400;  // <low=1, ≤high=2, >high=3
    };

    // Одне кероване вікно.
    // РЕЖИМ ЕКРАНА. Той самий механізм вікон обслуговує і ефір, і плеєр:
    // вони рухаються мишею, мають пресети й кнопки однаково. Різниця лише
    // в тому, чиї кадри показують, — а це вирішує, який набір вікон зараз
    // увімкнено на цій ролі.
    //
    // Дублювати заради плеєра всю механіку пресетів було б гірше, ніж
    // здається: дві копії одного коду з часом розходяться, і другий екран
    // починає поводитись "майже так само".
    enum Mode { kLive = 0, kPlayer = 1, kModes = 2 };

    struct Window {
        std::string name;                            // ключ у файлі
        std::shared_ptr<source::FrameSource> source;
        layout::Placement fallback;                  // якщо файлу пресета ще немає
        int mode = kLive;                            // до якого набору належить
    };

    explicit ScreenPresets(Config cfg);
    ~ScreenPresets() override;

    ScreenPresets(const ScreenPresets&) = delete;
    ScreenPresets& operator=(const ScreenPresets&) = delete;

    void attach(Pointer* pointer);
    void add_window(Window w);
    void set_telemetry(VtTelemetryStorage* tlm);   // для перемикача каналом 15; може бути nullptr

    const char* name() const override { return "presets"; }
    void attach_scene(render::Scene* scene);

    // Перемкнути роль між ефіром і плеєром. Вікна іншого набору стають
    // вимкненими в сцені, тобто рендерер їх просто не малює — жодного
    // окремого шляху показу для плеєра не з'являється.
    //
    // Активний пресет у кожного набору СВІЙ: перемикання 1/2/3 у плеєрі не
    // має чіпати розкладку ефіру, це різні задачі й різні розкладки.
    void set_mode(int role, int mode);
    int mode(int role) const;

    bool start() override;
    void stop() override;
    void set_frame_size(int role, int width, int height) override;
    const std::vector<render::OverlayImage>& images() const override;
    bool acquire(int role, render::DrawList& out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::ui
