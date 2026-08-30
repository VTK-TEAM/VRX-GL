#include "video_source.hpp"

#include "../diag/path_meter.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>

#include <drm_fourcc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <vector>
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
    SourceStats st{};

    // Черга готових кадрів замість одного слота.
    //
    // ЩО ВОНА РОБИТЬ НАСПРАВДІ — не те, заради чого писалася. Початковий
    // задум був такий: mppvideodec регулярно віддає два кадри майже
    // одночасно (заміряно на борті: інтервал на виході має min 0.2 мс
    // при середніх 16.7, тоді як на мережі min 2.5 мс), і з одним слотом
    // другий кадр пари витісняв перший. Черга мала зберегти обидва й
    // віддати по одному на розгортку.
    //
    // Так не виходить, і це нормально. При target_queue = 0 зливання в
    // acquire() зрізає чергу до одного кадру ПЕРЕД забором, тобто ми
    // щоразу показуємо НАЙСВІЖІШИЙ, а старший кадр пари йде в dropped.
    // Показувати його й не треба: петля фази тримає камеру й розгортку
    // синхронно, кадри приходять по одному на період, а пара — або
    // рідкісний викид, або взагалі артефакт того, як декодер ставить
    // мітки часу.
    //
    // Тож черга — це запас на випадок, коли рендерер став: щоб приймання
    // не заглухло й лічильники показали, скільки саме кадрів через це
    // втрачено. Глибини 3 (два робочих плюс один, якщо декодер таки
    // віддасть пару підряд) для цього досить із запасом.
    //
    // Більше було б не корисно, а шкідливо: кожен слот утримує буфер
    // ЧУЖОГО пулу, а при пробудженні рендерера все, що понад один кадр,
    // однаково буде зрізано не показаним. Заміряно з рендерером на
    // 59 Гц: черга в роботі стоїть на 0.93 при максимумі 1.
    static constexpr size_t kMaxQueue = 3;
    std::deque<std::shared_ptr<SourceFrame>> queue;
    std::shared_ptr<SourceFrame> in_use;

    std::atomic<int64_t> last_frame_ms{0};

    // Замір рівномірності приходу — щоб відрізнити "декодер віддає
    // пачками" від "ми самі губимо кадри".
    double prev_arrival_us = 0;
    double iv_min = 0, iv_max = 0, iv_sum = 0;
    // Оцінка періоду приходу — медіана останніх інтервалів. Стійка
    // одразу до двох хвостів: пар від декодера (короткі) і дір у лінку
    // (довгі). Див. облік втрачених кадрів в on_sample.
    static constexpr int kIvRing = 64;
    double iv_ring[kIvRing] = {};
    int iv_ring_n = 0, iv_ring_head = 0;
    double nominal_ms = 0;
    double credit_ms = 0;
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

        VRX_PM_IN(d->pm_ch, (int64_t)(us * 1000.0));

        d->in_times.push_back(us);
        // Якщо декодер щось проковтнув, черга міток поповзе — тоді
        // зіставлення втратить сенс, тож обмежуємо й скидаємо надлишок.
        while (d->in_times.size() > 8) d->in_times.pop_front();
        return GST_PAD_PROBE_OK;
    }
    std::atomic<int> fw{0}, fh{0};

    // ПЕРЕКРИТТЯ ПОРТУ НА ХОДУ. -1 = діє cfg.udp_port. Інакше пайплайн
    // слухає цей порт. Використовується ліцензійним саботажем: перемкнути
    // на "лівий" порт — і кадри перестають надходити, картинка застигає на
    // останньому, потім повернути назад. want_rebuild просить сторожа
    // перезібрати пайплайн чисто, без backoff (це навмисна дія, не збій).
    std::atomic<int> port_override{-1};
    std::atomic<bool> want_rebuild{false};

    // Номер каналу в наскрізному вимірі. -1, коли вимір вимкнено.
    int pm_ch = -1;

    std::thread watchdog;

    // Пауза сторожа переривається зупинкою, а не пересиджується.
    // Прогресивний backoff доходить до 8 секунд, і на sleep_for це
    // означало б, що stop() (а він join'ить сторожа) висить рівно
    // стільки ж — Ctrl-C посеред паузи гальмував би вихід на 8 с.
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    // Коли востаннє буфер зайшов У ДЕКОДЕР. Не те саме, що last_frame_ms:
    // той про ВИХІД. Різниця між ними й відрізняє "камера мовчить" від
    // "декодер завис".
    std::atomic<int64_t> last_input_ms{0};

    // Коли зібрано ПОТОЧНИЙ пайплайн. Нуль означає, що його немає.
    //
    // Це не те саме, що "коли востаннє перезбирали": відлік прив'язаний
    // до життя конкретного пайплайна, і саме тому він природно дає
    // фору на старті — новозібраному декодеру треба дочекатись першого
    // IDR, і до того кадрів на виході законно немає.
    int64_t built_at_ms = 0;

    // Скільки перезбирань поспіль не дали здорового пайплайна.
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

        // ОДНА мітка часу на весь хвіст. Раніше тут стояло три
        // послідовні clock_gettime() — для інтервалів, для produced_ns і
        // для затримки декоду. Вони давали трохи різний час, тобто три
        // заміри одного й того ж кадру не сходились між собою.
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t at_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
        const double at_us = at_ns / 1e3;
        frame->produced_ns = at_ns;

        fw.store(f.width, std::memory_order_relaxed);
        fh.store(f.height, std::memory_order_relaxed);
        last_frame_ms.store(now_ms(), std::memory_order_relaxed);

        // Затримка чистого декоду: вхід зіставляється з виходом за FIFO.
        {
            double matched_in_us = 0;
            {
                std::lock_guard<std::mutex> lk2(in_mtx);
                if (!in_times.empty()) {
                    matched_in_us = in_times.front();
                    const double dt = (at_us - matched_in_us) / 1000.0;
                    in_times.pop_front();
                    if (dec_n == 0 || dt < dec_min) dec_min = dt;
                    if (dt > dec_max) dec_max = dt;
                    dec_sum += dt;
                    dec_n++;
                }
            }
            VRX_PM_OUT(pm_ch, at_ns, (int64_t)(matched_in_us * 1000.0));
        }

        // ВСЯ решта статистики — під тим самим локом, що й черга.
        //
        // Раніше інтервали приходу й produced_hz рахувалися вище, поза
        // локом, а stats() читав їх під локом. Тобто ФАПЧ, локальні
        // канали ОСД і друк статусу читали ці поля просто конкурентно з
        // записом. Найпомітніший наслідок — неузгоджена пара sum/n, з
        // якої виходить середній інтервал, якого не було.
        std::lock_guard<std::mutex> lk(mtx);

        if (prev_arrival_us > 0) {
            const double dt = (at_us - prev_arrival_us) / 1000.0;
            if (iv_n == 0 || dt < iv_min) iv_min = dt;
            if (dt > iv_max) iv_max = dt;
            iv_sum += dt;
            iv_n++;

            // --- КАДРИ, ЯКІ МАЛИ ПРИЙТИ Й НЕ ПРИЙШЛИ ---
            //
            // Щоб порахувати діру в кадрах, треба знати період. Брати
            // produced_hz не можна: вона рахується як кадри поділити на
            // час, тобто САМА просідає від втрат, і чим більше кадрів
            // губиться, тим "правильнішою" виглядає діра.
            //
            // ПЕРІОД — МЕДІАНА останніх інтервалів, і це не стиль, а
            // єдиний спосіб, що витримує обидва хвости одразу.
            //
            // Спершу тут стояла ЕМА, яку я підтягував донизу швидше, ніж
            // угору: міркування було, що прийти раніше за період кадр не
            // може, отже короткий інтервал означає завищену оцінку.
            // Міркування хибне. Декодер регулярно віддає кадри ПАРАМИ —
            // заміряно на цьому ж лінку: мінімум інтервалу 4.6 мс при
            // середніх 16.9. Кожна пара тягнула оцінку вниз, а щойно вона
            // осідала нижче ~11 мс, ЗВИЧАЙНИЙ інтервал ставав "більшим за
            // півтора періоди", і втратою рахувався кожен нормальний
            // кадр. Лічильник ріс рівно й невпинно при бездоганному fps.
            //
            // Медіана байдужа до обох хвостів, поки їх менше половини:
            // пари тягнуть униз, діри вгору, середина стоїть.
            iv_ring[iv_ring_head] = dt;
            iv_ring_head = (iv_ring_head + 1) % kIvRing;
            if (iv_ring_n < kIvRing) iv_ring_n++;
            if (iv_ring_n >= 16 && (iv_ring_head % 16) == 0) {
                double tmp[kIvRing];
                std::copy(iv_ring, iv_ring + iv_ring_n, tmp);
                std::nth_element(tmp, tmp + iv_ring_n / 2, tmp + iv_ring_n);
                nominal_ms = tmp[iv_ring_n / 2];
            }

            // КРЕДИТ ЗА КОРОТКИЙ ІНТЕРВАЛ. Друга половина тієї ж пари:
            // якщо кадр прийшов удвічі раніше, наступний законно прийде
            // пізніше, і сам по собі довгий інтервал тут нічого не
            // втратив. Без цієї поправки кожна пара 4 + 29 мс давала б
            // фальшиву втрату.
            if (nominal_ms > 0.0 && dt < nominal_ms * 0.6) {
                credit_ms += nominal_ms - dt;
                if (credit_ms > nominal_ms) credit_ms = nominal_ms;
            }

            // Поріг 1.75, а не 1.5: у чистому прогоні найдовший інтервал
            // приходу — 24.9 мс при періоді 16.97, тобто 1.47. Півтора
            // періоди лежать усередині нормального джитера, і різати
            // треба вище за нього.
            //
            // Діра, довша за таймаут сигналу, — це вже не втрачені кадри,
            // а втрачений сигнал: він показується окремо, нулем частоти.
            if (nominal_ms > 0.0 && iv_ring_n >= 16 &&
                dt < (double)cfg.signal_timeout_ms) {
                const double eff = dt - credit_ms;
                if (eff > nominal_ms * 1.75) {
                    const long n = std::lround(eff / nominal_ms) - 1;
                    if (n > 0) {
                        st.missing += (uint64_t)n;
                        credit_ms = 0.0;
                    }
                }
            }
        }
        prev_arrival_us = at_us;

        // Частота по лічильнику на вікні 1..30 с. Джитер приходу (±5 мс)
        // ділиться на довжину вікна: за 10 с це вже краще за 2 мГц,
        // тобто тонше за крок підстроювання камери.
        {
            const uint64_t seq = st.produced;          // ще не інкрементований
            if (rate_anchor_ns == 0) {
                rate_anchor_ns = at_ns;
                rate_anchor_n = seq;
            } else {
                const int64_t span = at_ns - rate_anchor_ns;
                const uint64_t dn = seq - rate_anchor_n;
                if (span > 1000000000LL && dn > 0) {
                    produced_hz = double(dn) * 1e9 / double(span);
                    if (span > 30000000000LL) {
                        rate_anchor_ns = at_ns;
                        rate_anchor_n = seq;
                    }
                }
            }
        }

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
        std::string s = owner->source_element();
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
        built_at_ms = now_ms();
        std::fprintf(stderr, "[%s] порт %d, пайплайн піднято\n",
                     name.c_str(), cfg.udp_port);
        return true;
    }

    void teardown() {
        built_at_ms = 0;
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

    void restart(const char* why) {
        // ЧИ ДОПОМОГЛО ПОПЕРЕДНЄ ПЕРЕЗБИРАННЯ. Ознака здорового
        // пайплайна одна: він прожив довше за restart_after_ms і встиг
        // віддати хоч кадр. Тоді нинішній збій — разовий, і реагувати на
        // нього треба одразу, без пауз.
        //
        // Раніше прогресивна пауза рахувала ЛИШЕ невдалі build(), і тому
        // не спрацьовувала в найгіршому випадку: пайплайн збирається
        // успішно й одразу падає з помилкою на шині. fail_streak при
        // цьому обнулявся, кулдауна на цій гілці не було — і сторож
        // перезбирав п'ять разів на секунду, заливаючи лог.
        const bool healthy = built_at_ms != 0 &&
                             (now_ms() - built_at_ms) >= cfg.restart_after_ms &&
                             last_frame_ms.load(std::memory_order_relaxed) != 0;
        if (healthy) fail_streak = 0;
        else if (fail_streak < 4) fail_streak++;

        if (fail_streak > 1) {
            std::fprintf(stderr, "[%s] перезбираю пайплайн: %s (%d-те поспіль, пауза %d с)\n",
                         name.c_str(), why, fail_streak, 1 << (fail_streak - 1));
        } else {
            std::fprintf(stderr, "[%s] перезбираю пайплайн: %s\n", name.c_str(), why);
        }

        teardown();
        last_input_ms.store(0, std::memory_order_relaxed);
        last_frame_ms.store(0, std::memory_order_relaxed);

        build();

        // Прогресивна пауза 1, 2, 4, 8 с — стеля 8. Тримає темп
        // перезбирань при СТАЛОМУ збої, байдуже якої природи: немає
        // елемента, зайнятий порт, потік не той. Пайплайн, якщо він
        // зібрався, живе й працює всю паузу — ми лише не смикаємо його
        // повторно.
        if (fail_streak > 0) nap(1000 << (fail_streak - 1));
    }

    void watchdog_loop() {
        while (nap(200)) {
            // ЗОВНІШНЯ ЗМІНА ПОРТУ. Не збій, а навмисна дія — перезбираємо
            // чисто й одразу, без fail_streak і без backoff, щоб перемикання
            // тримало заданий темп.
            if (want_rebuild.exchange(false, std::memory_order_relaxed)) {
                teardown();
                last_input_ms.store(0, std::memory_order_relaxed);
                last_frame_ms.store(0, std::memory_order_relaxed);
                build();
                continue;
            }

            // ПРИЧИНА НУЛЬОВА: пайплайна немає взагалі.
            //
            // Так буває лише після невдалого build(). Без цієї гілки
            // канал лишався б мертвим назавжди: проби немає, шини немає,
            // тож жодна з умов нижче спрацювати не може в принципі. А
            // типові причини невдачі — порт ще тримає попередній процес,
            // GStreamer ще не побачив плагін — минають самі за секунди.
            if (!pipeline) {
                restart("пайплайна немає");
                continue;
            }

            // ПРИЧИНА ПЕРША: елемент поскаржився. Однозначна, реагуємо
            // одразу; темп повторів тримає пауза всередині restart().
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

            // ФОРА НОВОМУ ПАЙПЛАЙНУ. Гілка `out == 0` інакше спрацьовує
            // майже миттєво після старту: дані в декодер заходять за
            // ~50 мс, а першого кадру на виході законно немає, поки не
            // прийде IDR — а це до секунди, бо ми під'єднуємось посеред
            // GOP. Раніше замість цього стояв кулдаун від ОСТАННЬОГО
            // ПЕРЕЗБИРАННЯ, а на старті його ще не було: лічильник
            // дорівнював нулю, умова була істинна з першого ж проходу, і
            // станція перезбирала здоровий пайплайн на кожному запуску.
            const bool settled = (now - built_at_ms) > cfg.restart_after_ms;

            if (data_flows && no_frames && settled) {
                restart("дані йдуть, а кадрів немає");
            }
        }
    }

    // Пауза, яку перериває stop(). false = час виходити.
    bool nap(int ms) {
        std::unique_lock<std::mutex> lk(wake_mtx);
        wake_cv.wait_for(lk, std::chrono::milliseconds(ms),
                         [this] { return !running.load(std::memory_order_relaxed); });
        return running.load(std::memory_order_relaxed);
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

void VideoSource::set_udp_port(int port) {
    impl_->port_override.store(port, std::memory_order_relaxed);
    impl_->want_rebuild.store(true, std::memory_order_relaxed);
    impl_->wake_cv.notify_all();     // не чекати кінець поточної паузи сторожа
}

const char* VideoSource::name() const { return impl_->name.c_str(); }

const VideoSource::Config& VideoSource::config() const { return impl_->cfg; }

int VideoSource::effective_udp_port() const {
    const int p = impl_->port_override.load(std::memory_order_relaxed);
    return p < 0 ? impl_->cfg.udp_port : p;
}

// Дефолт: мережеве джерело (udpsrc). Для мультикаст-групи (потік релею
// захвату з loopback) додаємо address + iface=lo.
std::string VideoSource::source_element() const {
    std::string s = "udpsrc";
    if (!impl_->cfg.multicast_addr.empty()) {
        s += " address=" + impl_->cfg.multicast_addr + " multicast-iface=lo";
    }
    s += " port=" + std::to_string(effective_udp_port());
    const std::string c = caps();
    if (!c.empty()) s += " caps=\"" + c + "\"";
    return s;
}

bool VideoSource::start() {
    Impl& d = *impl_;
    if (d.running.load()) return true;

    // СТОРОЖ ПІДНІМАЄТЬСЯ НЕЗАЛЕЖНО ВІД ТОГО, чи зібрався пайплайн з
    // першого разу. Раніше невдача тут лишала канал мертвим до
    // перезапуску програми: start() виходив із false, потік нагляду не
    // стартував, і повторити спробу було нікому — а зайнятий порт чи
    // ще не підхоплений плагін минають самі за секунди.
    d.pm_ch = VRX_PM_CHANNEL(d.name.c_str());

    const bool ok = d.build();
    d.running.store(true);
    d.watchdog = std::thread([&d] { d.watchdog_loop(); });
    if (!ok) {
        std::fprintf(stderr, "[%s] пайплайн не зібрався на старті — сторож повторюватиме\n",
                     d.name.c_str());
    }
    return true;
}

void VideoSource::stop() {
    Impl& d = *impl_;
    if (d.running.exchange(false)) {
        d.wake_cv.notify_all();          // інакше чекали б до 8 с паузи
        if (d.watchdog.joinable()) d.watchdog.join();
    }
    d.teardown();
}

bool VideoSource::acquire(SourceFrame& out) {
    Impl& d = *impl_;

    // Єдина причина відповісти "показувати нічого": кадрів немає вже
    // довше за signal_timeout_ms. Окремі пропущені кадри сюди НЕ
    // потрапляють — вони норма, і нижче ми просто віддамо останній
    // валідний, ніби нічого не сталося.
    if (!d.signal_ok()) return false;

    // КАДРИ, ЩО ТУТ ПОМИРАЮТЬ, ВІДПУСКАЄМО ПОЗА ЛОКОМ.
    //
    // Смерть SourceFrame — це не звільнення пам'яті, а gst_sample_unref:
    // буфер повертається в пул декодера, і скільки це триває, вирішує
    // mppvideodec, а не ми. Раніше воно робилося ПІД локом, у потоці
    // показу — тобто на час повернення буфера блокувався й потік
    // GStreamer, який саме в цей момент кладе в чергу наступний кадр.
    //
    // Дешевий обмін під коротким локом — це прямий контракт acquire()
    // (див. frame_source.hpp): виклик іде з потоку показу, і будь-яке
    // очікування тут коштує vblank'а.
    std::vector<std::shared_ptr<SourceFrame>> dying;
    dying.reserve(4);

    {
        std::lock_guard<std::mutex> lk(d.mtx);

        // АКТИВНЕ ЗЛИВАННЯ. Без нього черга, раз набравшись (на старті або
        // на затримці рендера), лишається повною назавжди: джерело і показ
        // майже рівні за темпом, і розсмоктатись їй нема з чого. Тут ми
        // щоразу зрізаємо надлишок з ГОЛОВИ, лишаючи рівно target_queue
        // кадрів запасу після забору.
        const size_t keep = (size_t)(d.cfg.target_queue < 0 ? 0 : d.cfg.target_queue);
        while (d.queue.size() > keep + 1) {
            dying.push_back(std::move(d.queue.front()));
            d.queue.pop_front();
            d.st.dropped++;
        }

        d.q_sum += d.queue.size();
        d.q_n++;
        if ((int)d.queue.size() > d.q_max) d.q_max = (int)d.queue.size();

        if (!d.queue.empty()) {
            // Беремо НАЙСТАРІШИЙ із черги — щоб кадри йшли в тому
            // порядку, в якому їх зняли. Попередній in_use більше не
            // потрібен: рендерер із ним закінчив ще до цього виклику.
            dying.push_back(std::move(d.in_use));
            d.in_use = std::move(d.queue.front());
            d.queue.pop_front();
            d.st.taken++;
        } else {
            d.st.reused++;
        }

        // Нового немає — віддаємо ПОПЕРЕДНІЙ, а не false.
        //
        // Питання рендерера — "що показувати зараз", а не "чи є щось
        // свіже". Джерело на 30 к/с проти показу на 60 Гц має новий кадр
        // лише через раз; якби на друге звернення ми відповідали false,
        // воно зникало б з екрана кожен другий показ. Саме так і було:
        // мигання рівно на 30 Гц, і по лічильниках його не видно взагалі.
        //
        // false тут означає РІВНО одне: сигналу немає (перевірено вище).
        if (!d.in_use) return false;

        out = *d.in_use;
    }

    // Ось тут помирають старі кадри — лок уже відпущено.
    return true;
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

bool VideoSource::snapshot(SourceFrame& out) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    // Найсвіжіший із черги, а як черга порожня — той, що зараз на екрані.
    if (!impl_->queue.empty()) { out = *impl_->queue.back(); return true; }
    if (impl_->in_use) { out = *impl_->in_use; return true; }
    return false;
}

float VideoSource::frame_aspect() const {
    const int w = impl_->fw.load(std::memory_order_relaxed);
    const int h = impl_->fh.load(std::memory_order_relaxed);
    return (w > 0 && h > 0) ? float(w) / float(h) : 0.f;
}
int VideoSource::frame_width() const { return impl_->fw.load(std::memory_order_relaxed); }
int VideoSource::frame_height() const { return impl_->fh.load(std::memory_order_relaxed); }

} // namespace vrx::source
