#include "local_channels.hpp"

#include "telemetry/vt_telemetry_index.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace vrx::osd {

struct LocalChannels::Impl {
    Config cfg;

    VtTelemetryStorage* storage = nullptr;
    const display::Display* display = nullptr;
    const render::GlRenderer* renderer = nullptr;
    const control::PhaseController* phase = nullptr;
    const record::Recorder* rec = nullptr;
    const record::Recorder* rec_pip = nullptr;   // вторинні — для індикатора 200
    const record::Recorder* rec_cap = nullptr;
    const record::Storage* drive = nullptr;
    std::shared_ptr<source::FrameSource> h265;
    std::shared_ptr<source::FrameSource> mjpeg;
    std::shared_ptr<source::FrameSource> capture;   // локальний захват MS2106

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
    uint64_t prev_dropped = 0;
    uint64_t prev_late = 0;

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

    // Втрати згладжуються ОКРЕМО й сильніше: вони рідкі й нерівні. При
    // 0.04 збою на секунду сире вікно в пів секунди дає нуль двадцять
    // разів поспіль і разовий сплеск 2.0 — цифра, по якій нічого не
    // видно. Стала ~5 с показує саме темп подій.
    double ema_dropped = 0.0, ema_late = 0.0;

    explicit Impl(Config c) : cfg(std::move(c)) {}

