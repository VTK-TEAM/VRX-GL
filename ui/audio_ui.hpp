#pragma once

// ШАР ЗВУКУ: панель гучності під кнопкою-динаміком і сам аудіометр.
//
// Окремим шаром, як і таймлайн плеєра: пресети відповідають за вікна, і
// домішувати туди повзунки й шкали означало б розростати найскладніший
// файл проєкту. Розподіл такий:
//   - ScreenPresets — кнопка в ряду, вікно "meter" (де стоїть, чи показане,
//     збереження в screenN.json) і перетяг/масштаб мишею, як у відео;
//   - AudioUi — панель (перемикач метра + повзунок), малювання рівня в
//     прямокутнику метра, гучність і її збереження (audio.json).
//
// Миша спільна, тож панель звуку оголошує свою область зайнятою
// (hit() -> ScreenPresets::set_blocked_area): тягнучи повзунок, не тягнеш
// заразом відео під ним.

#include "../audio/audio_out.hpp"
#include "../render/overlay.hpp"
#include "pointer.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vrx::ui {

class ScreenPresets;

class AudioUi : public render::Overlay {
public:
    struct Config {
        std::string file = "audio.json";   // де живе гучність
        float panel_buttons = 6.0f;        // ширина панелі, у кнопках ряду
        float db_floor = -60.0f;           // низ шкали метра, дБ
        float db_yellow = -18.0f;          // з цього рівня — жовте
        float db_red = -6.0f;              // з цього — червоне
    };

    explicit AudioUi(Config cfg);
    ~AudioUi() override;

    AudioUi(const AudioUi&) = delete;
    AudioUi& operator=(const AudioUi&) = delete;

    void attach(Pointer* p);
    void attach_presets(ScreenPresets* sp);

    // Звідки брати рівень (метр) і куди віддавати гучність. Гучність —
    // ЛІНІЙНА (0..1); повзунок сам переводить своє положення у квадрат,
    // щоб половина ходу звучала як "тихіше вдвічі", а не як ледь чутно.
    void set_level(std::function<audio::AudioLevel()> cb);
    void set_volume(std::function<void(float linear)> cb);

    // Чи це місце зайняте панеллю звуку (для заблокованої області пресетів).
    bool hit(int role, float cx, float cy) const;

    const char* name() const override { return "звук"; }
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
