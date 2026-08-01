#pragma once

// Вимір фази приходу кадру відносно сітки розгортки.
//
// ЖИВЕ ОКРЕМО ВІД МАЛЮВАННЯ. Раніше це сиділо всередині draw_frame() — не
// з логіки, а через сусідство: саме там під рукою були обидві мітки часу.
// Спільного з малюванням у нього нема нічого, зате є власний нетривіальний
// стан на три механізми, і в чужій функції він тонув.
//
// ЩО ТУТ ВИМІРЮЄТЬСЯ І НАВІЩО.
//
// Фаза — де в періоді розгортки опиняється прихід кадру. Затримка показу
// дорівнює `період − фаза`, тож це і головна метрика тракту, і величина,
// якою керує ФАПЧ, підстроюючи частоту камери.
//
// ТРИ РЕЧІ, ЯКІ ТУТ ЗРОБЛЕНО НЕОЧЕВИДНО, І КОЖНА — НАСЛІДОК ПОМИЛКИ.
//
// 1. Середнє КРУГОВЕ, а не звичайне. Фаза живе на колі: 0.1 мс і 16.6 мс
//    сусіди, а не протилежності. Спершу тут стояла ЕМА по різниці "за
//    найкоротшою дугою" — і при дрейфі понад ~10 мс/с вона зривалася:
//    відставання переростало півперіоду, різниця по колу перекидала знак,
//    і фільтр їхав НАЗАД. На виході була рівна правдоподібна цифра
//    (−3.6 мс/с), яка не мінялася ні від чого. Контролер на такому вході
//    сліпий, і петля "не працювала" саме через це, а не через коефіцієнти.
//
//    Тепер складаємо одиничні вектори кутів і беремо аргумент суми —
//    операція, для якої обгортка не існує в принципі.
//
// 2. Розкид береться з ДОВЖИНИ тієї самої суми, безкоштовно: вектори
//    збігаються -> |сума| ≈ n; розкидані по колу -> |сума| -> 0.
//
// 3. Дрейф — НАХИЛ розгорнутої фази на вікні 2 с, а не різниця двох
//    сусідніх середніх. Різниця через 250 мс ділить похибку середнього
//    (~0.4 мс) на надто короткий базис і дає ±2.3 мс/с шуму; частотна
//    ланка ФАПЧ перетворює це на випадкове блукання тримінгу, яке потім
//    доводиться виправляти фазовій ланці.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace vrx::render {

class PhaseMeter {
public:
    struct Snapshot {
        double phase_ms = 0;          // середнє за останнє вікно
        double jitter_ms = 0;         // кругове σ, згладжене
        double drift_ms_per_s = 0;    // нахил розгорнутої фази
        bool valid = false;           // вікно вже закривалось хоч раз

        // СКІЛЬКИ КОШТУЄ САМЕ ЧИСЛО `phase_ms`. Воно — середнє по
        // скінченній вибірці, тож має власну похибку σ/√n, і без неї
        // будь-який поріг на фазу задається наосліп.
        //
        // Привід конкретний: критерій захоплення ФАПЧ стояв 0.5 мс при
        // цьому шумі 0.4 мс. Тобто прапорець ЗАХОПЛЕНО фізично не міг
        // стояти стабільно навіть тоді, коли петля тримає фазу ідеально:
        // поріг був того самого порядку, що й невизначеність числа, з
        // яким його порівнювали.
        //
        // Хто міряє — той і каже, чого його вимір вартий. Інакше кожен
        // споживач вигадує цю оцінку сам, і кожен по-своєму.
        int window_n = 0;             // кадрів у останньому закритому вікні
        double noise_ms = 0;          // σ самого середнього: jitter/√n
    };

    // Новий кадр першого джерела.
    //
    // produced_ns — коли кадр вийшов з декодера;
    // vblank_ns   — мітка останньої розгортки, від неї відлік;
    // period_ns   — період розгортки.
    //
    // Кличеться ЛИШЕ на НОВИХ кадрах. Коли нового немає, джерело за
    // контрактом віддає попередній — з його старою міткою. Пустити таку
    // мітку у фільтр означало б виміряти той самий прихід двічі, причому
    // вдруге вже від іншого vblank'а: рівно та частина вибірки, яка
    // ЗМІЩЕНА в бік цілі, дістала б подвійну вагу. Для контролера це
    // систематична похибка, а не шум.
    void sample(int64_t produced_ns, int64_t vblank_ns, int64_t period_ns) {
        if (produced_ns <= 0 || vblank_ns <= 0 || period_ns <= 0) return;
        if (produced_ns == last_produced_) return;      // той самий кадр
        last_produced_ = produced_ns;

        int64_t rel = (produced_ns - vblank_ns) % period_ns;
        if (rel < 0) rel += period_ns;

        const double period_ms = period_ns / 1e6;
        const double th = 2.0 * M_PI * (rel / (double)period_ns);

        if (debug_ && dbg_left_ > 0) {
            dbg_left_--;
            std::fprintf(stderr, "RAW %.6f %.6f %.3f\n",
                         produced_ns / 1e9, vblank_ns / 1e9, rel / 1e6);
        }

        acc_sin_ += std::sin(th);
        acc_cos_ += std::cos(th);
        acc_n_++;
        if (acc_start_ns_ == 0) acc_start_ns_ = produced_ns;

        // Вікно ще не закрилося. Мінімум кадрів окремо від часу: на 30 к/с
        // їх удвічі менше, а на кількох штуках кругове σ ще безглузде.
        if (produced_ns - acc_start_ns_ < kWindowNs || acc_n_ < 8) return;

        close_window(produced_ns, period_ms);
    }

