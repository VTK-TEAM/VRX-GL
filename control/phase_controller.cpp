#include "phase_controller.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace vrx::control {
namespace {

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Фаза живе на колі довжиною в період: 0.1 мс і 16.6 мс — сусіди, а не
// протилежності. Помилку треба брати по найкоротшій дузі, інакше петля
// повезе камеру довгою стороною через увесь період.
double wrap_signed(double v, double period) {
    if (period <= 0.0) return v;
    while (v >  period * 0.5) v -= period;
    while (v <= -period * 0.5) v += period;
    return v;
}

double wrap_positive(double v, double period) {
    if (period <= 0.0) return v;
    while (v < 0.0) v += period;
    while (v >= period) v -= period;
    return v;
}

double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

struct PhaseController::Impl {
    display::DisplayManager& display;
    render::GlRenderer& renderer;
    std::shared_ptr<source::FrameSource> source;
    Config cfg;
    CameraApi camera;

    std::thread th;
    std::atomic<bool> running{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    mutable std::mutex st_mtx;
    PhaseStats st{};

    // Стан регулятора.
    double integral = 0.0;         // мс·с
    int applied_mhz = 0;           // що, за нашими даними, стоїть на камері
    int64_t last_send_ns = 0;
    uint64_t last_taken = 0;
    int lock_run = 0;

    Impl(display::DisplayManager& d, render::GlRenderer& r,
         std::shared_ptr<source::FrameSource> s, Config c)
        : display(d), renderer(r), source(std::move(s)), cfg(std::move(c)),
          camera(cfg.camera) {}

    void hold(const char* why) {
        std::lock_guard<std::mutex> lk(st_mtx);
        st.engaged = false;
        st.locked = false;
        st.holding = why;
        lock_run = 0;
    }

    // Записує значення на камеру. Повертає true, якщо камера підтвердила.
    bool push(int mhz) {
        const bool ok = camera.set(cfg.param + ".fpsTrimMilliHz", mhz);
        last_send_ns = now_ns();
        {
            std::lock_guard<std::mutex> lk(st_mtx);
            st.sent = camera.requests();
            st.failed = camera.failures();
            if (ok) st.trim_mhz = mhz;
        }
        if (ok) applied_mhz = mhz;
        return ok;
    }

    void tick(double dt_s) {
        const auto ds = display.stats();
        const auto rs = renderer.stats();
        const auto ss = source->stats();

        if (ds.measured_hz <= 0.0) { hold("частота екрана ще не виміряна"); return; }

        // Немає нових кадрів — немає й нового виміру фази. Продовжувати
        // інтегрувати на застиглому значенні означало б накрутити
        // підстроювання в межу за час, поки сигналу просто не було.
        if (ss.taken == last_taken) {
            last_taken = ss.taken;
            hold("немає нових кадрів");
            return;
        }
        last_taken = ss.taken;

        const double period_ms = 1000.0 / ds.measured_hz;

        double target = cfg.target_phase_ms;
        if (target < 0.0) target = rs.poll_offset_ms - period_ms * 0.5;
        target = wrap_positive(target, period_ms);

        const double err = wrap_signed(rs.phase_ms - target, period_ms);

        integral += err * dt_s;

        // Антивіндап: інтегральна частина сама по собі не має права
        // з'їсти весь діапазон камери, інакше після довгої відсутності
        // сигналу петля виходитиме з насичення хвилинами.
        if (cfg.ki_mhz_per_ms_s > 0.0) {
            const double lim = cfg.trim_limit_mhz / cfg.ki_mhz_per_ms_s;
            integral = clamp(integral, -lim, lim);
        }

        // Знак. Помилка додатна (фаза попереду цілі) -> треба, щоб фаза
        // спадала -> камера має піти ШВИДШЕ -> підстроювання додатне.
        // Збігається з дрейф = −підстроювання/частота.
        const double raw = cfg.kp_mhz_per_ms * err + cfg.ki_mhz_per_ms_s * integral;
        const int want = (int)std::lround(clamp(raw, -double(cfg.trim_limit_mhz),
                                                double(cfg.trim_limit_mhz)));

        const bool due = (now_ns() - last_send_ns) > int64_t(cfg.heartbeat_ms) * 1000000LL;
        if (std::abs(want - applied_mhz) >= cfg.min_step_mhz || due) {
            push(want);
        }

        const bool in_lock = std::fabs(err) <= cfg.lock_ms;
        lock_run = in_lock ? lock_run + 1 : 0;

        {
            std::lock_guard<std::mutex> lk(st_mtx);
            st.engaged = true;
            st.holding = "";
            st.display_hz = ds.measured_hz;
            st.period_ms = period_ms;
            st.phase_ms = rs.phase_ms;
            st.target_ms = target;
            st.error_ms = err;
            st.locked = lock_run >= 3;
        }
    }

    void loop() {
        // Підстроювання на камері не зберігається між запусками, але наш
        // процес міг і перезапуститися окремо від неї. Явний нуль на
        // старті прибирає розбіжність між тим, що стоїть на камері, і
        // тим, що ми думаємо, ніби стоїть.
        push(0);
        applied_mhz = 0;

        const int64_t t_start = now_ns();
        int64_t prev = now_ns();

        while (running.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(wake_mtx);
                wake_cv.wait_for(lk, std::chrono::milliseconds(cfg.update_ms),
                                 [this] { return !running.load(std::memory_order_relaxed); });
            }
            if (!running.load(std::memory_order_relaxed)) break;

            const int64_t now = now_ns();
            const double dt_s = (now - prev) / 1e9;
            prev = now;

            if (now - t_start < int64_t(cfg.warmup_ms) * 1000000LL) {
                hold("розігрів");
                continue;
            }
            // Запит до камери блокує, а на поганому лінку ще й довго.
            // Тому крок часу міряється — інтеграл лишається правильним
            // навіть коли петля йшла нерівно.
            tick(dt_s);
        }

        // Лишати камеру підстроєною після нашого виходу нема сенсу:
        // наступний запуск почне з невідомого стану.
        push(0);
    }
};

PhaseController::PhaseController(display::DisplayManager& display,
                                 render::GlRenderer& renderer,
                                 std::shared_ptr<source::FrameSource> source,
                                 Config cfg)
    : impl_(new Impl(display, renderer, std::move(source), std::move(cfg))) {}

PhaseController::~PhaseController() { stop(); }

bool PhaseController::start() {
    if (impl_->running.load()) return true;
    if (!impl_->source) {
        std::fprintf(stderr, "[фаза] джерела немає, петля не піднімається\n");
        return false;
    }
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[фаза] петля піднята: камера %s, параметр %s.fpsTrimMilliHz\n",
                 impl_->cfg.camera.host.c_str(), impl_->cfg.param.c_str());
    return true;
}

void PhaseController::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->wake_cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
}

PhaseStats PhaseController::stats() const {
    std::lock_guard<std::mutex> lk(impl_->st_mtx);
    return impl_->st;
}

} // namespace vrx::control
