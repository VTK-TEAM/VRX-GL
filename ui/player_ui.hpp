#pragma once

// ТАЙМЛАЙН ПЛЕЄРА — смуга внизу екрана, який зараз у режимі плеєра.
//
// Малюється ЛИШЕ на своїй ролі й лише коли ця роль у плеєрі. Другий екран
// про нього не знає: у нього або ефір, або свій плеєр зі своєю смугою.
//
// Свій шар, а не частина ScreenPresets, навмисно: пресети відповідають за
// вікна й розкладку, і домішувати туди керування відтворенням означало б
// зробити найскладніший файл проєкту ще складнішим.

#include "render/overlay.hpp"
#include "source/player_session.hpp"
#include "ui/pointer.hpp"
#include "ui/screen_presets.hpp"

#include <memory>

namespace vrx::ui {

class PlayerUi : public render::Overlay {
public:
    struct Config {
        float bar_h = 0.020f;      // висота смуги, частка висоти екрана
        float side = 0.04f;        // відступ від бічних країв
        float bottom = 0.0f;       // впритул до низу екрана
    };

    explicit PlayerUi(Config cfg);
    ~PlayerUi() override;

    void attach(Pointer* p);
    void attach_presets(ScreenPresets* sp);
    void attach_player(int role, std::shared_ptr<source::PlayerSession> s);

    // Чи влучає точка в смугу. Потрібне пресетам: без цього перетяг
    // повзунка тягнув би заразом і вікно під ним.
    bool hit_bar(int role, float cx, float cy) const;

    const char* name() const override { return "плеєр"; }
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