    Snapshot read() const {
        Snapshot s;
        s.phase_ms = phase_ms_;
        s.jitter_ms = jitter_ms_;
        s.drift_ms_per_s = drift_;
        s.valid = valid_;
        s.window_n = last_n_;
        // Розкид беремо ЗГЛАДЖЕНИЙ, а кількість — останнього вікна: σ по
        // 15 кадрах сама має похибку ~18%, і поріг стрибав би разом із
        // нею. Кількість же від вікна до вікна майже не міняється.
        s.noise_ms = (valid_ && last_n_ > 1) ? jitter_ms_ / std::sqrt((double)last_n_) : 0.0;
        return s;
    }

    // Вивід змінився — попередні виміри стосуються іншого періоду.
    void reset() {
        acc_sin_ = acc_cos_ = 0.0;
        acc_n_ = 0;
        acc_start_ns_ = 0;
        last_produced_ = 0;
        prev_mean_ = -1;
        un_n_ = un_head_ = 0;
        un_acc_ = 0;
        last_n_ = 0;
        valid_ = false;
    }

private:
    // Вікно усереднення. Компроміс: довше — точніше середнє, але фаза
    // встигає проповзти всередині вікна й роздути оцінку розкиду.
    // 250 мс = ~15 кадрів, і при дрейфі 6 мс/с розмиття 1.5 мс.
    static constexpr int64_t kWindowNs = 250000000LL;
    static constexpr int kDriftPoints = 8;      // 8 x 250 мс = 2 с

    void close_window(int64_t now_ns, double period_ms) {
        const double n = (double)acc_n_;
        double mean = std::atan2(acc_sin_ / n, acc_cos_ / n);
        if (mean < 0.0) mean += 2.0 * M_PI;
        const double mean_ms = mean * period_ms / (2.0 * M_PI);

        // Довжина середнього вектора: 1 — усі кадри в одну точку, 0 —
        // рівномірно по колу. Стандартна кругова сигма sqrt(-2 ln R).
        const double R = std::hypot(acc_sin_, acc_cos_) / n;
        const double sig_rad = R > 1e-6 ? std::sqrt(-2.0 * std::log(R < 1.0 ? R : 1.0))
                                        : 2.0 * M_PI;
        double sig_ms = sig_rad * period_ms / (2.0 * M_PI);
        if (sig_ms > period_ms * 0.25) sig_ms = period_ms * 0.25;

        // Розгортаємо: до накопиченої фази додаємо крок по найкоротшій
        // дузі. Кроки йдуть через 250 мс, тож навіть 30 мс/с дають менше
        // півперіоду — обгортка однозначна.
        if (prev_mean_ >= 0) {
            double dd = mean_ms - prev_mean_;
            if (dd >  period_ms * 0.5) dd -= period_ms;
            if (dd < -period_ms * 0.5) dd += period_ms;
            un_acc_ += dd;
        }
        prev_mean_ = mean_ms;

        un_phase_[un_head_] = un_acc_;
        un_time_[un_head_] = now_ns / 1e9;
        un_head_ = (un_head_ + 1) % kDriftPoints;
        if (un_n_ < kDriftPoints) un_n_++;

        if (un_n_ >= 4) {
            double sx = 0, sy = 0;
            for (int k = 0; k < un_n_; ++k) { sx += un_time_[k]; sy += un_phase_[k]; }
            const double mx = sx / un_n_, my = sy / un_n_;
            double num = 0, den = 0;
            for (int k = 0; k < un_n_; ++k) {
                const double dx = un_time_[k] - mx;
                num += dx * (un_phase_[k] - my);
                den += dx * dx;
            }
            if (den > 1e-9) drift_ = num / den;
        }

        if (debug_) {
            std::fprintf(stderr, "PH %.3f %.3f %u %.3f %.3f\n",
                         now_ns / 1e9, mean_ms, acc_n_, sig_ms, drift_);
        }

        phase_ms_ = mean_ms;
        last_n_ = (int)acc_n_;
        // Розкид згладжуємо, а середнє — ні. Різниця не формальна:
        // середнє кутове, і фільтрувати його звичайним ЕМА не можна;
        // розкид же — звичайна додатна величина.
        //
        // Згладжувати ТРЕБА: по 15 кадрах вікна оцінка σ сама має похибку
        // ~18%, а один викид роздуває її вдвічі. Запас у ФАПЧ це подвоює,
        // ціль повзе за ним на ±1.6 мс, і петля ганяється за власним
        // шумом замість тримати фазу. Стала ~2.5 с.
        jitter_ms_ = valid_ ? jitter_ms_ * 0.9 + sig_ms * 0.1 : sig_ms;
        valid_ = true;

        acc_sin_ = acc_cos_ = 0.0;
        acc_n_ = 0;
        acc_start_ns_ = 0;
    }

    double acc_sin_ = 0, acc_cos_ = 0;
    uint32_t acc_n_ = 0;
    int64_t acc_start_ns_ = 0;
    int64_t last_produced_ = 0;

    double phase_ms_ = 0;
    double jitter_ms_ = 0;
    int last_n_ = 0;
    double drift_ = 0;
    bool valid_ = false;

    double prev_mean_ = -1;
    double un_phase_[kDriftPoints] = {};
    double un_time_[kDriftPoints] = {};
    int un_n_ = 0, un_head_ = 0;
    double un_acc_ = 0;

    // Сирі ряди в stderr — VRX_PHASE_DEBUG=1. Потрібне саме тому, що
    // зведені числа тут уже один раз збрехали, і без сирого ряду
    // відрізнити "вимір поганий" від "об'єкт такий" неможливо.
    bool debug_ = getenv("VRX_PHASE_DEBUG") != nullptr;
    int dbg_left_ = 400;
};

} // namespace vrx::render
