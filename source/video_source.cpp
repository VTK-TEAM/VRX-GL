#include "video_source.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>

#include <drm_fourcc.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace vrx::source {
namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t drm_fourcc_of(GstVideoFormat f) {
    switch (f) {
        case GST_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
        case GST_VIDEO_FORMAT_NV21: return DRM_FORMAT_NV21;
        case GST_VIDEO_FORMAT_NV16: return DRM_FORMAT_NV16;
        case GST_VIDEO_FORMAT_I420: return DRM_FORMAT_YUV420;
        case GST_VIDEO_FORMAT_YUY2: return DRM_FORMAT_YUYV;
        case GST_VIDEO_FORMAT_UYVY: return DRM_FORMAT_UYVY;
        default: return 0;
    }
}

} // namespace

// ---------------------------------------------------------------------

struct VideoSource::Impl {
    // Зворотне посилання на власника — щоб питати в нащадка, з чого
    // складати пайплайн. Викликається лише зі start(), тобто після
    // повної побудови об'єкта: віртуальні виклики з конструктора не
    // дійшли б до нащадка.
    VideoSource* owner = nullptr;

    Impl(std::string n, Config c) : name(std::move(n)), cfg(c) {}

    std::string name;
    Config cfg;

    GstElement* pipeline = nullptr;
    GstElement* appsink = nullptr;
    GstBus* bus = nullptr;

    mutable std::mutex mtx;
    layout::Placement place{};
    SourceStats st{};

    // Черга готових кадрів замість одного слота.
    //
    // ЧОМУ ЧЕРГА, А НЕ "ПЕРЕМАГАЄ НАЙСВІЖІШИЙ". Заміряно на живому
    // борті: інтервал між кадрами НА ВИХОДІ ДЕКОДЕРА має min 0.2 мс при
    // середніх 16.7 — тобто mppvideodec регулярно віддає два кадри
    // майже одночасно, а потім мовчить до 34 мс. На мережі такого немає
    // (там min 2.5 мс), отже парність додає сам декодер.
    //
    // З одним слотом другий кадр пари витісняв перший: той зникав
    // назавжди, і рух переривався. Черга обидва зберігає й віддає по
    // одному на розгортку — картинка йде послідовно.
    //
    // Стеля на випадок, якщо рендерер надовго став. У нормальній роботі
    // глибину тримає не вона, а активне зливання в acquire().
    static constexpr size_t kMaxQueue = 4;
    std::deque<std::shared_ptr<SourceFrame>> queue;
    std::shared_ptr<SourceFrame> in_use;

    std::atomic<int64_t> last_frame_ms{0};

    // Замір рівномірності приходу — щоб відрізнити "декодер віддає
    // пачками" від "ми самі губимо кадри".
    double prev_arrival_us = 0;
    double iv_min = 0, iv_max = 0, iv_sum = 0;
    uint64_t iv_n = 0;

    // ФАКТИЧНА частота кадрів камери. Рахується так само, як частота
    // розгортки в дисплеї: лічильник кадрів на довгому вікні, а не
    // усереднення сусідніх інтервалів. Причина та сама — інтервали
    // приходу гуляють на ±5 мс через мережу, і будь-яке усереднення на
    // коротких вікнах дає похибку більшу за сам ефект, який ми ловимо
    // (сотні мілігерц).
    //
    // Це ОПОРНИЙ вимір для петлі: різниця цієї частоти й частоти
    // розгортки і є те, що треба звести до нуля.
    int64_t rate_anchor_ns = 0;
    uint64_t rate_anchor_n = 0;
    double produced_hz = 0;

    uint64_t q_sum = 0, q_n = 0;
    int q_max = 0;

    // Ті самі заміри, але на ВХОДІ декодера — щоб побачити, чи парність
    // приходить із мережі, чи її створює сам mppvideodec.
    std::mutex in_mtx;
    double in_prev_us = 0;
    double in_min = 0, in_max = 0, in_sum = 0;
    uint64_t in_n = 0;

