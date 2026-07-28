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
    double freq_mhz = 0.0;         // частотна ланка, мГц

    // Історія фазового доданка: вимір дрейфу відображає минуле, тож
    // віднімати з нього треба той зсув, що діяв тоді ж. 2 такти = 1 с,
    // рівно половина вікна регресії.
    static constexpr int kPhaseTermLag = 2;
    double phase_term_hist[kPhaseTermLag] = {};
    int hist_head = 0;
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

    // Крок часу більше не потрібен: частотна ланка інтегрує НЕВ'ЯЗКУ
    // ЧАСТОТИ часткою за крок, а не помилку за секунду, тож нерівний
    // темп петлі на неї не впливає.
    void tick() {
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

        // Ціль = точка опиту мінус односторонній запас. Обрив пили один
        // (кадр, що не встиг до опиту, чекає цілий період), тож відходимо
        // від нього рівно настільки, наскільки дістає джитер, — далі
        // відходити означає платити затримкою просто так.
        //
        // Поки розкид ще не виміряний (коротка вибірка після старту),
        // беремо МАКСИМАЛЬНИЙ запас: помилитися в бік зайвої затримки
        // дешевше, ніж у бік втрачених кадрів.
        double guard = rs.phase_jitter_ms > 0.0
                     ? cfg.guard_sigmas * rs.phase_jitter_ms
                     : cfg.guard_max_ms;
        guard = clamp(guard, cfg.guard_min_ms, cfg.guard_max_ms);

        double target = cfg.target_phase_ms;
        if (target < 0.0) target = rs.poll_offset_ms - guard;
        target = wrap_positive(target, period_ms);

        const double err = wrap_signed(rs.phase_ms - target, period_ms);

        // --- ланка ЧАСТОТИ ---
        //
        // Дрейф фази — це різниця частот, виміряна навпростець і без
        // обгортки. Переводимо в мГц і гасимо інтегратором. Знак: дрейф
        // від'ємний (кадри приходять дедалі раніше) означає, що камера
        // швидша, тож команда має піти вниз.
        const double mismatch_mhz = -rs.phase_drift_ms_per_s * 1000.0 / period_ms;

        // ВІДНІМАЄМО ВЛАСНИЙ ВНЕСОК. Фазова ланка рухає фазу єдиним
        // доступним способом — навмисно розстроюючи частоту. У виміряній
        // нев'язці цей зсув присутній, і якщо його не відняти, частотна
        // ланка старанно прибиратиме те, що фазова щойно ввела: дві ланки
        // борються, фаза не доходить до цілі, петля гойдається.
        //
        // Беремо зсув НЕ поточний, а той, що діяв ~1 с тому: вимір дрейфу
        // рахується регресією на вікні 2 с, тобто відображає минуле.
        const double own = phase_term_hist[hist_head];
        phase_term_hist[hist_head] = cfg.kp_mhz_per_ms * err;
        hist_head = (hist_head + 1) % kPhaseTermLag;

        freq_mhz -= cfg.kf_per_step * (mismatch_mhz - own * cfg.actuator_gain)
                    / cfg.actuator_gain;

        // Антивіндап: частотна ланка сама по собі не має права впертися
        // в рейку, інакше після довгої відсутності сигналу петля
        // виходитиме з насичення хвилинами.
        freq_mhz = clamp(freq_mhz, -double(cfg.trim_limit_mhz), double(cfg.trim_max_mhz));

        // --- ланка ФАЗИ ---
        //
        // Помилка додатна (фаза попереду цілі) -> треба, щоб фаза спадала
        // -> камера має піти ШВИДШЕ -> команда додатна.
        const double raw = freq_mhz + cfg.kp_mhz_per_ms * err;
        const int want = (int)std::lround(clamp(raw, -double(cfg.trim_limit_mhz),
                                                double(cfg.trim_max_mhz)));

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
            st.poll_ms = rs.poll_offset_ms;
            st.jitter_ms = rs.phase_jitter_ms;
            st.guard_ms = guard;
            st.latency_ms = period_ms - rs.phase_ms;
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

        while (running.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lk(wake_mtx);
                wake_cv.wait_for(lk, std::chrono::milliseconds(cfg.update_ms),
                                 [this] { return !running.load(std::memory_order_relaxed); });
            }
            if (!running.load(std::memory_order_relaxed)) break;

            if (now_ns() - t_start < int64_t(cfg.warmup_ms) * 1000000LL) {
                hold("розігрів");
                continue;
            }
            tick();
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
