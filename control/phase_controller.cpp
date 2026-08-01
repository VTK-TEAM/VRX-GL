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

// --- сталі критерію захоплення ---
//
// Тут числа, а не поля конфігу, бо міняти їх окремо від самого критерію
// немає сенсу: вони описують не політику, а те, як читається статистика.

// Нижня межа порога. Страховка від виродженої оцінки шуму (коротке вікно,
// підозріло чистий лінк): поріг, менший за це, означав би, що ми віримо
// власному вимірникові точніше, ніж він того вартий.
constexpr double kLockFloorMs = 0.10;

// Верхня межа — частка запасу. Прапорець не має світитися, коли похибка
// з'їдає помітну частину того, що відділяє нас від обриву пили, навіть
// якщо вимір настільки грубий, що формально "не розрізняє".
constexpr double kLockGuardFrac = 0.25;

// У скільки шумів виміру дозволено гуляти похибці. Рівно один шум був би
// занадто жорстко: розкид сам оцінюється по короткій пам'яті ЕМА й має
// власну похибку близько 27%.
constexpr double kLockSpreadK = 1.6;

// Гістерезис: узятий поріг розпускається, поки прапорець уже стоїть.
// Без нього індикатор тремтить рівно на межі — тобто саме тоді, коли на
// нього дивляться.
constexpr double kLockHyst = 1.5;

} // namespace

struct PhaseController::Impl {
    display::Display& display;
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
    bool seeded = false;           // частотну ланку вже засіяно виміром
    int applied_mhz = 0;           // що, за нашими даними, стоїть на камері
    int64_t last_send_ns = 0;
    uint64_t last_taken = 0;

    // Стан ІНДИКАТОРА захоплення. Живе окремо від регулятора й на
    // керування не впливає ніяк: згладжування в контурі коштувало б
    // швидкодією, а тут воно потрібне лише щоб відрізнити рух фази від
    // шуму її виміру.
    double err_mean = 0;           // ЕМА похибки — зміщення
    double err_var = 0;            // ЕМА квадрата відхилення від нього
    bool lock_valid = false;       // фільтри вже засіяні
    bool locked = false;           // попередній стан — для гістерезису
    uint64_t ticks = 0, lock_ticks = 0;
    uint64_t legacy_lock_run = 0, legacy_lock_ticks = 0;

    Impl(display::Display& d, render::GlRenderer& r,
         std::shared_ptr<source::FrameSource> s, Config c)
        : display(d), renderer(r), source(std::move(s)), cfg(std::move(c)),
          camera(cfg.camera) {}

    void hold(const char* why) {
        // Пауза в ЛАНЦЮЖКУ вимірів, а не просто пропущений такт: після
        // неї фаза може стояти зовсім не там, де стояла. Тягти через
        // розрив згладжену похибку означало б показати захоплення, якого
        // вже немає.
        lock_valid = false;
        locked = false;
        legacy_lock_run = 0;

        std::lock_guard<std::mutex> lk(st_mtx);
        st.engaged = false;
        st.locked = false;
        st.holding = why;
    }

