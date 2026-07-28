#include "recorder.hpp"

#include <gst/gst.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

namespace vrx::record {
namespace {

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

} // namespace

struct Recorder::Impl {
    Config cfg;
    Storage& storage;

    std::thread th;
    std::atomic<bool> running{false};

    GstElement* pipeline = nullptr;
    gulong probe_id = 0;
    GstPad* probe_pad = nullptr;
    gulong frame_probe = 0;
    GstPad* frame_pad = nullptr;

    mutable std::mutex mtx;
    RecordStats st{};

    // Лічильники веде пад-проба, тобто потік GStreamer.
    std::atomic<uint64_t> file_bytes{0};
    std::atomic<int64_t> last_buffer_ms{0};

    // Поки не пройшов перший опорний кадр, буфери відкидаються.
    std::atomic<bool> seen_keyframe{false};

    uint32_t drive_generation = 0;

    // ЛЕГКА ПРОБА СИГНАЛУ: власний сокет на тому ж порту, без GStreamer.
    //
    // Навіщо. Без неї єдиний спосіб дізнатися, чи є сигнал, — підняти
    // пайплайн, а він одразу створює файл. При вставленій флешці й
    // вимкненій камері виходив цикл "відкрив -> 3 с без кадрів ->
    // закрив": заміряно, 10 файлів по 336 байтів за 40 секунд, самі
    // заголовки. За політ це тисячі сміттєвих файлів у корені запису.
    //
    // Сокет тут коштує нічого: борт шле бродкастом, ядро віддає копію
    // датаграми кожному сокету на порту, і зайвий слухач нікому не
    // заважає. Дані не розбираємо — питання лише "чи летить".
    int probe_fd = -1;
    int64_t last_packet_ms = 0;

    Impl(Config c, Storage& s) : cfg(std::move(c)), storage(s) {}

    bool open_probe() {
        probe_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (probe_fd < 0) return false;

        int one = 1;
        ::setsockopt(probe_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        ::setsockopt(probe_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)cfg.udp_port);
        if (::bind(probe_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::fprintf(stderr, "[запис %s] проба сигналу не стала на порт %d: %s\n",
                         cfg.name.c_str(), cfg.udp_port, std::strerror(errno));
            ::close(probe_fd);
            probe_fd = -1;
            return false;
        }
        return true;
    }

    // Вичитує все, що накопичилось, і оновлює час останнього пакета.
    // Вичитувати ОБОВ'ЯЗКОВО до кінця: недочитаний сокет переповнюється,
    // і ядро починає викидати датаграми — свої, не чужі, але шкода все
    // одно зайва.
    void poll_probe() {
        if (probe_fd < 0) return;
        char buf[2048];
        bool got = false;
        while (::recv(probe_fd, buf, sizeof(buf), 0) > 0) got = true;
        if (got) last_packet_ms = now_ms();
    }

    bool signal_present() const {
        return last_packet_ms > 0 && (now_ms() - last_packet_ms) < cfg.stream_lost_ms;
    }

    // Проба на виході ПАРСЕРА, тобто ДО муксера.
    //
    // Місце принципове. Тут буфер — це один кадр H.265, і DELTA_UNIT на
    // ньому означає рівно "не опорний". Після муксера прапорець стосується
    // вже блоків контейнера, і фільтр за ним викидає службові буфери
    // муксера разом із заголовком EBML: файл росте бездоганно, важить свої
    // сотні мегабайтів, але починається з Cluster замість 1a45dfa3 і не
    // відкривається нічим. Перевірено на залізі — 143 МБ у смітник.
    //
    // Тут же засікаємо час кадру: живість потоку — це саме кадри від
    // борту, а не байти, що дотекли до диска.
    static GstPadProbeReturn on_frame(GstPad*, GstPadProbeInfo* info, gpointer user) {
        auto* d = static_cast<Impl*>(user);
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buf) return GST_PAD_PROBE_OK;

        d->last_buffer_ms.store(now_ms(), std::memory_order_relaxed);

        if (d->cfg.start_on_keyframe && !d->seen_keyframe.load(std::memory_order_acquire)) {
            if (GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) {
                return GST_PAD_PROBE_DROP;      // ще не опорний
            }
            d->seen_keyframe.store(true, std::memory_order_release);
        }
        return GST_PAD_PROBE_OK;
    }

