#pragma once

// OSD: телеметрія поверх відео.
//
// Перенесено з VRX. Уся змістовна логіка збережена дослівно — формат
// атласу, .bin гліфів, osd_config.json, розбір UTF-8, синтаксис <icon>,
// п'ять розмірів, типи елементів LABEL/VALUE/ENUM_SWITCH/BAR/HORIZON,
// правила "коли канал вважати недоступним". Змінено РІВНО ОДНЕ: метод
// виведення.
//
//     було:  CPU alpha-blit гліфа -> ARGB8888 dumb-буфер -> свій DRM-плейн
//     стало: квад із координатами гліфа -> список -> GL у спільний кадр
//
// Разом із плейном відпали подвійна буферизація dumb-буферів, IPlaneSource,
// PlaneState і ping-pong по on_presented — тут кадр один на всіх.
//
// ПОБІЧНО ЦЕ ЗНІМАЄ НЕВИЗНАЧЕНІСТЬ. У VRX над blit_glyph() висів чесний
// коментар: .bin зберігає нормалізовані 0..1 текстурні координати, а що
// вони означають у пікселях екрана — довелося вгадувати, бо атлас
// будувався для GL-рендерера, якого на той момент уже не було. Тут
// вгадувати нема чого: числа вживаються рівно так, як їх заклав білдер.
//
// ДВА ПОТОКИ, як і має бути.
//
//   приймач   — UDP-broadcast телеметрії (порт 50122), розбір кадру,
//               складання значень у VtTelemetryStorage. Живе всередині
//               VtTelemetryListener, свій сокет і свій потік.
//   збирач    — раз на rebuild_ms читає сховище, обчислює значення,
//               розкладає гліфи й публікує готовий список квадів.
//
// Рендерер не робить нічого з цього: він бере останній опублікований
// список і малює. Тому темп OSD (десятки Гц) ніяк не пов'язаний із темпом
// показу (60 Гц), і повільне форматування рядків не може з'їсти vblank.

#include "../render/overlay.hpp"
#include "osd_types.h"
#include "telemetry/vt_telemetry_listener.hpp"
#include "telemetry/vt_telemetry_storage.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vrx::osd {

struct OsdStats {
    uint64_t builds = 0;        // скільки списків зібрано
    uint64_t quads = 0;         // квадів в останньому списку
    uint64_t packets = 0;       // пакетів телеметрії прийнято
    uint64_t crc_fails = 0;     // відкинуто через CRC
    double build_ms = 0;        // скільки коштує збірка списку
};

class Osd : public render::Overlay {
public:
    struct Config {
        // Шляхи відносно робочого каталогу — ті самі імена, що у VRX,
        // щоб osd_config.json переносився без правок.
        std::string atlas_png = "atlas.png";
        std::string glyph_bin = "osd_glyph_info.bin";
        std::string config_json = "osd_config.json";

        // Порт телеметрії. POINT і STATION шлють на один і той самий,
        // розрізняти відправника не треба — id унікальні по ролі.
        uint16_t telemetry_port = 50122;

        // Темп перезбирання списку. 20 Гц — стеля того, що око бачить у
        // цифрах, і вп'ятеро дешевше за показ. Ставити 60 немає сенсу:
        // телеметрія приходить рідше.
        int rebuild_ms = 50;
    };

    explicit Osd(Config cfg);
    ~Osd() override;

    // Читає атлас, гліфи й layout. Повертає false з чіткою причиною,
    // якщо будь-якого файлу немає — часткова робота тут гірша за відмову.
    bool init();

    const char* name() const override { return "osd"; }
    bool start() override;
    void stop() override;

    void set_frame_size(int width, int height) override;

    const std::vector<render::OverlayImage>& images() const override;
    bool acquire(render::DrawList& out) override;

    OsdStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::osd