    // Записує значення на камеру. Повертає true, якщо камера підтвердила.
    bool push(int mhz) {
        const bool ok = camera.set(cfg.param + ".fpsTrimMilliHz", mhz);
        last_send_ns = now_ns();
        {
            std::lock_guard<std::mutex> lk(st_mtx);
            st.sent = camera.requests();
            st.failed = camera.failures();
            // Накопичувальний лічильник пам'ятає й ті невдачі, що були,
            // поки камера ще не вмикалась. Тривожним є лише те, що не
            // проходить ЗАРАЗ.
            st.last_write_failed = !ok;
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
        // Запас сталий: підлаштовувати його під поточний розкид означає
        // рухати ціль, а рух цілі петля потім виправляє як помилку.
        const double guard = cfg.guard_ms;

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

        // ОДНОРАЗОВЕ ЗАСІВАННЯ замість повільного набігання з нуля.
        //
        // Ланка шукає константу — розстройку кварців, — і ця константа
        // ВИМІРЮЄТЬСЯ прямо, першим же виміром дрейфу. Набігати на неї
        // інтегратором означає витратити стільки часу, скільки дозволить
        // стала ланки, і весь цей час тримати фазу не там, де треба.
        //
        // Заміряно: при kf = 0.01 (стала 50 с) захоплення тривало 140 с,
        // і за цей час на екран не дійшло 4% кадрів, а крок зйомки був у
        // нормі лише 96%. Проти 20 с і 99.7% при kf = 0.10.
        //
        // Тому перший достовірний вимір дрейфу застосовуємо ЦІЛКОМ, а
        // повільний інтегратор далі лише підчищає залишок.
        if (!seeded) {
            freq_mhz = clamp(-mismatch_mhz / cfg.actuator_gain,
                             -double(cfg.trim_limit_mhz), double(cfg.trim_max_mhz));
            seeded = true;
        } else {
            freq_mhz -= cfg.kf_per_step * (mismatch_mhz - own * cfg.actuator_gain)
                        / cfg.actuator_gain;
        }

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

        // --- ІНДИКАТОР ЗАХОПЛЕННЯ ---
        //
        // Питання, на яке він відповідає: чи відрізняється те, що ми
        // бачимо, від "фаза стоїть на цілі, а гуляє лише вимір".
        //
        // Крок за часом беремо номінальний: справжній гуляє на десятки
        // мілісекунд через блокуючий запит до камери, і для сталої в 2 с
        // це нічого не міняє.
        const double alpha = 1.0 - std::exp(-(cfg.update_ms / 1000.0) / cfg.lock_tau_s);

        if (!lock_valid) {
            err_mean = err;
            err_var = 0.0;
            lock_valid = true;
        } else {
            err_mean += alpha * (err - err_mean);
            const double dev = err - err_mean;
            err_var += alpha * (dev * dev - err_var);
        }
        const double err_spread = std::sqrt(err_var);

        // Шум одиничного виміру фази (σ середнього по вікну). Поки
        // вимірник його не дає — критерій не працює, і чесніше не
        // світити прапорцем узагалі, ніж світити навмання.
        const double noise = rs.phase_noise_ms;

        // Згладжування давить некорельований шум у √(α/(2−α)) разів.
        // Сусідні такти читають РІЗНІ вікна виміру (такт 500 мс, вікно
        // 250), тож незалежність тут не припущення, а конструкція.
        const double smooth_noise = noise * std::sqrt(alpha / (2.0 - alpha));
        double thr = clamp(cfg.lock_sigmas * smooth_noise,
                           kLockFloorMs, guard * kLockGuardFrac);

        const double relax = locked ? kLockHyst : 1.0;
        const bool bias_ok = std::fabs(err_mean) <= thr * relax;
        const bool spread_ok = err_spread <= kLockSpreadK * noise * relax;
        locked = (noise > 0.0) && bias_ok && spread_ok;

        ticks++;
        if (locked) lock_ticks++;

        // СТАРИЙ КРИТЕРІЙ поруч — щоб A/B ліг з одного прогону, а не з
        // двох різних п'ятихвилинок: петля стохастична, і порівнювати її
        // саму з собою в різні дні означає міряти погоду.
        legacy_lock_run = std::fabs(err) <= 0.5 ? legacy_lock_run + 1 : 0;
        if (legacy_lock_run >= 3) legacy_lock_ticks++;

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
            st.locked = locked;
            st.error_smooth_ms = err_mean;
            st.error_spread_ms = err_spread;
            st.meas_noise_ms = noise;
            st.lock_thr_ms = thr;
            st.ticks = ticks;
            st.lock_ticks = lock_ticks;
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

        // Скільки часу петля справді тримала фазу — число, яке інакше
        // довелося б вигрібати з логу по слову ЗАХОПЛЕНО.
        if (ticks > 0) {
            std::fprintf(stderr,
                         "[фаза] тактів ведення %llu, у захопленні %llu (%.1f%%)"
                         " | старий критерій |похибка|<=0.5 три поспіль: %llu (%.1f%%)\n",
                         (unsigned long long)ticks, (unsigned long long)lock_ticks,
                         100.0 * lock_ticks / ticks,
                         (unsigned long long)legacy_lock_ticks,
                         100.0 * legacy_lock_ticks / ticks);
        }

        // Лишати камеру підстроєною після нашого виходу нема сенсу:
        // наступний запуск почне з невідомого стану.
        push(0);
    }
};

PhaseController::PhaseController(display::Display& display,
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
