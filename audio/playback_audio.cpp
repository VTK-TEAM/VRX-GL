#include "audio/playback_audio.hpp"

#include "record/session_index.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace vrx::audio {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct PlaybackAudio::Impl {
    Config cfg;
    std::vector<AudioOut*> outs;
    std::function<PlaybackTarget()> target;

    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{true};

    enum class St { Live, File, Silent };
    St st = St::Live;
    std::string cur_path;
    std::string cur_journal;
    record::SessionIndex ix;
    bool ix_ok = false;
    int64_t ix_loaded_ms = 0;
    std::vector<int64_t> seek_at;      // коли кожному виходу востаннє підсували

    Impl(Config c, std::vector<AudioOut*> o, std::function<PlaybackTarget()> t)
        : cfg(std::move(c)), outs(std::move(o)), target(std::move(t)), seek_at(outs.size(), 0) {}

    void all_live() {
        if (st == St::Live) return;
        for (AudioOut* o : outs) o->go_live();
        st = St::Live; cur_path.clear(); cur_journal.clear(); ix_ok = false;
        std::fprintf(stderr, "[звук плеєра] назад на ефір\n");
    }

    void all_silent() {
        if (st == St::Silent) return;
        for (AudioOut* o : outs) o->go_silent();
        st = St::Silent; cur_path.clear();
    }

    void step() {
        if (!enabled.load(std::memory_order_relaxed)) { all_silent(); return; }
        const PlaybackTarget t = target ? target() : PlaybackTarget{};
        if (!t.active) { all_live(); return; }

        const int64_t now = now_ms();
        if (t.journal != cur_journal ||
            (ix_ok && ix.live() && now - ix_loaded_ms > cfg.reload_ms)) {
            ix_ok = ix.load(t.journal, t.own_session);
            ix_loaded_ms = now;
            if (t.journal != cur_journal) {
                cur_journal = t.journal;
                int n = 0;
                if (ix_ok) for (const auto& f : ix.files()) if (f.channel == cfg.channel) ++n;
                std::fprintf(stderr, "[звук плеєра] сеанс %s: файлів звуку %d\n",
                             ix_ok ? ix.id().c_str() : "?", n);
            }
        }
        if (!ix_ok) { all_silent(); return; }

        // Який файл звуку покриває цю мить і де в ньому стати.
        const int64_t want = ix.start_us() + t.pos_us;
        const record::IndexFile* f = nullptr;
        for (const auto& x : ix.files()) {
            if (x.channel != cfg.channel) continue;
            if (want >= x.anchor_us && want <= x.end_us) { f = &x; break; }
        }
        if (!f) { all_silent(); return; }       // у записі тут звуку немає

        const std::string path = ix.dir() + "/" + f->name;
        const int64_t rel = want - f->anchor_us;
        const bool playing = std::fabs(t.speed - 1.0) < 1e-3;

        if (st != St::File || path != cur_path) {
            for (size_t i = 0; i < outs.size(); ++i) {
                outs[i]->play_file(path, rel, !playing);
                seek_at[i] = now;
            }
            st = St::File; cur_path = path;
            std::fprintf(stderr, "[звук плеєра] %s з %.1f с%s\n", f->name.c_str(),
                         rel / 1e6, playing ? "" : " (пауза)");
            return;
        }

        for (size_t i = 0; i < outs.size(); ++i) {
            AudioOut* o = outs[i];
            o->pause_file(!playing);
            if (!playing) continue;
            if (now - seek_at[i] < cfg.seek_cooldown_ms) continue;
            const int64_t p = o->file_position_us();
            if (p >= 0 && std::llabs(p - rel) > cfg.max_drift_us) {
                o->seek_file(rel);
                seek_at[i] = now;
            }
        }
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.poll_ms));
        }
        all_live();     // виходимо — виходи на ефір, як і було
    }
};

PlaybackAudio::PlaybackAudio(Config cfg, std::vector<AudioOut*> outs,
                             std::function<PlaybackTarget()> target)
    : impl_(new Impl(std::move(cfg), std::move(outs), std::move(target))) {}

PlaybackAudio::~PlaybackAudio() { stop(); }

bool PlaybackAudio::start() {
    if (impl_->running.load()) return true;
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    return true;
}

void PlaybackAudio::set_enabled(bool on) {
    impl_->enabled.store(on, std::memory_order_relaxed);
}

void PlaybackAudio::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace vrx::audio
