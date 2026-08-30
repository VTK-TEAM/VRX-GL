#pragma once

// ДЖЕРЕЛО КАДРІВ ІЗ ЗАПИСУ — те саме, що живе, лише читає з файлу.
//
// Реалізує FrameSource, тобто рендерер не відрізняє його від ефіру: ті
// самі вікна, той самий acquire(), ті самі dmabuf без копій.
//
// ТЕМП ЗАДАЄТЬСЯ СПОЖИВАННЯМ, а не годинником GStreamer.
//
// Рендерер і так забирає кадр рівно раз на розгортку. Тому "швидкість" —
// це просто скільки кадрів віддати за один забір: 1.0 — по одному, 2.0 —
// через один, 0.5 — той самий двічі. Звідси безкоштовно виходить і пауза
// (0.0), і покадровий крок, і весь діапазон 0.2–10x.
//
// Альтернатива — просити GStreamer грати зі швидкістю через seek із rate —
// на потоці, який ми самі подаємо в appsrc, працює погано й додає стан,
// про який довелося б думати на кожній перемотці.

#include "source/frame_source.hpp"
#include "record/session_index.hpp"

#include <memory>
#include <string>

namespace vrx::source {

class PlaybackSource : public FrameSource {
public:
    enum class Codec { H265, MJPEG };

    struct Config {
        std::string channel = "main";   // який канал сеансу відтворюємо
        Codec codec = Codec::H265;

        // Скільки кадрів тримати напоготів. Черга потрібна не для згладжування
        // — декодер швидший за показ у двадцять разів, — а щоб подавач не
        // блокувався на кожному кадрі.
        int queue_depth = 8;
    };

    PlaybackSource(Config cfg);
    ~PlaybackSource() override;

    const char* name() const override { return name_.c_str(); }
    bool start() override;
    void stop() override;
    bool acquire(SourceFrame& out) override;
    bool has_signal() const override;
    SourceStats stats() const override;

    // Відкриває сеанс і стає на позицію t від його початку.
    bool open(const record::SessionIndex& ix, int64_t t_us);

    // Перемотка. Дешева: пайплайн піднімається наново, а декодер швидший
    // за реальний час у двадцять сім разів, тож коштує це частки секунди.
    bool seek(int64_t t_us);

    // 0 = пауза. Один крок уперед на паузі — step().
    void set_speed(double s);
    void step();

    // Де ми зараз, від початку сеансу.
    int64_t position_us() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

} // namespace vrx::source