    // Мітки часу подачі в декодер. Зіставляються з виходом за принципом
    // FIFO: у потоці без B-кадрів декодер віддає кадри в тому ж порядку,
    // в якому їх отримав, тож черга міток і черга кадрів ідуть синхронно.
    std::deque<double> in_times;
    double dec_min = 0, dec_max = 0, dec_sum = 0;
    uint64_t dec_n = 0;

    static GstPadProbeReturn on_dec_input(GstPad*, GstPadProbeInfo*, gpointer user) {
        auto* d = static_cast<Impl*>(user);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double us = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
        std::lock_guard<std::mutex> lk(d->in_mtx);
        if (d->in_prev_us > 0) {
            const double dt = (us - d->in_prev_us) / 1000.0;
            if (d->in_n == 0 || dt < d->in_min) d->in_min = dt;
            if (dt > d->in_max) d->in_max = dt;
            d->in_sum += dt;
            d->in_n++;
        }
        d->in_prev_us = us;
        d->last_input_ms.store((int64_t)(us / 1000.0), std::memory_order_relaxed);

        d->in_times.push_back(us);
        // Якщо декодер щось проковтнув, черга міток поповзе — тоді
        // зіставлення втратить сенс, тож обмежуємо й скидаємо надлишок.
        while (d->in_times.size() > 8) d->in_times.pop_front();
        return GST_PAD_PROBE_OK;
    }
    std::atomic<int> fw{0}, fh{0};

    std::thread watchdog;

    // Коли востаннє буфер зайшов У ДЕКОДЕР. Не те саме, що last_frame_ms:
    // той про ВИХІД. Різниця між ними й відрізняє "камера мовчить" від
    // "декодер завис".
    std::atomic<int64_t> last_input_ms{0};

    // Стан перезбирання: коли робили востаннє й скільки разів поспіль
    // не вдалося.
    int64_t last_restart_ms = 0;
    int fail_streak = 0;
    std::atomic<bool> running{false};

    // --- прийом кадру, викликається з потоку GStreamer ---

    void on_sample(GstSample* sample) {
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buf || !caps) { gst_sample_unref(sample); return; }

        GstVideoInfo vi;
        if (!gst_video_info_from_caps(&vi, caps)) { gst_sample_unref(sample); return; }

