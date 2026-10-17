#include "audio/audio_out.hpp"

#include <alsa/asoundlib.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace vrx::audio {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Увімкнути перемикач мікшера (Headphone на ES8388).
//
// Через libasound, а не `amixer`: бібліотека на станції є завжди (її тягне
// SDL), а alsa-utils у мінімальному образі може й не бути. Немає такого
// перемикача — вважаємо, що вмикати нічого, і це не помилка.
bool unmute(const std::string& card, const std::string& control) {
    if (card.empty() || control.empty()) return true;
    snd_mixer_t* mx = nullptr;
    if (snd_mixer_open(&mx, 0) < 0) return false;

    bool ok = false;
    const std::string hw = "hw:CARD=" + card;
    if (snd_mixer_attach(mx, hw.c_str()) == 0 &&
        snd_mixer_selem_register(mx, nullptr, nullptr) == 0 &&
        snd_mixer_load(mx) == 0) {
        snd_mixer_selem_id_t* sid = nullptr;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, control.c_str());
        if (snd_mixer_elem_t* el = snd_mixer_find_selem(mx, sid)) {
            ok = !snd_mixer_selem_has_playback_switch(el) ||
                 snd_mixer_selem_set_playback_switch_all(el, 1) == 0;
        }
    }
    snd_mixer_close(mx);
    return ok;
}

} // namespace

struct AudioOut::Impl {
    Config cfg;

    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<int> port_override{-1};
    std::atomic<bool> want_rebuild{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    std::atomic<uint64_t> buffers{0};
    std::atomic<uint64_t> restarts{0};
    std::atomic<bool> playing{false};
    uint64_t failures = 0;

    explicit Impl(Config c) : cfg(std::move(c)) {}

    int eff_port() const {
        const int p = port_override.load(std::memory_order_relaxed);
        return p < 0 ? cfg.udp_port : p;
    }

    static GstPadProbeReturn on_buffer(GstPad*, GstPadProbeInfo*, gpointer self) {
        static_cast<Impl*>(self)->buffers.fetch_add(1, std::memory_order_relaxed);
        return GST_PAD_PROBE_OK;
    }

    // ПАЙПЛАЙН. Розбір рядка рівно за інтерфейсом камери. plc=true —
    // загублений кадр Opus декодер домальовує сам, замість клацання тишею.
    // Каналів на виході два: камера моно, а HDMI-приймачі моно часто не
    // беруть; audioconvert просто дублює.
    GstElement* build() {
        char desc[1024];
        std::snprintf(desc, sizeof(desc),
            "udpsrc port=%d caps=\"application/x-rtp,media=audio,clock-rate=48000,"
            "encoding-name=OPUS,payload=%d\" "
            "! rtpjitterbuffer latency=%d drop-on-latency=true "
            "! rtpopusdepay ! opusdec plc=true "
            "! audioconvert ! audioresample "
            "! audio/x-raw,rate=48000,channels=2 "
            "! alsasink name=sink device=\"%s\"",
            eff_port(), cfg.payload_type, cfg.jitter_ms, cfg.device.c_str());

        GError* err = nullptr;
        GstElement* p = gst_parse_launch(desc, &err);
        if (!p) {
            std::fprintf(stderr, "[звук %s] пайплайн не зібрався: %s\n",
                         cfg.name.c_str(), err ? err->message : "?");
            if (err) g_error_free(err);
            return nullptr;
        }
        if (err) g_error_free(err);

        if (GstElement* sink = gst_bin_get_by_name(GST_BIN(p), "sink")) {
            if (GstPad* pad = gst_element_get_static_pad(sink, "sink")) {
                gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_buffer, this, nullptr);
                gst_object_unref(pad);
            }
            gst_object_unref(sink);
        }
        return p;
    }

    // Пауза, яку перериває stop() і зміна порту.
    void nap(int ms) {
        std::unique_lock<std::mutex> lk(wake_mtx);
        wake_cv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
            return !running.load(std::memory_order_relaxed) ||
                   want_rebuild.load(std::memory_order_relaxed);
        });
    }

    // Не засипати журнал: перші кілька збоїв повністю, далі кожен 30-й.
    // HDMI-монітор без звуку відмовлятиме щоразу, і це нормальний стан.
    bool should_log() { ++failures; return failures <= 3 || failures % 30 == 0; }

    void loop() {
        if (!unmute(cfg.unmute_card, cfg.unmute_control)) {
            std::fprintf(stderr, "[звук %s] не вдалось увімкнути %s на %s — у виході може бути тиша\n",
                         cfg.name.c_str(), cfg.unmute_control.c_str(), cfg.unmute_card.c_str());
        }

        int backoff_ms = 500;
        bool first = true;
        while (running.load(std::memory_order_relaxed)) {
            want_rebuild.store(false, std::memory_order_relaxed);
            GstElement* p = build();

            if (p && gst_element_set_state(p, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
                if (!first) restarts.fetch_add(1, std::memory_order_relaxed);
                first = false;
                const int64_t t0 = now_ms();
                playing.store(true, std::memory_order_relaxed);
                std::fprintf(stderr, "[звук %s] слухаю UDP %d -> %s\n",
                             cfg.name.c_str(), eff_port(), cfg.device.c_str());

                GstBus* bus = gst_element_get_bus(p);
                while (running.load(std::memory_order_relaxed) &&
                       !want_rebuild.load(std::memory_order_relaxed)) {
                    GstMessage* m = gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND,
                        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                    if (!m) continue;
                    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR && should_log()) {
                        GError* e = nullptr;
                        gchar* dbg = nullptr;
                        gst_message_parse_error(m, &e, &dbg);
                        std::fprintf(stderr, "[звук %s] помилка (%llu-а): %s\n",
                                     cfg.name.c_str(), (unsigned long long)failures,
                                     e ? e->message : "?");
                        if (e) g_error_free(e);
                        g_free(dbg);
                    }
                    gst_message_unref(m);
                    break;
                }
                gst_object_unref(bus);
                playing.store(false, std::memory_order_relaxed);

                // Прожив довго — збій разовий, наступна пауза з початку.
                if (now_ms() - t0 > 10000) backoff_ms = 500;
            } else if (p && should_log()) {
                std::fprintf(stderr, "[звук %s] не стартував на %s\n",
                             cfg.name.c_str(), cfg.device.c_str());
            }

            if (p) {
                gst_element_set_state(p, GST_STATE_NULL);
                gst_object_unref(p);
            }
            if (!running.load(std::memory_order_relaxed)) break;
            if (want_rebuild.load(std::memory_order_relaxed)) continue;   // зміна порту — одразу
            nap(backoff_ms);
            backoff_ms = std::min(backoff_ms * 2, 8000);
        }
    }
};

AudioOut::AudioOut(Config cfg) : impl_(new Impl(std::move(cfg))) {}

AudioOut::~AudioOut() { stop(); }

bool AudioOut::start() {
    if (impl_->running.load()) return true;
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    return true;
}

void AudioOut::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->wake_cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
}

void AudioOut::set_udp_port(int port) {
    impl_->port_override.store(port, std::memory_order_relaxed);
    impl_->want_rebuild.store(true, std::memory_order_relaxed);
    impl_->wake_cv.notify_all();
}

AudioOutStats AudioOut::stats() const {
    AudioOutStats s;
    s.buffers = impl_->buffers.load(std::memory_order_relaxed);
    s.restarts = impl_->restarts.load(std::memory_order_relaxed);
    s.playing = impl_->playing.load(std::memory_order_relaxed);
    return s;
}

} // namespace vrx::audio
