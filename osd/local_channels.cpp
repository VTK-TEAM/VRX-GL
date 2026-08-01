#include "local_channels.hpp"

#include "telemetry/vt_telemetry_index.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace vrx::osd {

struct LocalChannels::Impl {
    Config cfg;

    VtTelemetryStorage* storage = nullptr;
    const display::Display* display = nullptr;
    const record::Recorder* rec = nullptr;
    const record::Storage* drive = nullptr;
    std::shared_ptr<source::FrameSource> h265;
    std::shared_ptr<source::FrameSource> mjpeg;

    std::thread th;
    std::atomic<bool> running{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    // Попередні значення лічильників — частота рахується різницею на
    // виміряному проміжку, а не припущеним.
    bool have_prev = false;
    std::chrono::steady_clock::time_point prev_at{};
    uint64_t prev_shown = 0;
    uint64_t prev_presented = 0;

    // ЗГЛАДЖУВАННЯ. За 0.5 с при 59 к/с у вікно потрапляє ~30 кадрів, і
    // похибка в один-два кадри це вже ±4 к/с на екрані. Цифра стрибала б
    // 50..60 навіть на рівному потоці.
    //
    // Тут ЕМА, а не довше вікно: довше вікно робить цифру млявою на
    // реальні провали, а ЕМА з постійною ~2.5 с прибирає саме
    // дискретизаційний шум, лишаючи справжні просідання видимими.
    //
    // Це не те саме, що produced_hz у джерела: там частота міряється по
    // лічильнику на довгому вікні й потрібна для ФАПЧ, тобто точність
    // важливіша за швидкість реакції. Тут навпаки — це показання для ока.
    double ema_shown = 0.0, ema_display = 0.0;
    bool ema_valid = false;

    explicit Impl(Config c) : cfg(std::move(c)) {}

    void tick() {
        const auto now = std::chrono::steady_clock::now();

        // --- 200: стан запису ---
        //
        // Три стани, а не два: "носія немає" і "носій є, але не пишемо" —
        // різні речі, і пілоту треба бачити саме яка. Значок у конфізі
        // розрізняє їх через ENUM_SWITCH.
        {
            const auto ds = drive->state();
            int state = 2;
            if (!ds.usable()) state = 0;
            else if (rec->stats().active) state = 1;
            storage->set_value(VT_TLM_LOCAL_RECORDING_STATE, (float)state);
        }

        // --- 201: наскрізні втрати в лінії ---
        //
        // Рахуємо ЛИШЕ коли обидва кінці свіжі. Інакше канал просто не
        // оновлюється й протухає сам — це чесніше, ніж показувати
        // різницю з застарілим доданком: втрати "застигли б" на
        // правдоподібному числі, і по ньому не було б видно, що зв'язку
        // вже немає.
        {
            float tx = 0.f, rx = 0.f;
            if (storage->get_value(VT_TLM_SFP_TX_DBM_POINT, &tx) &&
                storage->get_value(VT_TLM_SFP_RX_DBM_STATION, &rx)) {
                storage->set_value(VT_TLM_LOCAL_LINE_LOSS, tx - rx);
            }
        }

        // --- 202/203: що реально приймається кожним каналом ---
        //
        // Беремо готову produced_hz: джерело міряє її по лічильнику
        // кадрів на довгому вікні — так само, як міряється частота
        // розгортки, і саме тому вона не сіпається від джитера мережі.
        if (h265) {
            const auto s = h265->stats();
            if (s.produced_hz > 0.0) {
                storage->set_value(VT_TLM_LOCAL_H265_FPS, (float)s.produced_hz);
            }
        }
        if (mjpeg) {
            const auto s = mjpeg->stats();
            if (s.produced_hz > 0.0) {
                storage->set_value(VT_TLM_LOCAL_MJPEG_FPS, (float)s.produced_hz);
            }
        }

        // --- 204/205: що з цього дійшло до екрана ---
        //
        // Тут готового виміру немає, тож рахуємо різницею лічильників на
        // фактично виміряному проміжку.
        //
        //   204 — скільки НОВИХ кадрів забрав рендерер. Менше за 202
        //         означає, що кадри гинуть уже після декодера.
        //   205 — скільки разів екран показав хоч щось. Це частота
        //         розгортки за фактом, і вона не залежить від того, чи
        //         був кадр новим.
        {
            const uint64_t shown = h265 ? h265->stats().taken : 0;
            const uint64_t presented = display->stats().presented;

            if (have_prev) {
                const double dt = std::chrono::duration<double>(now - prev_at).count();
                if (dt > 0.05) {
                    const double raw_shown = (shown >= prev_shown)
                                           ? (shown - prev_shown) / dt : 0.0;
                    const double raw_display = (presented >= prev_presented)
                                             ? (presented - prev_presented) / dt : 0.0;

                    if (!ema_valid) {
                        ema_shown = raw_shown;
                        ema_display = raw_display;
                        ema_valid = true;
                    } else {
                        ema_shown = ema_shown * 0.8 + raw_shown * 0.2;
                        ema_display = ema_display * 0.8 + raw_display * 0.2;
                    }

                    if (h265) storage->set_value(VT_TLM_LOCAL_H265_SHOWN_FPS, (float)ema_shown);
                    storage->set_value(VT_TLM_LOCAL_DISPLAY_FPS, (float)ema_display);
                }
            }
            prev_shown = shown;
            prev_presented = presented;
            prev_at = now;
            have_prev = true;
        }
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            tick();
            std::unique_lock<std::mutex> lk(wake_mtx);
            wake_cv.wait_for(lk, std::chrono::milliseconds(cfg.period_ms),
                             [this] { return !running.load(std::memory_order_relaxed); });
        }
    }
};

// ---------------------------------------------------------------------

LocalChannels::LocalChannels(Config cfg) : impl_(new Impl(std::move(cfg))) {}

LocalChannels::~LocalChannels() { stop(); }

bool LocalChannels::start(VtTelemetryStorage& storage,
                          const display::Display& display,
                          const record::Recorder& rec,
                          const record::Storage& drive,
                          std::shared_ptr<source::FrameSource> h265,
                          std::shared_ptr<source::FrameSource> mjpeg) {
    if (impl_->running.load()) return true;
    impl_->storage = &storage;
    impl_->display = &display;
    impl_->rec = &rec;
    impl_->drive = &drive;
    impl_->h265 = std::move(h265);
    impl_->mjpeg = std::move(mjpeg);

    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[локальні канали] піднято: опитування раз на %d мс\n",
                 impl_->cfg.period_ms);
    return true;
}

void LocalChannels::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->wake_cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace vrx::osd
