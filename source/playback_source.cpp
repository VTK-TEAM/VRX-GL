#include "source/playback_source.hpp"
#include "source/mkv_feeder.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include <drm_fourcc.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

namespace vrx::source {
namespace {

uint32_t drm_fourcc_of(GstVideoFormat f) {
    switch (f) {
        case GST_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
        case GST_VIDEO_FORMAT_NV16: return DRM_FORMAT_NV16;
        case GST_VIDEO_FORMAT_I420: return DRM_FORMAT_YUV420;
        case GST_VIDEO_FORMAT_YUY2: return DRM_FORMAT_YUYV;
        default: return 0;
    }
}

int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

} // namespace

struct PlaybackSource::Impl {
    Config cfg;
    std::string name;

    // Опис сеансу тримаємо СВОЮ КОПІЮ: плеєр живе довго, а той, хто його
    // відкрив, може піти раніше.
    std::string cur_file;        // який файл зараз програється

    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;

    MkvFeeder feeder;
    std::thread pump;                 // подає байти в appsrc
    std::atomic<bool> running{false};

    struct Item {
        std::shared_ptr<SourceFrame> frame;
        int64_t pts_us = 0;
    };

    mutable std::mutex mtx;
    std::condition_variable room;   // місце звільнилось у черзі
    std::deque<Item> queue;
    Item current;                     // що показуємо зараз

    // Ціль у часі СЕАНСУ. Кадр із черги стає поточним, поки його власний
    // час не перевищує цілі.
    std::atomic<int64_t> target_us{-1};
    std::atomic<float> aspect{0.f};      // пропорція останнього кадру

    // ЯКІР І ПОЧАТОК СЕАНСУ — АТОМАРНІ.
    //
    // Їх пише перемотка (потік годинника), а читає показ, переводячи PTS
    // кадру в час сеансу. Детектор гонок ловить це першим і не дарма:
    // зсунутий якір означає кадр, покладений не на ту секунду.
    std::atomic<int64_t> anchor_us{0};
    std::atomic<int64_t> session_start_us{0};

    // Час кадру в шкалі сеансу. Якір — момент, якому відповідає PTS=0 у
    // цьому файлі; звідси різні файли одного каналу лягають на спільну
    // вісь без жодних поправок.
    int64_t session_time(int64_t pts_us) const {
        return anchor_us.load(std::memory_order_relaxed) + pts_us -
               session_start_us.load(std::memory_order_relaxed);
    }

    SourceStats st;

    ~Impl() { teardown(); }

    void teardown() {
        running.store(false);
        room.notify_all();          // випустити подавача з чекання
        if (appsrc) gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
        if (pump.joinable()) pump.join();
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
        if (appsrc) { gst_object_unref(appsrc); appsrc = nullptr; }
        if (appsink) { gst_object_unref(appsink); appsink = nullptr; }
        feeder.close();

        std::deque<Item> dying;
        {
            std::lock_guard<std::mutex> lk(mtx);
            dying.swap(queue);
            current = Item{};
        }
        // dying помирає ПОЗА локом: смерть кадру — це повернення буфера в
        // пул декодера, і скільки воно триває, вирішує не наш код.
    }

    static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user) {
        static_cast<Impl*>(user)->take(gst_app_sink_pull_sample(sink));
        return GST_FLOW_OK;
    }

