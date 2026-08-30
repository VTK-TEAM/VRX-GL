#pragma once

// ДЖЕРЕЛО КАДРІВ ІЗ ЗАПИСУ — те саме, що живе, лише читає з файлу.
//
// Реалізує FrameSource, тобто рендерер не відрізняє його від ефіру: ті
// самі вікна, той самий acquire(), ті самі dmabuf без копій.
//
// ЧАС НАЛЕЖИТЬ СЕАНСУ, А НЕ ДЖЕРЕЛУ.
//
// Джерело не вирішує, коли який кадр показати: йому кажуть "зараз у
// сеансі ось така секунда", а воно віддає кадр, який на цю секунду
// припадає. Швидкість, пауза й покадровий крок живуть на рівень вище, у
// PlayerSession, — бо вони спільні для всіх каналів.
//
// Перша спроба відмірювала темп КАДРАМИ: один кадр за кожен забір
// рендерера. Виглядало охайно й було неправильним — "один кадр за забір"
// дорівнює одинарній швидкості лише коли частота джерела збігається з
// частотою показу. Основний канал 60 к/с, PiP і захват 25 — і за секунду
// показу вони розходились у два з половиною рази (заміряно: 1.03 с проти
// 2.44 с).
//
// Просити GStreamer грати зі швидкістю через seek із rate тут теж не
// годиться: на потоці, який ми самі подаємо в appsrc, це додає стан, про
// який довелося б думати на кожній перемотці.

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

    // ЦІЛЬОВА СЕКУНДА СЕАНСУ. Джерело віддає найсвіжіший кадр, що не
    // пізніший за неї. Ставить PlayerSession — він володіє часом.
    void set_target(int64_t session_us);

    // Час НАСТУПНОГО наявного кадру після заданого — для покадрового
    // кроку. 0, якщо такого поки немає.
    int64_t next_after(int64_t session_us) const;

    // Де ми зараз, від початку сеансу.
    int64_t position_us() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

} // namespace vrx::source
