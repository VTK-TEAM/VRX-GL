#pragma once

// ЖИВИЙ ЗВУК КАМЕРИ НА АУДІОВИХІД СТАНЦІЇ.
//
// Камера шле звук окремим потоком: Opus/RTP, PT 98, 48 кГц моно, кадр
// 20 мс, бродкастом на UDP 5601 (інтерфейс камери, waybeam 0.43). На 5600
// лише відео, тож показ від звуку не залежить узагалі — і навпаки.
//
// ОДИН ВИХІД = ОДИН ЕКЗЕМПЛЯР СО СВОЇМ ПАЙПЛАЙНОМ. HDMI і джек — дві
// окремі звукові карти; якби вони сиділи в одному пайплайні за tee, карта,
// що не відкрилась (монітор без звуку, вийнятий кабель), зупинила б і
// другу. Окремо кожен падає й піднімається сам. Свій udpsrc у кожного —
// це те саме, що в рекордерів: камера шле бродкастом, і ядро віддає копію
// кожному сокету на порту.
//
// Розсинхрон HDMI проти джека не лікується навмисно: слухають щось одне.
//
// Свій потік: будує пайплайн, слухає шину, при помилці розбирає й піднімає
// знову з наростаючою паузою. Ззовні лише start/stop і перемикання порту
// (ліцензійний саботаж, як у решти каналів).

#include <cstdint>
#include <memory>
#include <string>

namespace vrx::audio {

struct AudioOutStats {
    uint64_t buffers = 0;     // скільки буферів дійшло до звукової карти
    uint64_t restarts = 0;    // скільки разів пайплайн перебудовувався
    bool playing = false;     // пайплайн зараз у PLAYING
};

class AudioOut {
public:
    struct Config {
        std::string name = "hdmi";                          // для логу
        std::string device = "plughw:CARD=rockchiphdmi0,DEV=0";
        int udp_port = 5601;
        int payload_type = 98;

        // Буфер пересортування. Для слухання затримка важить, тож малий:
        // мережа — один комутатор, перестановок майже не буває.
        int jitter_ms = 60;

        // ПЕРЕМИКАЧ МІКШЕРА, ЯКИЙ ТРЕБА УВІМКНУТИ ПЕРЕД ВИВОДОМ.
        // На ES8388 вихід "Headphone" (джек) після старту системи вимкнений,
        // і без цього в навушниках була б тиша при живому пайплайні.
        // Порожньо — нічого не чіпаємо.
        std::string unmute_card;      // ім'я карти ALSA, напр. "rockchipes8388"
        std::string unmute_control;   // напр. "Headphone"
    };

    explicit AudioOut(Config cfg);
    ~AudioOut();

    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    bool start();
    void stop();

    // Перекриття вхідного порту на ходу (ліцензійний саботаж). Пайплайн
    // перебудовується на новому порту.
    void set_udp_port(int port);

    AudioOutStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::audio