        GstMemory* mem = gst_buffer_peek_memory(buf, 0);
        if (!mem || !gst_is_dmabuf_memory(mem)) {
            // Без dmabuf уся архітектура не працює: довелось би тягнути
            // кадр через CPU. Краще сказати прямо, ніж тихо гальмувати.
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "[%s] декодер віддає НЕ dmabuf — zero-copy неможливий\n",
                             name.c_str());
                warned = true;
            }
            gst_sample_unref(sample);
            return;
        }

        auto frame = std::make_shared<SourceFrame>();
        display::Frame& f = frame->image;

        f.fourcc = drm_fourcc_of(GST_VIDEO_INFO_FORMAT(&vi));
        if (!f.fourcc) { gst_sample_unref(sample); return; }
        f.modifier = DRM_FORMAT_MOD_LINEAR;
        f.width  = GST_VIDEO_INFO_WIDTH(&vi);
        f.height = GST_VIDEO_INFO_HEIGHT(&vi);

        // Stride і offset беремо з GstVideoMeta, а не рахуємо самі.
        // Вирівнювання декодера сидить САМЕ тут: stride майже ніколи не
        // дорівнює width*bpp, і припущення про це дає перекошену по
        // діагоналі картинку. А от зайві рядки (1088 замість 1080)
        // окремо обробляти не треба — ми імпортуємо рівно height рядків,
        // і решта просто не входить у зображення.
        GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
        const int n_planes = vm ? (int)vm->n_planes : (int)GST_VIDEO_INFO_N_PLANES(&vi);
        f.n_planes = n_planes > display::Frame::kMaxPlanes
                         ? display::Frame::kMaxPlanes : n_planes;

        const int fd = gst_dmabuf_memory_get_fd(mem);
        for (int i = 0; i < f.n_planes; ++i) {
            f.fd[i] = fd;   // площини NV12 лежать в ОДНОМУ dmabuf, різні лише offset
            f.stride[i] = vm ? (uint32_t)vm->stride[i] : (uint32_t)GST_VIDEO_INFO_PLANE_STRIDE(&vi, i);
            f.offset[i] = vm ? (uint32_t)vm->offset[i] : (uint32_t)GST_VIDEO_INFO_PLANE_OFFSET(&vi, i);
        }

        // Тримає GstSample живим, доки кадром користуються. Коли структура
        // помре — слот повернеться в пул декодера сам.
        f.keepalive = std::shared_ptr<void>(sample, [](void* p) {
            gst_sample_unref(static_cast<GstSample*>(p));
        });

        frame->pixel_aspect =
            GST_VIDEO_INFO_PAR_D(&vi) > 0
                ? float(GST_VIDEO_INFO_PAR_N(&vi)) / float(GST_VIDEO_INFO_PAR_D(&vi))
                : 1.0f;

        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const double us = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
            if (prev_arrival_us > 0) {
                const double dt = (us - prev_arrival_us) / 1000.0;
                if (iv_n == 0 || dt < iv_min) iv_min = dt;
                if (dt > iv_max) iv_max = dt;
                iv_sum += dt;
                iv_n++;
            }
            prev_arrival_us = us;
        }

        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            frame->produced_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;

            // Частота по лічильнику на вікні 1..30 с. Джитер приходу
            // (±5 мс) ділиться на довжину вікна: за 10 с це вже краще
            // за 2 мГц, тобто тонше за крок підстроювання камери.
            const int64_t now = frame->produced_ns;
            const uint64_t seq = st.produced;          // ще не інкрементований
            if (rate_anchor_ns == 0) {
                rate_anchor_ns = now;
                rate_anchor_n = seq;
            } else {
                const int64_t span = now - rate_anchor_ns;
                const uint64_t dn = seq - rate_anchor_n;
                if (span > 1000000000LL && dn > 0) {
                    produced_hz = double(dn) * 1e9 / double(span);
                    if (span > 30000000000LL) {
                        rate_anchor_ns = now;
                        rate_anchor_n = seq;
                    }
                }
            }
        }

        {
            struct timespec ts2;
            clock_gettime(CLOCK_MONOTONIC, &ts2);
            const double out_us = ts2.tv_sec * 1e6 + ts2.tv_nsec / 1e3;
            std::lock_guard<std::mutex> lk2(in_mtx);
            if (!in_times.empty()) {
                const double dt = (out_us - in_times.front()) / 1000.0;
                in_times.pop_front();
                if (dec_n == 0 || dt < dec_min) dec_min = dt;
                if (dt > dec_max) dec_max = dt;
                dec_sum += dt;
                dec_n++;
            }
        }

        fw.store(f.width, std::memory_order_relaxed);
        fh.store(f.height, std::memory_order_relaxed);
        last_frame_ms.store(now_ms(), std::memory_order_relaxed);

        std::lock_guard<std::mutex> lk(mtx);
        frame->where = place;
        queue.push_back(std::move(frame));
        while (queue.size() > kMaxQueue) {
            queue.pop_front();     // найстаріший помирає -> буфер у пул
            st.dropped++;
        }
        st.produced++;
    }

    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user) {
        GstSample* s = gst_app_sink_pull_sample(sink);
        if (s) static_cast<Impl*>(user)->on_sample(s);
        return GST_FLOW_OK;
    }

    // --- пайплайн ---

    // Збирає опис із гачків нащадка. Сама база про кодеки не знає нічого.
    std::string describe() const {
        std::string s = "udpsrc port=" + std::to_string(cfg.udp_port);

        const std::string c = owner->caps();
        if (!c.empty()) s += " caps=\"" + c + "\"";

        s += " ! " + owner->parse_chain();

        // Без черги перед appsink. Вона стояла з max-size-buffers=1,
        // тобто була постійно повна й постійно текла, а appsink із
        // max-buffers=1 drop=true і так лишає лише найсвіжіший кадр.
        // Два послідовні відкидання там, де досить одного, лише додавали
        // нерівномірності.
        s += " ! " + owner->decoder() + " name=dec"
             " ! appsink name=sink emit-signals=false sync=false max-buffers=1 drop=true";
        return s;
    }

    bool build() {
        // ОДИН ПАЙПЛАЙН, БЕЗ ВАРІАНТІВ. Раніше тут був перебір ланок між
        // парсером і декодером — на випадок, якщо негоціація не складеться.
        // Прибрано з двох причин.
        //
        // По-перше, він не працював саме тоді, коли був потрібен: невдала
        // негоціація стається АСИНХРОННО, коли піде перший буфер, а
        // set_state(PLAYING) на живому udpsrc успішний завжди. Тобто до
        // другого варіанта справа не доходила ніколи, а після помилки на
        // шині сторож перезбирав той самий перший.
        //
        // По-друге, вгадувати нема чого: камера — наша ж прошивка, потік
        // сталий. Формат на стику пиниться явно в parse_chain() нащадка.
        const std::string desc = describe();

        GError* err = nullptr;
        GstElement* p = gst_parse_launch(desc.c_str(), &err);
        if (!p || err) {
            std::fprintf(stderr, "[%s] пайплайн не зібрався: %s\n",
                         name.c_str(), err ? err->message : "?");
            if (err) g_error_free(err);
            if (p) gst_object_unref(p);
            return false;
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(p), "sink");
        if (!sink) { gst_object_unref(p); return false; }

        GstAppSinkCallbacks cb{};
        cb.new_sample = &Impl::on_new_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, this, nullptr);

        if (gst_element_set_state(p, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "[%s] пайплайн не пішов у PLAYING\n", name.c_str());
            gst_element_set_state(p, GST_STATE_NULL);
            gst_object_unref(sink);
            gst_object_unref(p);
            return false;
        }

        // Проба на ВХОДІ декодера: інтервали тут проти інтервалів на
        // виході покажуть, хто саме створює парність.
        if (GstElement* dec = gst_bin_get_by_name(GST_BIN(p), "dec")) {
            if (GstPad* sp = gst_element_get_static_pad(dec, "sink")) {
                gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER,
                                  &Impl::on_dec_input, this, nullptr);
                gst_object_unref(sp);
            }
            gst_object_unref(dec);
        }

        pipeline = p;
        appsink = sink;
        bus = gst_element_get_bus(p);
        std::fprintf(stderr, "[%s] порт %d, пайплайн піднято\n",
                     name.c_str(), cfg.udp_port);
        return true;
    }

    void teardown() {
        if (bus) { gst_object_unref(bus); bus = nullptr; }
        if (appsink) { gst_object_unref(appsink); appsink = nullptr; }
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        std::lock_guard<std::mutex> lk(mtx);
        queue.clear();
        in_use.reset();
    }

    // Шину треба зливати ПОВНІСТЮ, а не лише ERROR: bus-watch у нас
    // немає, тож невичитані повідомлення лишалися б у черзі назавжди. На
    // битому лінку декодер сипле WARNING на кожен пробій, і за політ це
    // помітно росло б у пам'яті.
    bool drain_bus_has_error() {
        if (!bus) return false;
        bool err = false;
        while (GstMessage* m = gst_bus_pop(bus)) {
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(m, &e, &dbg);
                std::fprintf(stderr, "[%s] помилка від %s: %s\n", name.c_str(),
                             GST_OBJECT_NAME(m->src), e ? e->message : "?");
                if (e) g_error_free(e);
                if (dbg) g_free(dbg);
                err = true;
            }
            gst_message_unref(m);
        }
        return err;
    }

    // Перезбирання пайплайна. Дві причини, і кожна своя.
    void restart(const char* why) {
        std::fprintf(stderr, "[%s] перезбираю пайплайн: %s\n", name.c_str(), why);
        teardown();
        last_input_ms.store(0, std::memory_order_relaxed);
        last_frame_ms.store(0, std::memory_order_relaxed);

        if (build()) {
            fail_streak = 0;
        } else {
            // Прогресивна пауза. При СТАЛІЙ помилці — немає елемента,
            // зайнятий порт — перезбирання раз на 200 мс залило б лог і
            // не дало б нічого. Крок 1, 2, 4, 8 с, стеля 8.
            if (fail_streak < 4) fail_streak++;
            const int wait_s = 1 << (fail_streak - 1);
            std::this_thread::sleep_for(std::chrono::seconds(wait_s));
        }
        last_restart_ms = now_ms();
    }

    void watchdog_loop() {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!running.load(std::memory_order_relaxed)) break;

            // ПРИЧИНА ПЕРША: елемент поскаржився. Однозначна, реагуємо
            // одразу.
            if (drain_bus_has_error()) {
                restart("помилка на шині");
                continue;
            }

            // ПРИЧИНА ДРУГА: дані в декодер ІДУТЬ, а кадрів на виході
            // немає. Це і є "декодер завис" або "негоціація не склалася":
            // пайплайн у PLAYING, елементи здорові, на шині тиша.
            //
            // ЧОМУ САМЕ ТАКА УМОВА, А НЕ ПРОСТО "кадрів немає". Якщо
            // камера мовчить, перезбирати НЕМА ЧОГО: udpsrc підхопить
            // потік сам, щойно той з'явиться, і перезапуск був би
            // марною роботою з ризиком. Відрізняє їх проба на вході
            // декодера: буфери туди заходять — значить дані є.
            const int64_t now = now_ms();
            const int64_t in = last_input_ms.load(std::memory_order_relaxed);
            const int64_t out = last_frame_ms.load(std::memory_order_relaxed);

            const bool data_flows = in != 0 && (now - in) < 1000;
            const bool no_frames = out == 0 || (now - out) > cfg.restart_after_ms;
            const bool cooled = (now - last_restart_ms) > cfg.restart_after_ms;

            if (data_flows && no_frames && cooled) {
                restart("дані йдуть, а кадрів немає");
            }
        }
    }

    bool signal_ok() const {
        const int64_t last = last_frame_ms.load(std::memory_order_relaxed);
        return last != 0 && (now_ms() - last) < cfg.signal_timeout_ms;
    }
};