    void take(GstSample* sample) {
        if (!sample) return;
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        GstVideoInfo vi;
        if (!buf || !caps || !gst_video_info_from_caps(&vi, caps)) {
            gst_sample_unref(sample);
            return;
        }
        GstMemory* mem = gst_buffer_peek_memory(buf, 0);
        if (!mem || !gst_is_dmabuf_memory(mem)) {
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "[%s] декодер віддає НЕ dmabuf\n", name.c_str());
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

        GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
        const int n = vm ? (int)vm->n_planes : (int)GST_VIDEO_INFO_N_PLANES(&vi);
        f.n_planes = n > display::Frame::kMaxPlanes ? display::Frame::kMaxPlanes : n;
        const int fd = gst_dmabuf_memory_get_fd(mem);
        for (int i = 0; i < f.n_planes; ++i) {
            f.fd[i] = fd;
            f.stride[i] = vm ? (uint32_t)vm->stride[i]
                             : (uint32_t)GST_VIDEO_INFO_PLANE_STRIDE(&vi, i);
            f.offset[i] = vm ? (uint32_t)vm->offset[i]
                             : (uint32_t)GST_VIDEO_INFO_PLANE_OFFSET(&vi, i);
        }
        f.keepalive = std::shared_ptr<void>(sample, [](void* p) {
            gst_sample_unref(static_cast<GstSample*>(p));
        });
        frame->pixel_aspect = GST_VIDEO_INFO_PAR_D(&vi) > 0
            ? float(GST_VIDEO_INFO_PAR_N(&vi)) / float(GST_VIDEO_INFO_PAR_D(&vi))
            : 1.0f;
        frame->produced_ns = mono_ns();

        aspect.store(frame->aspect(), std::memory_order_relaxed);

        const int64_t pts = GST_BUFFER_PTS_IS_VALID(buf)
                                ? (int64_t)GST_BUFFER_PTS(buf) / 1000 : 0;

        // ЧЕКАЄМО МІСЦЯ, А НЕ ВИКИДАЄМО.
        //
        // Перша спроба викидала з голови переповнену чергу — і темп поїхав
        // геть: декодер швидший за показ у двадцять сім разів, тож ніщо
        // його не гальмувало, і за шістдесят заборів позиція проскакувала
        // двадцять вісім секунд замість двох.
        //
        // Чекання тут — це і є гальмо. Воно тримає потік GStreamer, той
        // перестає забирати з appsink, і затримка доходить аж до
        // блокуючого push у appsrc. Швидкість показу починає визначати
        // рівно те, що й має, — з якою частотою рендерер забирає кадри.
        std::unique_lock<std::mutex> lk(mtx);
        room.wait(lk, [this] {
            return !running.load(std::memory_order_relaxed) ||
                   (int)queue.size() < cfg.queue_depth;
        });
        if (!running.load(std::memory_order_relaxed)) return;
        queue.push_back({frame, pts});
        st.produced++;
    }

    // Подавач байтів. Блокуючий push в appsrc сам себе гальмує, коли
    // черга наповнилась, — окремого регулювання темпу не потрібно.
    void pump_loop() {
        std::vector<uint8_t> buf(256 * 1024);
        while (running.load(std::memory_order_relaxed)) {
            const size_t n = feeder.read(buf.data(), buf.size());
            if (n == 0) {
                // ДЛЯ ЖИВОГО ЗАПИСУ НУЛЬ — НЕ КІНЕЦЬ, а "поки що все".
                //
                // Раніше подача тут завершувалась назавжди: відео завмирало
                // на останньому кадрі, хоч журнал за дві секунди додавав
                // нових даних, а таймлайн бачив їх і біг далі. Виглядало
                // так, ніби завис показ, а насправді нікуди було вливати.
                if (!feeder.growing()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                continue;
            }
            GstBuffer* b = gst_buffer_new_allocate(nullptr, n, nullptr);
            gst_buffer_fill(b, 0, buf.data(), n);
            if (gst_app_src_push_buffer(GST_APP_SRC(appsrc), b) != GST_FLOW_OK) break;
        }
        if (appsrc) gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    }

    bool build(const std::string& path, int64_t byte_off, int64_t limit) {
        teardown();
        if (!feeder.open(path)) return false;
        feeder.set_limit(limit);
        if (byte_off > 0 && !feeder.seek(byte_off)) return false;

        const char* parse = cfg.codec == Codec::H265 ? "h265parse ! mppvideodec"
                                                     : "jpegparse ! mppjpegdec";
        char desc[512];
        std::snprintf(desc, sizeof(desc),
            "appsrc name=src format=bytes block=true max-bytes=4194304 "
            "caps=video/x-matroska ! matroskademux ! %s "
            "! appsink name=sink emit-signals=false sync=false "
            "max-buffers=2 drop=false", parse);

        GError* err = nullptr;
        pipeline = gst_parse_launch(desc, &err);
        if (!pipeline) {
            std::fprintf(stderr, "[%s] пайплайн не зібрався: %s\n", name.c_str(),
                         err && err->message ? err->message : "?");
            if (err) g_error_free(err);
            return false;
        }
        appsrc  = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        if (!appsrc || !appsink) return false;

        GstAppSinkCallbacks cb {};
        cb.new_sample = &Impl::on_new_sample;
        gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &cb, this, nullptr);

        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
            return false;

        running.store(true);
        pump = std::thread([this] { pump_loop(); });
        return true;
    }
};

