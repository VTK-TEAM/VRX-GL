#include "recorder.hpp"

#include "../diag/record_log.hpp"

#include <gst/gst.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/falloc.h>
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

    // ПЕРЕКРИТТЯ ВХІДНОГО ПОРТУ НА ХОДУ (ліцензійний саботаж). -1 = діє
    // cfg.udp_port. На "лівому" порту проба не бачить пакетів -> сигнал
    // "зник" -> файл закривається, і клон нічого корисного не пише. Той
    // самий приймач шле на 5600, тож рекордер мусить саботуватись окремо
    // від показу: у нього СВІЙ udpsrc і своя проба.
    std::atomic<int> port_override{-1};
    std::atomic<bool> want_reopen{false};
    int eff_port() const {
        const int p = port_override.load(std::memory_order_relaxed);
        return p < 0 ? cfg.udp_port : p;
    }

    // Елемент udpsrc для пайплайну. Для мультикаст-групи (внутрішній
    // стрімер захвату) додаємо address + iface=lo, щоб приймати саме її
    // з loopback; інакше — звичайний порт.
    std::string udpsrc_prefix() const {
        std::string s = "udpsrc";
        if (!cfg.multicast_addr.empty()) {
            s += " address=" + cfg.multicast_addr + " multicast-iface=lo";
        }
        s += " port=" + std::to_string(eff_port());
        return s;
    }

    // Лічильники веде пад-проба, тобто потік GStreamer.
    std::atomic<uint64_t> file_bytes{0};
    std::atomic<int64_t> last_buffer_ms{0};

    // РЕЗЕРВУВАННЯ МІСЦЯ ПІД ФАЙЛ: другий дескриптор того самого файлу,
    // відкритий лише заради fallocate. Пише в файл, як і раніше, filesink
    // своїм власним.
    //
    // Це не оптимізація, а умова того, що запис ВСТИГАЄ. Три канали
    // пишуть одночасно, і на FAT кластери трьох файлів лягають упереміш
    // по 32 КБ. Розкидане так записування дає ~3-4 МБ/с проти 13.8 МБ/с
    // суцільного (заміряно на робочій флешці) — МЕНШЕ за потік даних
    // ~4.2 МБ/с. Наслідок заміряно теж: брудні сторінки ростуть
    // необмежено, syncfs у Storage не завершується десятки секунд і
    // довше, а зупинка станції (перехід у редактор) чекала 3+ хвилини,
    // поки ядро дожене чергу. З резервом суцільними шматками ті самі три
    // потоки дають ~15 МБ/с — утричі вище за потік, і черга не росте.
    //
    // Саме KEEP_SIZE, а не звичайний режим: розмір файлу завжди дорівнює
    // реально записаному, тож висмикнута флешка показує чесний файл без
    // хвоста нулів (індексу в mkv немає — streamable). Зайві кластери за
    // межею розміру ядро звільняє саме при закритті. Якщо ФС резервувати
    // не вміє або місце скінчилось — пишемо без резерву, як раніше.
    static constexpr uint64_t kReserveChunk    = 64ull * 1024 * 1024;
    static constexpr uint64_t kReserveHeadroom = 16ull * 1024 * 1024;
    int resv_fd = -1;
    uint64_t reserved = 0;

    // Поки не пройшов перший опорний кадр, буфери відкидаються.
    std::atomic<bool> seen_keyframe{false};

    uint32_t drive_generation = 0;
    int64_t last_sync_ms = 0;

    // Коли пайплайн зібрано. Фора новому пайплайну відлічується саме
    // звідси, а не від останнього перезбирання: на старті останнього ще
    // не було, лічильник дорівнював би нулю, і сторож зніс би пайплайн,
    // який щойно піднявся.
    int64_t built_at_ms = 0;

    // Прогресивна пауза. Рахує САМЕ СТАЛІ збої: пайплайн, що прожив
    // довше за kHealthyMs і віддав байти, обнуляє лічильник, байдуже,
    // чим саме він потім закінчився.
    int fail_streak = 0;
    int64_t next_open_ms = 0;
    static constexpr int64_t kHealthyMs = 5000;

    int64_t last_probe_open_ms = 0;

    // Тільки для детального логу: попередній стан, щоб писати ПЕРЕХОДИ, а
    // не однакові рядки чотири рази на секунду.
    bool prev_alive = false;
    bool prev_drive_ok = false;
    int64_t last_beat_ms = 0;

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
        addr.sin_port = htons((uint16_t)eff_port());
        if (::bind(probe_fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::fprintf(stderr, "[запис %s] проба сигналу не стала на порт %d: %s\n",
                         cfg.name.c_str(), eff_port(), std::strerror(errno));
            ::close(probe_fd);
            probe_fd = -1;
            return false;
        }

        // Мультикаст (внутрішній стрімер захвату): проба мусить ПРИЄДНАТИСЬ
        // до групи на loopback, інакше датаграм не побачить. Без цього
        // сигнал завжди читався б як "немає", і файл не створювався б.
        if (!cfg.multicast_addr.empty()) {
            ip_mreq mreq{};
            mreq.imr_multiaddr.s_addr = ::inet_addr(cfg.multicast_addr.c_str());
            mreq.imr_interface.s_addr = ::inet_addr("127.0.0.1");
            if (::setsockopt(probe_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                             &mreq, sizeof(mreq)) != 0) {
                std::fprintf(stderr, "[запис %s] проба не приєдналась до групи %s: %s\n",
                             cfg.name.c_str(), cfg.multicast_addr.c_str(),
                             std::strerror(errno));
            }
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

    // ШИНА GSTREAMER, ЯКУ РАНІШЕ НЕ ЧИТАВ НІХТО.
    //
    // Автобус опитувався рівно в одному місці — усередині close_file()
    // при штатному дозаписі. Тобто поки пайплайн живий, будь-яка помилка
    // лишалась непоміченою: elemenet падав, запис ставав, а назовні
    // `active` далі казав "пишу", і байти просто переставали рости.
    // Вийти з цього можна було тільки через зникнення носія або сигналу.
    //
    // Опитування НЕБЛОКУЮЧЕ: власного циклу подій GLib тут немає й не
    // треба, а такт рекордера і так 250 мс.
    //
    // EOS тут теж помилка. Джерело — `udpsrc`, воно не закінчується
    // саме; EOS означає, що гілка розібралась не з нашої волі.
    bool bus_failed() {
        if (!pipeline) return false;
        GstBus* bus = gst_element_get_bus(pipeline);
        if (!bus) return false;

        bool failed = false;
        while (GstMessage* m = gst_bus_pop_filtered(
                   bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(m, &e, &dbg);
                std::fprintf(stderr, "[запис %s] помилка на шині: %s\n",
                             cfg.name.c_str(), e && e->message ? e->message : "?");
                // Повне повідомлення з елементом і налагоджувальним
                // хвостом — саме воно каже, ХТО з ланцюжка впав, а в
                // штатний лог такий обсяг не поставиш.
                VRX_RLOG(cfg.name.c_str(), "ШИНА: помилка від %s: %s | %s",
                         GST_OBJECT_NAME(GST_MESSAGE_SRC(m)),
                         e && e->message ? e->message : "?", dbg ? dbg : "—");
                if (e) g_error_free(e);
                if (dbg) g_free(dbg);
            } else {
                std::fprintf(stderr, "[запис %s] несподіваний EOS\n", cfg.name.c_str());
                VRX_RLOG(cfg.name.c_str(), "ШИНА: несподіваний EOS від %s",
                         GST_OBJECT_NAME(GST_MESSAGE_SRC(m)));
            }
            failed = true;
            gst_message_unref(m);
        }
        gst_object_unref(bus);
        return failed;
    }

    // Дані від борту йдуть, а до парсера не доходить нічого.
    //
    // Не плутати з "сигнал зник": там винен борт, і перезбирати нема
    // чого — udpsrc підхопить потік сам. Тут винен наш бік, і єдиний
    // спосіб щось змінити — зібрати пайплайн наново.
    // Пауза відлічується від МОМЕНТУ ЗАКРИТТЯ, а не від спроби: дозапис
    // сам по собі триває до 10 секунд, і пауза, відрахована наперед,
    // вичерпалась би ще до того, як пайплайн розібрався.
    void back_off() {
        int64_t back = cfg.retry_ms;
        for (int i = 1; i < fail_streak && back < cfg.retry_max_ms; ++i) back *= 2;
        if (back > cfg.retry_max_ms) back = cfg.retry_max_ms;
        next_open_ms = now_ms() + back;
        std::fprintf(stderr, "[запис %s] %d-й сталий збій поспіль, пауза %lld мс\n",
                     cfg.name.c_str(), fail_streak, (long long)back);
    }

    bool stalled(int64_t now) const {
        if (!pipeline) return false;
        const int64_t last = last_buffer_ms.load(std::memory_order_relaxed);
        return (now - last) > cfg.stall_ms && (now - built_at_ms) > cfg.stall_ms;
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

    // Тримає резерв попереду записаного. Викликається щотіку циклу;
    // поки запас не з'їдено — коштує атомарне читання й порівняння.
    // Розмір беремо з лічильника проби, а не зі stat(): stat на щойно
    // висмикнутій флешці висить у D-стані секунди (див. on_bytes).
    void reserve_more() {
        if (resv_fd < 0) return;
        const uint64_t written = file_bytes.load(std::memory_order_relaxed);
        if (reserved > written + kReserveHeadroom) return;

        const uint64_t want = reserved + kReserveChunk;
        if (::fallocate(resv_fd, FALLOC_FL_KEEP_SIZE, 0, (off_t)want) != 0) {
            std::fprintf(stderr, "[запис %s] резерв не вдався (%s) — пишу без нього\n",
                         cfg.name.c_str(), std::strerror(errno));
            ::close(resv_fd);
            resv_fd = -1;
            return;
        }
        reserved = want;
    }

    bool open_file() {
        const int64_t open_t0 = now_ms();

        // make_path МОЖЕ ЗАБЛОКУВАТИСЬ: усередині mkdir на носії, тобто
        // те саме блокуюче I/O. Скільки саме — видно лише звідси.
        const std::string path = storage.make_path(cfg.name);
        if (path.empty()) {
            VRX_RLOG(cfg.name.c_str(), "НЕ ВІДКРИВ: шлях не склався (%lld мс)",
                     (long long)(now_ms() - open_t0));
            return false;
        }

        char desc[1024];
        if (cfg.codec == Recorder::Codec::MJPEG) {
            // Без jitterbuffer: RTP немає, переставляти нічого. jpegparse
            // потрібен не лише для меж кадрів — без коректних image/jpeg
            // на буферах muxer мовчки не запише жодного кадру.
            std::snprintf(desc, sizeof(desc),
                "%s "
                "! jpegparse name=parse "
                "! queue name=buf leaky=downstream max-size-time=%llu "
                "max-size-bytes=%d max-size-buffers=%d "
                "! matroskamux name=mux streamable=true "
                "! filesink name=sink location=\"%s\" sync=false async=false",
                udpsrc_prefix().c_str(), (unsigned long long)cfg.queue_ms * 1000000ULL,
                cfg.queue_bytes, cfg.queue_buffers, path.c_str());
        } else {
        std::snprintf(desc, sizeof(desc),
            "udpsrc port=%d "
            "caps=\"application/x-rtp,media=video,clock-rate=90000,"
            "encoding-name=H265,payload=%d\" "
            "! rtpjitterbuffer latency=%d drop-on-latency=false "
            "! rtph265depay "
            "! h265parse name=parse config-interval=-1 "
            "! queue name=buf leaky=downstream max-size-time=%llu "
                "max-size-bytes=%d max-size-buffers=%d "
            "! matroskamux name=mux streamable=true "
            "! filesink name=sink location=\"%s\" sync=false async=false",
            eff_port(), cfg.payload_type, cfg.jitter_ms,
            (unsigned long long)cfg.queue_ms * 1000000ULL,
            cfg.queue_bytes, cfg.queue_buffers, path.c_str());
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
        built_at_ms = now_ms();
        seen_keyframe.store(!cfg.start_on_keyframe, std::memory_order_release);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "[запис %s] не запустився\n", cfg.name.c_str());
            close_file(false, "пайплайн не запустився");
            return false;
        }

        // Резерв відкриваємо ПІСЛЯ старту пайплайна: filesink створює
        // файл із O_TRUNC, і зроблене раніше резервування він би зніс.
        resv_fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
        reserved = 0;
        if (resv_fd < 0) {
            std::fprintf(stderr, "[запис %s] резерв: не відкрив файл (%s)\n",
                         cfg.name.c_str(), std::strerror(errno));
        } else {
            reserve_more();
        }

        {
            std::lock_guard<std::mutex> lk(mtx);
            st.active = true;
            st.file = path;
            st.bytes = 0;
            st.files++;
        }
        std::fprintf(stderr, "[запис %s] почав: %s\n", cfg.name.c_str(), path.c_str());
        VRX_RLOG(cfg.name.c_str(), "ВІДКРИВ %s (файл №%u, підняття %lld мс)",
                 path.c_str(), st.files, (long long)(now_ms() - open_t0));
        return true;
    }

    // ЧОМУ МУКСЕР У РЕЖИМІ streamable.
    //
    // За замовчуванням matroskamux при завершенні ПОВЕРТАЄТЬСЯ НАЗАД по
    // файлу й дописує тривалість та таблицю переходів. Поки запис
    // завершується штатно, це добре. Але цей запис за задумом можуть
    // обірвати будь-якої миті — висмикнути флешку, зняти живлення — і
    // тоді дописувати нікуди: файл лишається без службових структур,
    // тобто битий. Перевірено на залізі: після висмикування носія
    // 417 МБ і 267 МБ не читалися.
    //
    // streamable=true пише так, щоб повертатися не треба було взагалі.
    // Ціна — немає таблиці переходів навіть при штатному закритті, тож
    // перемотка йде скануванням. Це дешево порівняно з утратою всього
    // файлу, а для запису польоту перемотка й не головне.

    // graceful — дочекатися дозапису. Без нього муксер не встигне
    // випхати чергу, і хвіст запису просто не потрапить у файл.
    void close_file(bool graceful, const char* why) {
        if (!pipeline) return;

        const int64_t close_t0 = now_ms();
        VRX_RLOG(cfg.name.c_str(), "ЗАКРИВАЮ %s (%s), дозапис %s, у файлі %.1f МБ,"
                 " брудних %ld КБ, прожив %lld мс",
                 st.file.c_str(), why, graceful ? "так" : "НІ",
                 file_bytes.load(std::memory_order_relaxed) / 1e6, VRX_RLOG_DIRTY(),
                 (long long)(built_at_ms ? now_ms() - built_at_ms : 0));

        // Знімаємо "пишу" ДО розбирання, а не після.
        //
        // Далі йде дозапис до 10 секунд, а на мертвому носії ще й
        // set_state(NULL) може впертися в заблокований filesink. Усе це
        // час, протягом якого назовні не має стояти "запис іде": канал
        // 200 в OSD показував би пілоту роботу, якої вже немає.
        {
            std::lock_guard<std::mutex> lk(mtx);
            st.active = false;
        }

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
            VRX_RLOG(cfg.name.c_str(), "дозапис зайняв %lld мс",
                     (long long)(now_ms() - close_t0));
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

        // Резерв просто закриваємо: незаписані кластери за межею розміру
        // ядро звільняє саме. ftruncate тут вимагав би fstat — звернення
        // до носія рівно тоді, коли він може бути вже висмикнутий.
        if (resv_fd >= 0) { ::close(resv_fd); resv_fd = -1; }
        reserved = 0;

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

        // Пайплайн, який пожив і віддав байти, — не сталий збій, чим би
        // він потім не закінчився. Лічильник міряє саме СТАЛІСТЬ: інакше
        // ротація файлів або звичайне зникнення сигналу накручували б
        // паузу так само, як несправність.
        if (wrote > 0 && built_at_ms > 0 && (now_ms() - built_at_ms) > kHealthyMs) {
            fail_streak = 0;
        }
        built_at_ms = 0;

        {
            std::lock_guard<std::mutex> lk(mtx);
            st.active = false;
            st.total_bytes += wrote;
            st.bytes = 0;
            st.file.clear();
        }
        std::fprintf(stderr, "[запис %s] закрив, %.1f МБ\n",
                     cfg.name.c_str(), wrote / 1e6);
        VRX_RLOG(cfg.name.c_str(), "ЗАКРИТО за %lld мс, %.1f МБ, брудних %ld КБ,"
                 " сталих збоїв поспіль %d",
                 (long long)(now_ms() - close_t0), wrote / 1e6, VRX_RLOG_DIRTY(),
                 fail_streak);

        // Просимо скинути кеші одразу: файл щойно закрито, і саме зараз
        // втрата була б найприкрішою.
        storage.request_sync();
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            const int64_t now = now_ms();

            // ЗМІНА ПОРТУ НА ХОДУ. Закриваємо пробу (стане на новий порт
            // наступним тіком) і поточний файл — далі звичайна логіка сама
            // відкриє новий, коли/якщо на новому порту з'явиться сигнал.
            if (want_reopen.exchange(false, std::memory_order_relaxed)) {
                if (probe_fd >= 0) { ::close(probe_fd); probe_fd = -1; }
                last_probe_open_ms = 0;
                last_packet_ms = 0;
                if (pipeline) close_file(false, "зміна порту");
            }

            // ПОВТОРНА СПРОБА ВІДКРИТИ ПРОБУ.
            //
            // Раніше сокет відкривався один раз перед циклом, і якщо
            // bind не пройшов — signal_present() лишався false назавжди,
            // тобто файл не створювався б НІКОЛИ, а в лозі був один
            // рядок. Той самий клас, що й невдалий build() у джерелі:
            // разова невдача на старті вбивала підсистему до кінця
            // польоту.
            if (probe_fd < 0 && now - last_probe_open_ms >= 2000) {
                last_probe_open_ms = now;
                open_probe();
            }
            poll_probe();
            reserve_more();     // тримаємо резерв попереду записаного

            const DriveState drive = storage.state();

            // Живість беремо з проби, а не з кадрів у пайплайні: проба
            // працює й тоді, коли пайплайну немає, і саме тому файл
            // створюється лише під реальний сигнал.
            const bool alive = signal_present();

            {
                std::lock_guard<std::mutex> lk(mtx);
                st.bytes = file_bytes.load(std::memory_order_relaxed);
                st.stream_alive = alive;
            }

            // ЗНІМОК НОСІЯ МОЖЕ БУТИ ЗАСТАРІЛИМ, і це не те саме, що
            // "носія немає". Потік опитування блокується на мертвій
            // флешці разом із рештою — і тоді state() чесно віддає
            // ОСТАННІЙ знімок, у якому все гаразд. Вік знімка рахувався й
            // документувався від початку, але не читався ніким.
            const bool stale = drive.age_ms > cfg.drive_stale_ms;
            const bool drive_ok = drive.usable() && !stale;

            // ПЕРЕХОДИ, а не стан: рядок раз на 250 мс перетворив би лог
            // на кашу, у якій сама подія загубиться.
            if (alive != prev_alive) {
                VRX_RLOG(cfg.name.c_str(), "СИГНАЛ %s (останній пакет %lld мс тому)",
                         alive ? "з'явився" : "ЗНИК",
                         (long long)(last_packet_ms ? now - last_packet_ms : -1));
                prev_alive = alive;
            }
            if (drive_ok != prev_drive_ok) {
                VRX_RLOG(cfg.name.c_str(), "НОСІЙ %s: %s, номер %u, знімку %lld мс",
                         drive_ok ? "придатний" : "НЕпридатний",
                         drive.root.empty() ? "—" : drive.root.c_str(),
                         drive.generation, (long long)drive.age_ms);
                prev_drive_ok = drive_ok;
            }

            // Биття раз на секунду. Потрібне не для подій, а для ТИШІ:
            // якщо биття зникло на десять секунд, значить цикл рекордера
            // сам десь застряг, і без цього ряду цього не побачити ніяк.
            if (now - last_beat_ms >= 1000) {
                last_beat_ms = now;
                VRX_RLOG(cfg.name.c_str(),
                         "· %.1f МБ | сигнал %d | носій %d знімку %lld мс |"
                         " кадр %lld мс тому | збоїв %d | брудних %ld КБ",
                         file_bytes.load(std::memory_order_relaxed) / 1e6,
                         (int)alive, (int)drive_ok, (long long)drive.age_ms,
                         (long long)(pipeline
                             ? now - last_buffer_ms.load(std::memory_order_relaxed) : -1),
                         fail_streak, VRX_RLOG_DIRTY());
            }

            if (pipeline) {
                // Носій зник, підмінили іншим або знімок протух — далі
                // писати нікуди. Graceful не пробуємо: файлової системи
                // вже немає, і EOS завис би на весь таймаут.
                //
                // Перевіряється ПЕРШИМ, хоч шина при цьому теж кричить.
                // Зникнення носія — ПРИЧИНА, а помилка на шині — її
                // наслідок, і в лозі має стояти причина. Інакше кожне
                // висмикування флешки рахувалося б як внутрішній збій
                // пайплайна, і лічильник помилок перестав би означати те,
                // заради чого заведений.
                if (!drive.usable() || drive.generation != drive_generation || stale) {
                    // Три різні події, і в лозі вони мають читатися
                    // по-різному: "зник" і "перемонтовано" лікуються
                    // однаково, але означають зовсім різне, а розрізнити
                    // їх постфактум по одному рядку буде нічим.
                    const char* why = stale  ? "знімок носія застарів — опит застряг"
                                    : !drive.usable() ? "носій зник"
                                    : "носій перемонтовано — файлова система вже інша";
                    std::fprintf(stderr, "[запис %s] %s\n", cfg.name.c_str(), why);
                    close_file(false, why);
                    if (stale) {
                        fail_streak++;
                        back_off();
                        std::lock_guard<std::mutex> lk(mtx);
                        st.drive_stale++;
                    }
                }
                // Носій на місці, а пайплайн повідомив про помилку —
                // отже зламались саме ми. Йде ПЕРЕД перевіркою сигналу:
                // на зламаному пайплайні дозапис через EOS не пройде, і
                // пробувати його означало б чекати весь таймаут дарма.
                else if (bus_failed()) {
                    close_file(false, "помилка на шині");
                    fail_streak++;
                    back_off();
                    std::lock_guard<std::mutex> lk(mtx);
                    st.errors++;
                }
                // Сигнал пропав — закриваємо файл. При поверненні буде
                // НОВИЙ файл, а не дозапис у старий: між ними діра, і
                // склеювати їх в один — гірше, ніж розділити.
                else if (!alive) {
                    std::fprintf(stderr, "[запис %s] сигнал зник\n", cfg.name.c_str());
                    close_file(true, "сигнал зник");
                    std::lock_guard<std::mutex> lk(mtx);
                    st.restarts++;
                }
                // Дані йдуть, а кадрів на виході парсера немає — винен
                // наш бік. Дозапису не пробуємо: пайплайн, який не
                // віддає кадрів, з великою ймовірністю не віддасть і EOS.
                else if (stalled(now)) {
                    std::fprintf(stderr, "[запис %s] сигнал є, а кадрів немає %d с —"
                                 " перезбираю\n", cfg.name.c_str(), cfg.stall_ms / 1000);
                    close_file(false, "сигнал є, а кадрів немає");
                    fail_streak++;
                    back_off();
                    std::lock_guard<std::mutex> lk(mtx);
                    st.stalls++;
                }
                // Межа розміру.
                else if (file_bytes.load(std::memory_order_relaxed) >= cfg.rotate_bytes) {
                    close_file(true, "ротація за розміром");
                    std::lock_guard<std::mutex> lk(mtx);
                    st.rotations++;
                }
                // Скидання кешів на носій. Саме воно обмежує втрату при
                // висмикуванні: без нього незаписане накопичується в
                // ядрі, і заміряно, що це понад 20 секунд відео.
                else if (now - last_sync_ms >= cfg.sync_interval_ms) {
                    last_sync_ms = now;
                    storage.request_sync();
                }
            }

            // Файл створюємо ЛИШЕ коли є і носій, і сигнал.
            if (!pipeline && drive_ok && alive && now_ms() >= next_open_ms) {
                drive_generation = drive.generation;
                if (open_file()) {
                    last_sync_ms = now_ms();
                } else {
                    // Не зібрався — це теж сталий збій: наступного разу
                    // причина буде та сама, і крутити спроби чотири рази
                    // на секунду означало б лише залити лог.
                    fail_streak++;
                    back_off();
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        if (pipeline) close_file(true, "зупинка станції");
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

void Recorder::set_udp_port(int port) {
    impl_->port_override.store(port, std::memory_order_relaxed);
    impl_->want_reopen.store(true, std::memory_order_relaxed);
}

RecordStats Recorder::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

} // namespace vrx::record