// ---------------------------------------------------------------------

VideoSource::VideoSource(std::string name, Config cfg)
    : impl_(std::make_unique<Impl>(std::move(name), cfg)) {
    impl_->owner = this;
}

VideoSource::~VideoSource() { stop(); }

const char* VideoSource::name() const { return impl_->name.c_str(); }

const VideoSource::Config& VideoSource::config() const { return impl_->cfg; }

bool VideoSource::start() {
    Impl& d = *impl_;
    if (d.running.load()) return true;
    if (!d.build()) return false;
    d.running.store(true);
    d.watchdog = std::thread([&d] { d.watchdog_loop(); });
    return true;
}

void VideoSource::stop() {
    Impl& d = *impl_;
    if (d.running.exchange(false) && d.watchdog.joinable()) d.watchdog.join();
    d.teardown();
}

bool VideoSource::acquire(SourceFrame& out) {
    Impl& d = *impl_;

    // Єдина причина відповісти "показувати нічого": кадрів немає вже
    // довше за signal_timeout_ms. Окремі пропущені кадри сюди НЕ
    // потрапляють — вони норма, і нижче ми просто віддамо останній
    // валідний, ніби нічого не сталося.
    if (!d.signal_ok()) return false;

    std::lock_guard<std::mutex> lk(d.mtx);

    // Беремо НАЙСТАРІШИЙ із черги — щоб кадри йшли в тому порядку, в
    // якому їх зняли. Звільнення НЕЯВНЕ: попередній in_use помирає саме
    // тут, його keepalive відпускає GstSample, і буфер повертається в
    // пул декодера.
    // АКТИВНЕ ЗЛИВАННЯ. Без нього черга, раз набравшись (на старті або
    // на затримці рендера), лишається повною назавжди: джерело і показ
    // майже рівні за темпом, і розсмоктатись їй нема з чого. Тут ми
    // щоразу зрізаємо надлишок з ГОЛОВИ, лишаючи рівно target_queue
    // кадрів запасу після забору.
    const size_t keep = (size_t)(d.cfg.target_queue < 0 ? 0 : d.cfg.target_queue);
    while (d.queue.size() > keep + 1) {
        d.queue.pop_front();      // найстаріший помирає -> буфер у пул
        d.st.dropped++;
    }

    d.q_sum += d.queue.size();
    d.q_n++;
    if ((int)d.queue.size() > d.q_max) d.q_max = (int)d.queue.size();

    if (!d.queue.empty()) {
        d.in_use = std::move(d.queue.front());
        d.queue.pop_front();
        d.st.taken++;
    } else {
        d.st.reused++;
    }

    // Нового немає — віддаємо ПОПЕРЕДНІЙ, а не false.
    //
    // Питання рендерера — "що показувати зараз", а не "чи є щось свіже".
    // Джерело на 30 к/с проти показу на 60 Гц має новий кадр лише через
    // раз; якби на друге звернення ми відповідали false, воно зникало б
    // з екрана кожен другий показ. Саме так і було: мигання рівно на
    // 30 Гц, і по лічильниках його не видно взагалі.
    //
    // false тут означає РІВНО одне: сигналу немає (перевірено вище).
    if (!d.in_use) return false;

    out = *d.in_use;
    out.where = d.place;
    return true;
}