    void tick() {
        const auto now = std::chrono::steady_clock::now();

        // Крок часу рахується ОДИН РАЗ на такт і на всі лічильники: усі
        // вони — різниці на тому самому проміжку, і брати його по-різному
        // означало б рахувати частоти від різних баз.
        const double dt = have_prev
            ? std::chrono::duration<double>(now - prev_at).count() : 0.0;
        const bool step_ok = have_prev && dt > 0.05;

        // --- 200: стан запису ---
        //
        // Три стани, а не два: "носія немає" і "носій є, але не пишемо" —
        // різні речі, і пілоту треба бачити саме яка. Значок у конфізі
        // розрізняє їх через ENUM_SWITCH.
        {
            const auto ds = drive->state();
            // "Пише" — це коли активний БУДЬ-ЯКИЙ рекордер, а не лише
            // основний: основне джерело може бути вимкнене, а захват іти,
            // і пілот має бачити, що на носій щось лягає.
            const bool any_rec = rec->stats().active
                || (rec_pip && rec_pip->stats().active)
                || (rec_cap && rec_cap->stats().active);
            int state = 2;
            if (any_rec) state = 1;          // пише хоч один
            else if (!ds.usable()) state = 0; // носія немає й ніхто не пише
            storage->set_value(VT_TLM_LOCAL_RECORDING_STATE, (float)state);

            // --- 213: скільки ще влізе на носій, ГіБ ---
            //
            // Storage міряв це від початку, але друкував лише в рядок
            // статусу — тобто в термінал, якого в полі немає. Пілот бачив
            // "запис іде", а чи надовго його вистачить — ніде.
            //
            // Ділимо на 1024³, а не на 1e9: саме так рахують ГБ Windows і
            // провідник, і 57 на екрані має збігатися з 57 у властивостях
            // флешки, а не показувати 61 через десяткові "гіга".
            //
            // Оновлюємо ЛИШЕ коли носій придатний. Немає флешки — канал
            // протухає сам і дає "--", а не нуль: нуль читався б як
            // "місця не лишилось", і це зовсім інша новина.
            if (ds.usable()) {
                storage->set_value(VT_TLM_LOCAL_DRIVE_FREE,
                                   (float)(ds.free_bytes / 1073741824.0));  // 1024³
            }
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
        if (capture) {
            const auto s = capture->stats();
            if (s.produced_hz > 0.0) {
                storage->set_value(VT_TLM_LOCAL_CAPTURE_FPS, (float)s.produced_hz);
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

            if (step_ok) {
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
            prev_shown = shown;
            prev_presented = presented;
        }

        // --- 206: чи веде петля фази ---
        //
        // Три стани, а не два, з тієї ж причини, що й у запису: "не веде"
        // і "веде, але ще не захопила" — різні речі. Перше означає, що
        // петлі немає (вимкнена, немає сигналу чи виміру частоти), друге
        // — що вона працює й фаза ще їде до цілі.
        {
            const auto ps = phase->stats();
            const int state = !ps.engaged ? 0 : (ps.locked ? 2 : 1);
            storage->set_value(VT_TLM_LOCAL_PHASE_LOCK, (float)state);
        }

        // --- 207: затримка тракту ---
        //
        // Береться готова з рендерера: він єдиний, хто бачить обидва
        // кінці — мітку виходу кадру з декодера й ПІДТВЕРДЖЕННЯ показу
        // від заліза. Це вже згладжена ЕМА, тож тут її не чіпаємо.
        //
        // Не плутати з 209: затримка каже, скільки кадр їхав, а 209 —
        // скільком кадрам вона дісталася на цілий період більша.
        {
            const auto rs = renderer->stats();
            if (rs.latency_avg_ms > 0.0) {
                storage->set_value(VT_TLM_LOCAL_LATENCY_MS, (float)rs.latency_avg_ms);
            }
        }

        // --- 208/209: чим заплачено за цю затримку ---
        //
        //   208 — кадр декодований, але на екран не потрапив: черга
        //         зрізала його як застарілий. Чиста втрата.
        //   209 — кадр потрапив, але спізнився на опит рендерера й тому
        //         поїхав на розгортку пізніше. НЕ втрата: рух на екрані
        //         від цього не рветься, платимо лише затримкою.
        //
        // Обидва рахуються різницею лічильників на виміряному проміжку,
        // як 204/205.
        {
            const uint64_t dropped = h265 ? h265->stats().dropped : 0;
            const uint64_t late = renderer->stats().late;

            if (step_ok) {
                const double raw_d = dropped >= prev_dropped ? (dropped - prev_dropped) / dt : 0.0;
                const double raw_l = late >= prev_late ? (late - prev_late) / dt : 0.0;
                ema_dropped = ema_dropped * 0.9 + raw_d * 0.1;
                ema_late = ema_late * 0.9 + raw_l * 0.1;
                storage->set_value(VT_TLM_LOCAL_DROPPED_FPS, (float)ema_dropped);
                storage->set_value(VT_TLM_LOCAL_LATE_FPS, (float)ema_late);
            }
            prev_dropped = dropped;
            prev_late = late;
        }

        // --- 210: кадри, які мали прийти й не прийшли ---
        //
        // Єдиний канал тракту, що віддається КІЛЬКІСТЮ, а не темпом. Решта
        // відповідають на "як воно зараз", а цей — на "скільки всього
        // загубилось за політ", і для такого питання згладжування шкідливе:
        // рідкісний пробій має лишитись видимим і через дві хвилини.
        //
        // Береться з основного каналу: другий іде на 25 к/с зі своїм
        // передавачем, і зводити їхні втрати в одне число означало б
        // отримати величину, за якою не видно жодного з двох.
        if (h265) {
            storage->set_value(VT_TLM_LOCAL_LOST_FRAMES, (float)h265->stats().missing);
        }

        // --- 211/212: системний годинник і дата ---
        //
        // Найпростіший канал у станції: просто те, що показує сам
        // пристрій. Потрібен саме тому, що синхронізувати його в полі нема
        // з чим — інтернету немає, — і єдине джерело правди тут це те, що
        // хтось виставив руками.
        {
            const std::time_t t = std::time(nullptr);
            struct tm lt;
            localtime_r(&t, &lt);
            storage->set_value(VT_TLM_LOCAL_CLOCK,
                               (float)(lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec));
            storage->set_value(VT_TLM_LOCAL_DATE,
                               (float)((lt.tm_year % 100) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday));
        }

        prev_at = now;
        have_prev = true;
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
                          const render::GlRenderer& renderer,
                          const control::PhaseController& phase,
                          const record::Recorder& rec,
                          const record::Storage& drive,
                          std::shared_ptr<source::FrameSource> h265,
                          std::shared_ptr<source::FrameSource> mjpeg,
                          std::shared_ptr<source::FrameSource> capture,
                          const record::Recorder* rec_pip,
                          const record::Recorder* rec_cap) {
    if (impl_->running.load()) return true;
    impl_->storage = &storage;
    impl_->display = &display;
    impl_->renderer = &renderer;
    impl_->phase = &phase;
    impl_->rec = &rec;
    impl_->rec_pip = rec_pip;
    impl_->rec_cap = rec_cap;
    impl_->drive = &drive;
    impl_->h265 = std::move(h265);
    impl_->mjpeg = std::move(mjpeg);
    impl_->capture = std::move(capture);

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
