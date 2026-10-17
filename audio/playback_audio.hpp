#pragma once

// ЗВУК ПЛЕЄРА: веде аудіовиходи за записаним сеансом.
//
// Плеєр показує відео, підтягуючи кадри до "цільового часу сеансу" (див.
// PlaybackSource), а звук безперервний — його не тягнуть по кадру, а
// ставлять на позицію й дають грати. Тож тут не декодер, а ДИСПЕТЧЕР: раз
// на poll_ms дивиться, що зараз у плеєрі, і каже виходам, що грати.
//
//   екран не в плеєрі / сеанс не відкрито -> виходи на ефір;
//   швидкість 1x                          -> .mka з позиції сеансу;
//   пауза або інша швидкість               -> мовчати на місці (звук
//                                            розійшовся б із картинкою);
//   у цю мить звуку в записі немає          -> тиша.
//
// Позицію у файлі рахуємо з ЯКОРЯ журналу: настінний час, якому відповідає
// PTS=0 файлу. Момент t сеансу — це start_us + t; у файлі це start_us + t -
// anchor. Ті самі якорі ставлять поруч і відео, тож звук ляже туди ж.
//
// Розбіжність з плеєром більше за max_drift_us — підсуваємо. Годинник
// звукової карти й годинник плеєра різні, і без цього за годину звук
// міг би втекти; підсування раз на кілька хвилин на слух непомітне.
//
// Метр рівня сюди не входить: він лишається на живому звуці, тож тривога
// працює й під час перегляду запису.

#include "audio/audio_out.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vrx::audio {

// Що зараз у плеєрі, з погляду звуку.
struct PlaybackTarget {
    bool active = false;          // якийсь екран у плеєрі з відкритим сеансом
    std::string journal;          // журнал сеансу
    std::string own_session;      // мітка поточного вмикання (для live())
    int64_t pos_us = 0;           // позиція від початку сеансу
    double speed = 0.0;           // 0 = пауза
};

class PlaybackAudio {
public:
    struct Config {
        int poll_ms = 100;
        int64_t max_drift_us = 150000;
        int reload_ms = 3000;          // перечитувати журнал сеансу, що ще пишеться
        int seek_cooldown_ms = 600;    // після підсування дати виходу стати на місце
        std::string channel = "audio"; // канал звуку в журналі
    };

    PlaybackAudio(Config cfg, std::vector<AudioOut*> outs,
                  std::function<PlaybackTarget()> target);
    ~PlaybackAudio();

    PlaybackAudio(const PlaybackAudio&) = delete;
    PlaybackAudio& operator=(const PlaybackAudio&) = delete;

    bool start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::audio
