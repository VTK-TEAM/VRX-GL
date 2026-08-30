#pragma once

// СЕАНС У ПЛЕЄРІ — три канали, один час на всіх.
//
// Кожен канал має власний файл, власний якір і власні розриви, але глядач
// бачить ОДИН запис. Тому позиція, швидкість і перемотка тут спільні, а
// джерела під ними просто виконують.
//
// Вирівнювання тримається на якорях із журналу: кожне джерело стає на ту
// саму секунду СЕАНСУ, а не файлу, — і різниця в моментах відкриття
// файлів (заміряно 0/5/68 мс) зникає сама.

#include "record/session_index.hpp"
#include "source/playback_source.hpp"

#include <memory>
#include <string>

namespace vrx::source {

class PlayerSession {
public:
    static constexpr int kChannels = 3;      // основний, PiP, захват

    bool open(const std::string& journal_path, int64_t t_us = 0);
    void close();

    // Джерело каналу для рендерера. Може бути без кадрів — це провал у
    // записі, а не помилка.
    FrameSource* channel(int i);
    const char* channel_name(int i) const;

    // ГОДИННИК СЕАНСУ. Кличеться раз на кадр показу, звідти й міра часу:
    // позиція посувається на стільки, скільки минуло, помножене на
    // швидкість. Пауза — це просто нульова швидкість, окремого стану під
    // неї немає.
    void tick();

    void seek(int64_t t_us);
    void set_speed(double s);         // 0 = пауза, далі 0.2..10
    double speed() const { return speed_; }

    // Крок на ОДИН КАДР. Береться час наступного наявного кадру основного
    // каналу, а не умовна шістдесята частка: у записі бувають пропуски, і
    // крок мусить потрапляти на справжній кадр, а не між ними.
    void step();

    int64_t position_us() const { return position_us_; }
    int64_t length_us() const { return ix_.length_us(); }
    const record::SessionIndex& index() const { return ix_; }
    bool live() const { return ix_.live(); }

private:
    record::SessionIndex ix_;
    std::unique_ptr<PlaybackSource> ch_[kChannels];
    bool open_ = false;

    int64_t position_us_ = 0;
    double speed_ = 1.0;
    int64_t last_tick_ns_ = 0;

    void push_target();
};

} // namespace vrx::source