layout::Placement VideoSource::placement() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->place;
}

void VideoSource::set_placement(const layout::Placement& p) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->place = p;
}

SourceStats VideoSource::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    SourceStats s = impl_->st;
    s.interval_min_ms = impl_->iv_min;
    s.interval_max_ms = impl_->iv_max;
    s.interval_avg_ms = impl_->iv_n ? impl_->iv_sum / impl_->iv_n : 0.0;
    s.queue_avg = impl_->q_n ? double(impl_->q_sum) / impl_->q_n : 0.0;
    s.queue_max = impl_->q_max;
    s.produced_hz = impl_->produced_hz;
    return s;
}

void VideoSource::input_intervals(double* mn, double* avg, double* mx) const {
    std::lock_guard<std::mutex> lk(impl_->in_mtx);
    *mn = impl_->in_min;
    *avg = impl_->in_n ? impl_->in_sum / impl_->in_n : 0.0;
    *mx = impl_->in_max;
}

void VideoSource::decode_latency(double* mn, double* avg, double* mx) const {
    std::lock_guard<std::mutex> lk(impl_->in_mtx);
    *mn = impl_->dec_min;
    *avg = impl_->dec_n ? impl_->dec_sum / impl_->dec_n : 0.0;
    *mx = impl_->dec_max;
}

bool VideoSource::has_signal() const { return impl_->signal_ok(); }
int VideoSource::frame_width() const { return impl_->fw.load(std::memory_order_relaxed); }
int VideoSource::frame_height() const { return impl_->fh.load(std::memory_order_relaxed); }

} // namespace vrx::source
