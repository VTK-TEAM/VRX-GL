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

enum class Src { Live, File, Silent };

const char* src_name(Src s) {
    return s == Src::Live ? "ефір" : (s == Src::File ? "запис" : "тиша");
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

    // Гучність і елемент, яким її міняти на ходу. Елемент живе стільки,
    // скільки пайплайн; захищений, бо гучність крутять з потоку показу.
    std::atomic<float> volume{1.0f};
    std::mutex vol_mtx;
    GstElement* vol_el = nullptr;

    // Останній рівень (режим метра). Атомарні — читає потік показу.
    std::atomic<float> rms_db{-100.f}, peak_db{-100.f};
    std::atomic<int64_t> level_at_ms{-1};

    // ЗАПИТАНЕ ДЖЕРЕЛО. Пише будь-хто, виконує лише потік виходу.
    // Зміна режиму чи файлу — перебудова пайплайна; позиція й пауза —
    // на живому, без перебудови.
    std::mutex want_mtx;
    Src want_src = Src::Live;
    std::string want_path;
    int64_t want_pos_us = 0;
    uint64_t want_seek_gen = 0;
    bool want_paused = false;

    std::atomic<int64_t> file_pos_us{-1};

    explicit Impl(Config c) : cfg(std::move(c)) {}

    int eff_port() const {
        const int p = port_override.load(std::memory_order_relaxed);
        return p < 0 ? cfg.udp_port : p;
    }

    void wake() { wake_cv.notify_all(); }

    static GstPadProbeReturn on_buffer(GstPad*, GstPadProbeInfo*, gpointer self) {
        static_cast<Impl*>(self)->buffers.fetch_add(1, std::memory_order_relaxed);
        return GST_PAD_PROBE_OK;
    }

    // Хвіст пайплайна: або звукова карта з гучністю, або метр.
    std::string tail() const {
        char b[512];
        if (cfg.meter) {
            std::snprintf(b, sizeof(b),
                "! level name=lvl interval=%llu post-messages=true "
                "! fakesink name=sink sync=false async=false",
                (unsigned long long)cfg.meter_interval_ms * 1000000ULL);
        } else {
            std::snprintf(b, sizeof(b),
                "! audio/x-raw,rate=48000,channels=2 "
                "! volume name=vol volume=%.4f "
                "! alsasink name=sink device=\"%s\"",
                (double)volume.load(std::memory_order_relaxed), cfg.device.c_str());
        }
        return b;
    }

    // Розібрати повідомлення level: найгучніший канал.
    void on_level(const GstStructure* s) {
        float rms = -100.f, peak = -100.f;
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (const GValue* v = gst_structure_get_value(s, "rms")) {
            if (GValueArray* a = (GValueArray*)g_value_get_boxed(v))
                for (guint i = 0; i < a->n_values; ++i)
                    rms = std::max(rms, (float)g_value_get_double(g_value_array_get_nth(a, i)));
        }
        if (const GValue* v = gst_structure_get_value(s, "peak")) {
            if (GValueArray* a = (GValueArray*)g_value_get_boxed(v))
                for (guint i = 0; i < a->n_values; ++i)
                    peak = std::max(peak, (float)g_value_get_double(g_value_array_get_nth(a, i)));
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
        rms_db.store(rms, std::memory_order_relaxed);
        peak_db.store(peak, std::memory_order_relaxed);
        level_at_ms.store(now_ms(), std::memory_order_relaxed);
    }

    // ПАЙПЛАЙН.
    //
    // Ефір — рівно за інтерфейсом камери: Opus/RTP, PT 98. plc=true —
    // загублений кадр декодер домальовує сам, замість клацання тишею.
    //
    // Запис — той самий .mka, що пише рекордер: Matroska з Opus без
    // перекодування. Демультиплексор сам точно стає на час, тож мітки
    // журналу тут не потрібні — лише якір файлу (див. PlaybackAudio).
    //
    // Каналів на виході два: камера моно, а HDMI-приймачі моно часто не
    // беруть; audioconvert просто дублює.
    GstElement* build(Src src, const std::string& path) {
        char desc[1536];
        if (src == Src::File) {
            std::snprintf(desc, sizeof(desc),
                "filesrc location=\"%s\" ! matroskademux ! opusparse ! opusdec plc=true "
                "! audioconvert ! audioresample %s",
                path.c_str(), tail().c_str());
        } else {
            std::snprintf(desc, sizeof(desc),
                "udpsrc port=%d caps=\"application/x-rtp,media=audio,clock-rate=48000,"
                "encoding-name=OPUS,payload=%d\" "
                "! rtpjitterbuffer latency=%d drop-on-latency=true "
                "! rtpopusdepay ! opusdec plc=true "
                "! audioconvert ! audioresample %s",
                eff_port(), cfg.payload_type, cfg.jitter_ms, tail().c_str());
        }

        GError* err = nullptr;
        GstElement* p = gst_parse_launch(desc, &err);
        if (!p) {
            std::fprintf(stderr, "[звук %s] пайплайн не зібрався: %s\n",
                         cfg.name.c_str(), err ? err->message : "?");
            if (err) g_error_free(err);
            return nullptr;
        }
        if (err) g_error_free(err);

        {
            std::lock_guard<std::mutex> lk(vol_mtx);
            vol_el = gst_bin_get_by_name(GST_BIN(p), "vol");   // nullptr у метра
        }
        if (GstElement* sink = gst_bin_get_by_name(GST_BIN(p), "sink")) {
            if (GstPad* pad = gst_element_get_static_pad(sink, "sink")) {
                gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_buffer, this, nullptr);
                gst_object_unref(pad);
            }
            gst_object_unref(sink);
        }
        return p;
    }

    void teardown(GstElement* p) {
        {
            std::lock_guard<std::mutex> lk(vol_mtx);
            if (vol_el) { gst_object_unref(vol_el); vol_el = nullptr; }
        }
        if (p) {
            gst_element_set_state(p, GST_STATE_NULL);
            gst_object_unref(p);
        }
        playing.store(false, std::memory_order_relaxed);
        file_pos_us.store(-1, std::memory_order_relaxed);
    }

    static bool file_seek(GstElement* p, int64_t pos_us) {
        if (pos_us < 0) pos_us = 0;
        return gst_element_seek_simple(p, GST_FORMAT_TIME,
            (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            (gint64)pos_us * 1000);
    }

    // Пауза, яку перериває stop() і будь-яка зміна запиту.
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
        Src last_src = Src::Live;
        while (running.load(std::memory_order_relaxed)) {
            want_rebuild.store(false, std::memory_order_relaxed);
            Src src; std::string path; int64_t pos; uint64_t gen; bool paused;
            {
                std::lock_guard<std::mutex> lk(want_mtx);
                src = want_src; path = want_path; pos = want_pos_us;
                gen = want_seek_gen; paused = want_paused;
            }
            if (src != last_src) {
                std::fprintf(stderr, "[звук %s] джерело: %s%s%s\n", cfg.name.c_str(), src_name(src),
                             src == Src::File ? " " : "", src == Src::File ? path.c_str() : "");
                last_src = src;
            }

            // ТИША: пайплайна немає, карту відпущено. Чекаємо нового запиту.
            if (src == Src::Silent) {
                while (running.load(std::memory_order_relaxed) &&
                       !want_rebuild.load(std::memory_order_relaxed))
                    nap(500);
                continue;
            }

            GstElement* p = build(src, path);
            bool started = false;
            if (p) {
                if (src == Src::File) {
                    // Стати на позицію в PAUSED, а вже тоді грати: інакше
                    // перші мілісекунди з початку файлу встигли б прозвучати.
                    if (gst_element_set_state(p, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE &&
                        gst_element_get_state(p, nullptr, nullptr, 2 * GST_SECOND) != GST_STATE_CHANGE_FAILURE) {
                        file_seek(p, pos);
                        started = paused || gst_element_set_state(p, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
                    }
                } else {
                    started = gst_element_set_state(p, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
                }
            }

            if (started) {
                if (!first) restarts.fetch_add(1, std::memory_order_relaxed);
                first = false;
                const int64_t t0 = now_ms();
                playing.store(true, std::memory_order_relaxed);
                if (src == Src::Live)
                    std::fprintf(stderr, "[звук %s] слухаю UDP %d -> %s\n",
                                 cfg.name.c_str(), eff_port(),
                                 cfg.meter ? "метр рівня" : cfg.device.c_str());

                uint64_t applied_gen = gen;
                bool applied_paused = paused;
                GstBus* bus = gst_element_get_bus(p);
                while (running.load(std::memory_order_relaxed) &&
                       !want_rebuild.load(std::memory_order_relaxed)) {
                    GstMessage* m = gst_bus_timed_pop_filtered(bus, 50 * GST_MSECOND,
                        (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_ELEMENT));
                    bool stop_here = false;
                    if (m) {
                        const GstMessageType t = GST_MESSAGE_TYPE(m);
                        if (t == GST_MESSAGE_ELEMENT) {
                            const GstStructure* s = gst_message_get_structure(m);
                            if (s && gst_structure_has_name(s, "level")) on_level(s);
                        } else if (t == GST_MESSAGE_EOS && src == Src::File) {
                            // Кінець файлу — не збій: мовчимо на місці, доки
                            // плеєр не попросить іншу позицію чи файл.
                        } else {
                            if (t == GST_MESSAGE_ERROR && should_log()) {
                                GError* e = nullptr;
                                gchar* dbg = nullptr;
                                gst_message_parse_error(m, &e, &dbg);
                                std::fprintf(stderr, "[звук %s] помилка (%llu-а): %s\n",
                                             cfg.name.c_str(), (unsigned long long)failures,
                                             e ? e->message : "?");
                                if (e) g_error_free(e);
                                g_free(dbg);
                            }
                            stop_here = true;
                        }
                        gst_message_unref(m);
                    }
                    if (stop_here) break;

                    if (src == Src::File) {
                        // Позиція й пауза — на живому пайплайні.
                        uint64_t g; int64_t ps; bool pa;
                        {
                            std::lock_guard<std::mutex> lk(want_mtx);
                            g = want_seek_gen; ps = want_pos_us; pa = want_paused;
                        }
                        if (g != applied_gen) { file_seek(p, ps); applied_gen = g; }
                        if (pa != applied_paused) {
                            gst_element_set_state(p, pa ? GST_STATE_PAUSED : GST_STATE_PLAYING);
                            applied_paused = pa;
                        }
                        gint64 cur = 0;
                        if (gst_element_query_position(p, GST_FORMAT_TIME, &cur) && cur >= 0)
                            file_pos_us.store(cur / 1000, std::memory_order_relaxed);
                    }
                }
                gst_object_unref(bus);

                // Прожив довго — збій разовий, наступна пауза з початку.
                if (now_ms() - t0 > 10000) backoff_ms = 500;
            } else if (p && should_log()) {
                // Причина — на шині: без неї "не стартував" нічого не каже
                // (зайнята карта, нема файлу, битий контейнер — усе одно).
                std::string why = "?";
                if (GstBus* b = gst_element_get_bus(p)) {
                    if (GstMessage* m = gst_bus_pop_filtered(b, GST_MESSAGE_ERROR)) {
                        GError* e = nullptr; gchar* dbg = nullptr;
                        gst_message_parse_error(m, &e, &dbg);
                        if (e) { why = e->message; g_error_free(e); }
                        g_free(dbg);
                        gst_message_unref(m);
                    }
                    gst_object_unref(b);
                }
                std::fprintf(stderr, "[звук %s] не стартував (%s) на %s: %s\n",
                             cfg.name.c_str(), src_name(src), cfg.device.c_str(), why.c_str());
            }

            teardown(p);
            if (!running.load(std::memory_order_relaxed)) break;
            if (want_rebuild.load(std::memory_order_relaxed)) continue;   // новий запит — одразу
            nap(backoff_ms);
            backoff_ms = std::min(backoff_ms * 2, 8000);
        }
    }

    // Змінити запит. rebuild — пайплайн треба зібрати наново (інше
    // джерело чи файл); інакше потік виходу підхопить зміну на живому.
    template <class F> void request(F&& f, bool rebuild) {
        { std::lock_guard<std::mutex> lk(want_mtx); f(); }
        if (rebuild) want_rebuild.store(true, std::memory_order_relaxed);
        wake();
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
    impl_->wake();
    if (impl_->th.joinable()) impl_->th.join();
}

void AudioOut::set_udp_port(int port) {
    impl_->port_override.store(port, std::memory_order_relaxed);
    Impl& d = *impl_;
    // Порт важить лише ефіру; на записі чи в тиші перебудовувати нема що.
    bool live;
    { std::lock_guard<std::mutex> lk(d.want_mtx); live = d.want_src == Src::Live; }
    if (live) { d.want_rebuild.store(true, std::memory_order_relaxed); d.wake(); }
}

void AudioOut::set_volume(float v) {
    if (v < 0.f) v = 0.f;
    impl_->volume.store(v, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(impl_->vol_mtx);
    if (impl_->vol_el) g_object_set(impl_->vol_el, "volume", (gdouble)v, nullptr);
}

AudioLevel AudioOut::level() const {
    AudioLevel l;
    l.rms_db = impl_->rms_db.load(std::memory_order_relaxed);
    l.peak_db = impl_->peak_db.load(std::memory_order_relaxed);
    const int64_t at = impl_->level_at_ms.load(std::memory_order_relaxed);
    l.age_ms = at < 0 ? -1 : now_ms() - at;
    return l;
}

void AudioOut::play_file(const std::string& path, int64_t pos_us, bool paused) {
    Impl& d = *impl_;
    bool rebuild;
    {
        std::lock_guard<std::mutex> lk(d.want_mtx);
        rebuild = d.want_src != Src::File || d.want_path != path;
    }
    d.request([&] {
        d.want_src = Src::File; d.want_path = path; d.want_pos_us = pos_us;
        d.want_paused = paused; ++d.want_seek_gen;
    }, rebuild);
}

void AudioOut::seek_file(int64_t pos_us) {
    Impl& d = *impl_;
    d.request([&] { d.want_pos_us = pos_us; ++d.want_seek_gen; }, false);
}

void AudioOut::pause_file(bool paused) {
    Impl& d = *impl_;
    bool same;
    { std::lock_guard<std::mutex> lk(d.want_mtx); same = d.want_paused == paused; }
    if (!same) d.request([&] { d.want_paused = paused; }, false);
}

void AudioOut::go_live() {
    Impl& d = *impl_;
    bool same;
    { std::lock_guard<std::mutex> lk(d.want_mtx); same = d.want_src == Src::Live; }
    if (!same) d.request([&] { d.want_src = Src::Live; d.want_path.clear(); }, true);
}

void AudioOut::go_silent() {
    Impl& d = *impl_;
    bool same;
    { std::lock_guard<std::mutex> lk(d.want_mtx); same = d.want_src == Src::Silent; }
    if (!same) d.request([&] { d.want_src = Src::Silent; d.want_path.clear(); }, true);
}

int64_t AudioOut::file_position_us() const {
    return impl_->file_pos_us.load(std::memory_order_relaxed);
}

AudioOutStats AudioOut::stats() const {
    AudioOutStats s;
    s.buffers = impl_->buffers.load(std::memory_order_relaxed);
    s.restarts = impl_->restarts.load(std::memory_order_relaxed);
    s.playing = impl_->playing.load(std::memory_order_relaxed);
    return s;
}

} // namespace vrx::audio