PlaybackSource::PlaybackSource(Config cfg)
    : impl_(std::make_unique<Impl>()), name_("плеєр:" + cfg.channel) {
    impl_->cfg = std::move(cfg);
    impl_->name = name_;
}
PlaybackSource::~PlaybackSource() = default;

bool PlaybackSource::open(const record::SessionIndex& ix, int64_t t_us) {
    return seek(ix, t_us);
}

void PlaybackSource::refresh(const record::SessionIndex& ix) {
    Impl& d = *impl_;
    if (d.cur_file.empty()) return;
    for (const auto& f : ix.files())
        if (f.name == d.cur_file) {
            d.feeder.set_limit(f.closed ? -1 : f.safe_bytes);
            return;
        }
}

bool PlaybackSource::seek(const record::SessionIndex& ix, int64_t t_us) {
    Impl& d = *impl_;
    const auto sp = ix.locate(d.cfg.channel, t_us);
    if (!sp.valid) return false;

    // Межа читання: для сеансу, що ще пишеться, далі останньої мітки
    // дані можуть не дійти до носія.
    int64_t limit = -1, anchor = 0;
    for (const auto& f : ix.files())
        if (f.name == sp.name) {
            anchor = f.anchor_us;
            if (!f.closed) limit = f.safe_bytes;
        }
    d.anchor_us.store(anchor, std::memory_order_relaxed);
    d.session_start_us.store(ix.start_us(), std::memory_order_relaxed);
    d.cur_file = sp.name;
    d.target_us.store(t_us, std::memory_order_relaxed);
    return d.build(ix.dir() + "/" + sp.name, sp.byte_off, limit);
}

bool PlaybackSource::start() { return true; }       // піднімає open/seek
void PlaybackSource::stop()  { impl_->teardown(); }

void PlaybackSource::set_target(int64_t session_us) {
    impl_->target_us.store(session_us, std::memory_order_relaxed);
}

int64_t PlaybackSource::next_after(int64_t session_us) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (const auto& it : impl_->queue) {
        const int64_t t = impl_->session_time(it.pts_us);
        if (t > session_us) return t;
    }
    return 0;
}

bool PlaybackSource::acquire(SourceFrame& out) {
    Impl& d = *impl_;

    std::vector<std::shared_ptr<SourceFrame>> dying;
    {
        std::lock_guard<std::mutex> lk(d.mtx);

        const int64_t target = d.target_us.load(std::memory_order_relaxed);

        // Знімаємо все, що вже настало, лишаючи ОСТАННІЙ такий кадр.
        // Проміжні при швидкості вище одиниці глядач і так не побачив би.
        while (!d.queue.empty() && d.session_time(d.queue.front().pts_us) <= target) {
            if (d.current.frame) dying.push_back(std::move(d.current.frame));
            d.current = d.queue.front();
            d.queue.pop_front();
            d.st.taken++;
        }
        if (!dying.empty()) d.room.notify_one();

        if (!d.current.frame) return false;
        out = *d.current.frame;
        d.st.reused++;
    }
    return true;                            // dying вмирає поза локом
}

bool PlaybackSource::snapshot(SourceFrame& out) const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (!impl_->current.frame) return false;
    out = *impl_->current.frame;
    return true;
}

float PlaybackSource::frame_aspect() const {
    return impl_->aspect.load(std::memory_order_relaxed);
}

bool PlaybackSource::has_signal() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->current.frame != nullptr || !impl_->queue.empty();
}

SourceStats PlaybackSource::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

int64_t PlaybackSource::position_us() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (!impl_->current.frame) return 0;
    return impl_->session_time(impl_->current.pts_us);
}

} // namespace vrx::source