    // Проба на вході filesink: САМЕ ЛИШЕ рахування розміру. Потрібна,
    // щоб не звертатися до файлової системи взагалі: stat() на щойно
    // висмикнутій флешці висить у D-стані реальні секунди.
    static GstPadProbeReturn on_bytes(GstPad*, GstPadProbeInfo* info, gpointer user) {
        auto* d = static_cast<Impl*>(user);
        if (GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info)) {
            d->file_bytes.fetch_add(gst_buffer_get_size(buf), std::memory_order_relaxed);
        }
        return GST_PAD_PROBE_OK;
    }

    bool open_file() {
        const std::string path = storage.make_path(cfg.name);
        if (path.empty()) return false;

        char desc[1024];
        if (cfg.codec == Recorder::Codec::MJPEG) {
            // Без jitterbuffer: RTP немає, переставляти нічого. jpegparse
            // потрібен не лише для меж кадрів — без коректних image/jpeg
            // на буферах muxer мовчки не запише жодного кадру.
            std::snprintf(desc, sizeof(desc),
                "udpsrc port=%d "
                "! jpegparse name=parse "
                "! queue name=buf max-size-time=%llu max-size-bytes=0 max-size-buffers=0 "
                "! matroskamux name=mux "
                "! filesink name=sink location=\"%s\" sync=false async=false",
                cfg.udp_port, (unsigned long long)cfg.queue_ms * 1000000ULL, path.c_str());
        } else {
        std::snprintf(desc, sizeof(desc),
            "udpsrc port=%d "
            "caps=\"application/x-rtp,media=video,clock-rate=90000,"
            "encoding-name=H265,payload=%d\" "
            "! rtpjitterbuffer latency=%d drop-on-latency=false "
            "! rtph265depay "
            "! h265parse name=parse config-interval=-1 "
            "! queue name=buf max-size-time=%llu max-size-bytes=0 max-size-buffers=0 "
            "! matroskamux name=mux "
            "! filesink name=sink location=\"%s\" sync=false async=false",
            cfg.udp_port, cfg.payload_type, cfg.jitter_ms,
            (unsigned long long)cfg.queue_ms * 1000000ULL, path.c_str());
        }

        GError* err = nullptr;
        pipeline = gst_parse_launch(desc, &err);
        if (!pipeline) {
            std::fprintf(stderr, "[запис %s] пайплайн не зібрався: %s\n",
                         cfg.name.c_str(), err ? err->message : "?");
            if (err) g_error_free(err);
            return false;
        }
        if (err) g_error_free(err);

        if (GstElement* parse = gst_bin_get_by_name(GST_BIN(pipeline), "parse")) {
            frame_pad = gst_element_get_static_pad(parse, "src");
            if (frame_pad) {
                frame_probe = gst_pad_add_probe(frame_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                                on_frame, this, nullptr);
            }
            gst_object_unref(parse);
        }
        if (GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink")) {
            probe_pad = gst_element_get_static_pad(sink, "sink");
            if (probe_pad) {
                probe_id = gst_pad_add_probe(probe_pad, GST_PAD_PROBE_TYPE_BUFFER,
                                             on_bytes, this, nullptr);
            }
            gst_object_unref(sink);
        }

        file_bytes.store(0, std::memory_order_relaxed);
        last_buffer_ms.store(now_ms(), std::memory_order_relaxed);
        seen_keyframe.store(!cfg.start_on_keyframe, std::memory_order_release);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "[запис %s] не запустився\n", cfg.name.c_str());
            close_file(false);
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            st.active = true;
            st.file = path;
            st.bytes = 0;
            st.files++;
        }
        std::fprintf(stderr, "[запис %s] почав: %s\n", cfg.name.c_str(), path.c_str());
        return true;
    }

    // graceful — дочекатися дозапису. Без нього файл лишиться без
    // індексу муксера, і програвач побачить обрубок.
    void close_file(bool graceful) {
        if (!pipeline) return;

        if (graceful) {
            // EOS проганяє чергу до кінця й змушує муксер дописати
            // заголовки. Чекаємо ОБМЕЖЕНО: якщо носія вже фізично
            // немає, EOS не дійде ніколи, і вічне очікування тут
            // означало б зависання зупинки всієї програми.
            gst_element_send_event(pipeline, gst_event_new_eos());

            GstBus* bus = gst_element_get_bus(pipeline);
            GstMessage* msg = gst_bus_timed_pop_filtered(
                bus, (GstClockTime)cfg.shutdown_flush_ms * GST_MSECOND,
                (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    std::fprintf(stderr, "[запис %s] помилка при дозаписі\n",
                                 cfg.name.c_str());
                }
                gst_message_unref(msg);
            } else {
                std::fprintf(stderr, "[запис %s] дозапис не встиг за %d мс\n",
                             cfg.name.c_str(), cfg.shutdown_flush_ms);
            }
            gst_object_unref(bus);
        }

        if (frame_pad) {
            if (frame_probe) gst_pad_remove_probe(frame_pad, frame_probe);
            gst_object_unref(frame_pad);
            frame_pad = nullptr;
            frame_probe = 0;
        }
        if (probe_pad) {
            if (probe_id) gst_pad_remove_probe(probe_pad, probe_id);
            gst_object_unref(probe_pad);
            probe_pad = nullptr;
            probe_id = 0;
        }

        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = nullptr;

        const uint64_t wrote = file_bytes.load(std::memory_order_relaxed);

        // Кадрів так і не було — лишився самий заголовок муксера.
        // Такий файл нікому не потрібен, а на флешці він живий сміттям.
        if (!seen_keyframe.load(std::memory_order_acquire) || wrote == 0) {
            std::string path;
            {
                std::lock_guard<std::mutex> lk(mtx);
                path = st.file;
            }
            if (!path.empty()) ::unlink(path.c_str());
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            st.active = false;
            st.total_bytes += wrote;
            st.bytes = 0;
            st.file.clear();
        }
        std::fprintf(stderr, "[запис %s] закрив, %.1f МБ\n",
                     cfg.name.c_str(), wrote / 1e6);

        // Просимо скинути кеші одразу: файл щойно закрито, і саме зараз
        // втрата була б найприкрішою.
        storage.request_sync();
    }

    void loop() {
        open_probe();

        while (running.load(std::memory_order_relaxed)) {
            poll_probe();

            const DriveState drive = storage.state();
            const int64_t now = now_ms();

            // Живість беремо з проби, а не з кадрів у пайплайні: проба
            // працює й тоді, коли пайплайну немає, і саме тому файл
            // створюється лише під реальний сигнал.
            const bool alive = signal_present();

            {
                std::lock_guard<std::mutex> lk(mtx);
                st.bytes = file_bytes.load(std::memory_order_relaxed);
                st.stream_alive = alive;
            }

            if (pipeline) {
                // Носій зник або його підмінили іншим — далі писати
                // нікуди. Graceful тут не пробуємо: файлової системи вже
                // немає, і EOS завис би на весь таймаут.
                if (!drive.usable() || drive.generation != drive_generation) {
                    std::fprintf(stderr, "[запис %s] носій зник\n", cfg.name.c_str());
                    close_file(false);
                }
                // Сигнал пропав — закриваємо файл. При поверненні буде
                // НОВИЙ файл, а не дозапис у старий: між ними діра, і
                // склеювати їх в один — гірше, ніж розділити.
                else if (!alive) {
                    std::fprintf(stderr, "[запис %s] сигнал зник\n", cfg.name.c_str());
                    close_file(true);
                    std::lock_guard<std::mutex> lk(mtx);
                    st.restarts++;
                }
                // Межа розміру.
                else if (file_bytes.load(std::memory_order_relaxed) >= cfg.rotate_bytes) {
                    close_file(true);
                    std::lock_guard<std::mutex> lk(mtx);
                    st.rotations++;
                }
            }

            // Файл створюємо ЛИШЕ коли є і носій, і сигнал.
            if (!pipeline && drive.usable() && alive) {
                drive_generation = drive.generation;
                open_file();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (pipeline) close_file(true);
        if (probe_fd >= 0) {
            ::close(probe_fd);
            probe_fd = -1;
        }
    }
};

Recorder::Recorder(Config cfg, Storage& storage)
    : impl_(new Impl(std::move(cfg), storage)) {}

Recorder::~Recorder() { stop(); }

bool Recorder::start() {
    if (impl_->running.exchange(true)) return true;
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[запис %s] потік піднято, порт %d\n",
                 impl_->cfg.name.c_str(), impl_->cfg.udp_port);
    return true;
}

void Recorder::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
}

RecordStats Recorder::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

} // namespace vrx::record
