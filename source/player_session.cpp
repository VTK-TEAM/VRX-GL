#include "source/player_session.hpp"

#include <ctime>

namespace vrx::source {
namespace {
const char* kNames[PlayerSession::kChannels] = {"main", "pip", "capture"};
const PlaybackSource::Codec kCodecs[PlayerSession::kChannels] = {
    PlaybackSource::Codec::H265,     // основний — з борту
    PlaybackSource::Codec::MJPEG,    // PiP
    PlaybackSource::Codec::MJPEG,    // захват із камери
};
}

PlayerSession::PlayerSession() {
    for (int i = 0; i < kChannels; ++i) {
        PlaybackSource::Config c;
        c.channel = kNames[i];
        c.codec = kCodecs[i];
        ch_[i] = std::make_shared<PlaybackSource>(c);
    }
}

std::shared_ptr<FrameSource> PlayerSession::source(int i) const {
    return (i >= 0 && i < kChannels) ? ch_[i] : nullptr;
}

bool PlayerSession::open(const std::string& journal_path, int64_t t_us) {
    for (auto& c : ch_) c->stop();
    if (!ix_.load(journal_path)) return false;

    // Невдача каналу — НЕ привід кидати сеанс: канал міг узагалі не
    // писатись (не було сигналу, вимкнений захват), і це нормальний запис,
    // просто з двома доріжками замість трьох. Джерело лишається на місці й
    // просто мовчить.
    for (int i = 0; i < kChannels; ++i)
        ch_[i]->open(ix_, t_us);

    open_ = true;
    position_us_ = t_us;
    last_tick_ns_ = 0;
    push_target();
    return true;
}

void PlayerSession::close() {
    for (auto& c : ch_) c->stop();     // джерела лишаються, кадри зникають
    open_ = false;
}

FrameSource* PlayerSession::channel(int i) {
    if (i < 0 || i >= kChannels) return nullptr;
    return ch_[i].get();
}

const char* PlayerSession::channel_name(int i) const {
    return (i >= 0 && i < kChannels) ? kNames[i] : "";
}

void PlayerSession::push_target() {
    for (auto& c : ch_) if (c) c->set_target(position_us_);
}

void PlayerSession::tick() {
    if (!open_) return;          // закритий плеєр часу не рахує

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now = ts.tv_sec * 1000000000LL + ts.tv_nsec;

    if (last_tick_ns_ != 0 && speed_ > 0.0) {
        // Понад чверть секунди між тактами — це не сповільнений показ, а
        // зупинка: перемикання екрана, підняття пайплайна, затик носія.
        // Зараховувати такий провал у позицію означало б перескочити
        // шматок запису, якого глядач не бачив.
        int64_t dt = now - last_tick_ns_;
        if (dt > 250000000LL) dt = 0;
        position_us_ += (int64_t)(dt / 1000.0 * speed_);
        const int64_t len = ix_.length_us();
        if (len > 0 && position_us_ > len) position_us_ = len;
    }
    last_tick_ns_ = now;
    push_target();
}

void PlayerSession::seek(int64_t t_us) {
    const int64_t len = ix_.length_us();
    position_us_ = t_us < 0 ? 0 : (len > 0 && t_us > len ? len : t_us);
    last_tick_ns_ = 0;

    // Усі канали на ту саму секунду СЕАНСУ. Кожен сам знайде свій файл і
    // свій байтовий зсув — у них різні ротації й різні розриви.
    for (auto& c : ch_) if (c) c->seek(position_us_);
    push_target();
}

void PlayerSession::set_speed(double s) {
    speed_ = s < 0 ? 0 : (s > 10.0 ? 10.0 : s);
}

void PlayerSession::step() {
    // Питаємо основний канал, коли в нього НАСТУПНИЙ кадр. Якщо його
    // немає (провал або черга ще порожня) — пробуємо решту, і лише потім
    // відступаємо до умовного кроку.
    int64_t next = 0;
    for (int i = 0; i < kChannels && next == 0; ++i)
        if (ch_[i]) next = ch_[i]->next_after(position_us_);

    position_us_ = next > 0 ? next : position_us_ + 16667;
    push_target();
}

} // namespace vrx::source
