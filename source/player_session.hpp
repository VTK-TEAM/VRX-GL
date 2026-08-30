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
#include "osd/telemetry_reader.hpp"
#include "osd/telemetry/vt_telemetry_storage.h"
#include "source/playback_source.hpp"

#include <atomic>
#include <mutex>
#include <memory>
#include <string>

namespace vrx::source {

class PlayerSession {
public:
    static constexpr int kChannels = 3;      // основний, PiP, захват

    PlayerSession();

    bool open(const std::string& journal_path, int64_t t_us = 0);
    void close();

    // ДЖЕРЕЛА ІСНУЮТЬ ЗАВЖДИ, ще до відкриття сеансу.
    //
    // Інакше їх довелося б реєструвати в рендерері в момент натискання
    // кнопки — тобто дописувати у список, який щокадру читає потік показу.
    // Порожнє джерело просто не віддає кадрів, і рендерер його пропускає.
    std::shared_ptr<FrameSource> source(int i) const;

    // Джерело каналу для рендерера. Може бути без кадрів — це провал у
    // записі, а не помилка.
    FrameSource* channel(int i);
    const char* channel_name(int i) const;

    // НАМІРИ ВІД МИШІ.
    //
    // Перемотка перезбирає до трьох пайплайнів GStreamer — заміряно 34..74
    // мс, при розгортці 17 мс. Виконувати це прямо з обробки миші означало
    // б щоразу губити три-чотири кадри, а перетяг повзунка перетворювати на
    // низку пропусків.
    //
    // Тому назовні лишається лише намір, а виконує його потік годинника.
    // Побічно це прибирає й гонку за індексом сеансу: живий журнал
    // перечитує той самий потік, і більше ніхто до ix_ не торкається.
    void request_open(const std::string& journal_path);
    void request_seek(int64_t t_us);
    void request_jump(int64_t delta_us);
    void request_step(int dir);

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
    // Крок на один кадр: +1 уперед, -1 назад.
    //
    // Назад дорожче за вперед — треба перезібрати подачу від найближчої
    // мітки й дочитати до потрібного кадру, — але декодер швидший за
    // реальний час у двадцять сім разів, тож це частки секунди.
    void step(int dir = 1);

    // Стрибок на задану кількість мікросекунд від поточного місця.
    void jump(int64_t delta_us);

    int64_t position_us() const { return position_us_; }

    // Довжина й початок читаються з ПОТОКУ ПОКАЗУ, а журнал живого сеансу
    // перечитує годинник — тому атомарні знімки, а не посилання на індекс.
    int64_t length_us() const { return length_us_.load(std::memory_order_relaxed); }

    // ДОВЖИНА ДЛЯ ТАЙМЛАЙНУ. Для закритого сеансу це просто його довжина.
    //
    // Для живого — час від початку запису ДО ЗАРАЗ, а не до останньої
    // мітки журналу. Мітки лягають на носій пачками по кілька секунд, і
    // кінець, рахований по них, ріс стрибками — разом із ним смикалась і
    // позиція, що в нього впиралась. Запис же йде рівно, у реальному часі,
    // тож його край і є "зараз".
    int64_t timeline_len_us() const;
    int64_t start_wall_us() const { return start_wall_us_.load(std::memory_order_relaxed); }
    bool live() const { return live_.load(std::memory_order_relaxed); }

    // ЗАПИСАНА ТЕЛЕМЕТРІЯ цього сеансу. Віддається OSD замість ефірної, і
    // той малює з неї звичним шляхом. nullptr, якщо логу немає — тоді на
    // екрані просто не буде телеметрії, а решта працює.
    VtTelemetryStorage* telemetry() { return tlm_ok_ ? &tlm_store_ : nullptr; }

    // Знімок УСІХ каналів у теку. Кодування йде на власному потоці: воно
    // триває десятки мілісекунд на кадр, а показ чекати не може.
    //
    // Повертає кількість каналів, які мали що знімати.
    int save_snapshots(const std::string& dir) const;

private:
    record::SessionIndex ix_;
    std::shared_ptr<PlaybackSource> ch_[kChannels];
    bool open_ = false;

    std::string journal_;
    std::atomic<int64_t> length_us_{0};
    std::atomic<int64_t> start_wall_us_{0};
    std::atomic<bool> live_{false};

    int64_t position_us_ = 0;
    double speed_ = 1.0;
    int64_t last_tick_ns_ = 0;
    int64_t last_reload_ns_ = 0;

    std::mutex req_mtx_;
    std::string req_open_;
    bool have_open_ = false;
    int64_t req_seek_ = 0;
    bool have_seek_ = false;
    int64_t req_jump_ = 0;
    int req_step_ = 0;

    osd::TelemetryReader tlm_;
    VtTelemetryStorage tlm_store_;
    bool tlm_ok_ = false;

    void push_target();
    void process_requests();        // виконати накопичені наміри
    void adopt();                 // перенести знімки з ix_ у атомарні поля
};

} // namespace vrx::source
