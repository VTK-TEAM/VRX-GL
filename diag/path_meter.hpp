#pragma once

// НАСКРІЗНИЙ ВИМІР ТРАКТУ. Вимкнено на компіляції.
//
// Штатна статистика програми зведена: min/avg/max від самого запуску,
// плюс кілька ЕМА. Для щоденної роботи цього досить, але побачити розподіл
// по ній не можна — ні перцентилів, ні σ, ні того, як виглядає хвіст. А
// саме хвіст і визначає, смикається картинка чи ні.
//
// Тут — сирі мітки часу кожного кадру на всіх чотирьох рубежах:
//
//     вхід декодера -> вихід декодера -> показ на екрані
//                                     -> розгортка (окремо)
//
// Пишеться це в пам'ять (5 хвилин на 60 к/с — близько 0.4 МБ на канал) і
// зводиться в звіт наприкінці прогону.
//
// ЧОМУ ДЕФАЙН, А НЕ ПРАПОРЕЦЬ. Вимір сидить у гарячих місцях — у колбеку
// appsink і в обробнику розгортки. Прапорець лишив би там перевірку
// назавжди, а головне — лишив би сам код у робочому бінарнику. З дефайном
// у вимкненому стані всі виклики порожні, і від них не лишається нічого.
//
// ЯК УВІМКНУТИ:
//     cmake -B build -DCMAKE_CXX_FLAGS=-DVRX_MEASURE=1 && cmake --build build
// або просто виправити нуль на одиницю рядком нижче.
//
// Звіт лягає у VRX_MEASURE_OUT (типово ./measure.txt) при виході.
//
// ЩО ВИМІР РОБИТЬ ІЗ ТИМ, ЩО МІРЯЄ. Кожна точка бере мьютекс і робить
// push_back. У колбеку розгортки лок уже й так береться, тож це не зміна
// роду занять, але для чистоти: результати вимірювального прогону варто
// звіряти зі штатною статистикою того ж прогону — вона рахується інакше.

#ifndef VRX_MEASURE
#define VRX_MEASURE 0
#endif

#if VRX_MEASURE

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vrx::diag {

class PathMeter {
public:
    static PathMeter& get() {
        static PathMeter m;
        return m;
    }

    // Реєструє канал за іменем і повертає сталий номер. Кличеться і з
    // джерела, і з рендерера — обидва приходять до одного номера, і
    // плести його через інтерфейси не треба.
    int channel(const char* name) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < ch_.size(); ++i) {
            if (ch_[i].name == name) return (int)i;
        }
        ch_.push_back(Channel{name, {}, {}, {}, 0, 0});
        ch_.back().recs.reserve(32768);
        return (int)ch_.size() - 1;
    }

    // Буфер зайшов у декодер.
    void in(int ch, int64_t ns) {
        if (ch < 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (ch >= (int)ch_.size()) return;
        ch_[ch].ins.push_back(ns);
    }

    // Кадр вийшов з декодера. in_ns — мітка ЙОГО ж входу, зіставлена за
    // FIFO самим джерелом (у потоці без B-кадрів порядок зберігається).
    void out(int ch, int64_t produced_ns, int64_t in_ns) {
        if (ch < 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (ch >= (int)ch_.size()) return;
        Channel& c = ch_[ch];
        c.by_produced[produced_ns] = c.recs.size();
        c.recs.push_back(Rec{in_ns, produced_ns, 0});
    }

    // Кадр опинився на екрані. Кличеться на КОЖНІЙ розгортці з тим, що
    // зараз показано, тож той самий кадр приходить сюди повторно — беремо
    // ПЕРШИЙ показ, решту рахуємо як повтори.
    void shown(int ch, int64_t produced_ns, int64_t vblank_ns) {
        if (ch < 0 || produced_ns <= 0) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (ch >= (int)ch_.size()) return;
        Channel& c = ch_[ch];
        auto it = c.by_produced.find(produced_ns);
        if (it == c.by_produced.end()) { c.orphans++; return; }
        Rec& r = c.recs[it->second];
        if (r.shown_ns != 0) { c.repeats++; return; }
        r.shown_ns = vblank_ns;
    }

    // Розгортка. Окремо від каналів: вона одна на всіх.
    void flip(int64_t ns) {
        std::lock_guard<std::mutex> lk(mtx_);
        flips_.push_back(ns);
    }

    void report(const char* path) const;

private:
    struct Rec {
        int64_t in_ns = 0;       // зайшов у декодер
        int64_t out_ns = 0;      // вийшов з декодера (= produced_ns)
        int64_t shown_ns = 0;    // розгортка, на якій його побачили
    };
    struct Channel {
        std::string name;
        std::vector<Rec> recs;
        std::vector<int64_t> ins;
        std::unordered_map<int64_t, size_t> by_produced;
        uint64_t repeats = 0;    // показів кадру, який уже показували
        uint64_t orphans = 0;    // показано кадр, якого немає в записах
    };

    mutable std::mutex mtx_;
    std::vector<Channel> ch_;
    std::vector<int64_t> flips_;
};

// ---------------------------------------------------------------------
// Зведення

struct Dist {
    size_t n = 0;
    double mean = 0, sd = 0;
    double min = 0, p01 = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

inline Dist distribution(std::vector<double> v) {
    Dist d;
    d.n = v.size();
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double x : v) s += x;
    d.mean = s / v.size();
    double q = 0;
    for (double x : v) q += (x - d.mean) * (x - d.mean);
    d.sd = std::sqrt(q / v.size());
    auto at = [&v](double p) {
        size_t i = (size_t)(p * (v.size() - 1) + 0.5);
        return v[i];
    };
    d.min = v.front();
    d.p01 = at(0.01);
    d.p50 = at(0.50);
    d.p90 = at(0.90);
    d.p99 = at(0.99);
    d.p999 = at(0.999);
    d.max = v.back();
    return d;
}

inline void print_dist(std::FILE* f, const char* label, const Dist& d, const char* unit) {
    if (d.n == 0) {
        std::fprintf(f, "    %-34s даних немає\n", label);
        return;
    }
    std::fprintf(f,
        "    %-34s сер %8.3f  σ %7.3f  |  min %8.3f  p1 %8.3f  p50 %8.3f"
        "  p90 %8.3f  p99 %8.3f  p99.9 %8.3f  max %8.3f  (%s, n=%zu)\n",
        label, d.mean, d.sd, d.min, d.p01, d.p50, d.p90, d.p99, d.p999, d.max,
        unit, d.n);
}

// Інтервали між сусідніми мітками, мс.
inline std::vector<double> intervals_ms(const std::vector<int64_t>& t) {
    std::vector<double> v;
    if (t.size() < 2) return v;
    v.reserve(t.size() - 1);
    for (size_t i = 1; i < t.size(); ++i) {
        if (t[i] > t[i - 1]) v.push_back((t[i] - t[i - 1]) / 1e6);
    }
    return v;
}

inline void PathMeter::report(const char* path) const {
    std::lock_guard<std::mutex> lk(mtx_);

    std::FILE* f = std::fopen(path, "w");
    if (!f) { std::fprintf(stderr, "[вимір] не пишеться %s\n", path); return; }

    std::fprintf(f, "НАСКРІЗНИЙ ВИМІР ТРАКТУ\n");
    std::fprintf(f, "=======================\n\n");

    // --- розгортка ---
    {
        const auto iv = intervals_ms(flips_);
        const Dist d = distribution(iv);
        double span = flips_.size() > 1
                    ? (flips_.back() - flips_.front()) / 1e9 : 0.0;
        std::fprintf(f, "РОЗГОРТКА\n");
        std::fprintf(f, "    показів %zu за %.1f с = %.4f Гц\n",
                     flips_.size(), span, span > 0 ? (flips_.size() - 1) / span : 0.0);
        print_dist(f, "інтервал розгортки", d, "мс");
        std::fprintf(f, "\n");
    }

    for (const Channel& c : ch_) {
        std::vector<double> dec, out2scr, in2scr;
        std::vector<int64_t> outs, showns;
        size_t shown_n = 0;

        for (const Rec& r : c.recs) {
            outs.push_back(r.out_ns);
            if (r.in_ns > 0 && r.out_ns > r.in_ns) dec.push_back((r.out_ns - r.in_ns) / 1e6);
            if (r.shown_ns > 0) {
                shown_n++;
                showns.push_back(r.shown_ns);
                if (r.shown_ns > r.out_ns) out2scr.push_back((r.shown_ns - r.out_ns) / 1e6);
                if (r.in_ns > 0 && r.shown_ns > r.in_ns)
                    in2scr.push_back((r.shown_ns - r.in_ns) / 1e6);
            }
        }

        double span = c.recs.size() > 1
                    ? (c.recs.back().out_ns - c.recs.front().out_ns) / 1e9 : 0.0;

        std::fprintf(f, "КАНАЛ %s\n", c.name.c_str());
        std::fprintf(f, "    у декодер %zu | з декодера %zu (%.4f к/с) | на екран %zu (%.2f%%)\n",
                     c.ins.size(), c.recs.size(),
                     span > 0 ? (c.recs.size() - 1) / span : 0.0, shown_n,
                     c.recs.empty() ? 0.0 : 100.0 * shown_n / c.recs.size());
        std::fprintf(f, "    показів того самого кадру повторно: %llu | показано поза записами: %llu\n",
                     (unsigned long long)c.repeats, (unsigned long long)c.orphans);
        print_dist(f, "інтервал на ВХОДІ декодера", distribution(intervals_ms(c.ins)), "мс");
        print_dist(f, "інтервал на ВИХОДІ декодера", distribution(intervals_ms(outs)), "мс");
        print_dist(f, "крок показаних кадрів", distribution(intervals_ms(showns)), "мс");
        print_dist(f, "ЗАТРИМКА декоду", distribution(dec), "мс");
        print_dist(f, "декодер -> екран", distribution(out2scr), "мс");
        print_dist(f, "НАСКРІЗНА: вхід декодера -> екран", distribution(in2scr), "мс");
        std::fprintf(f, "\n");
    }

    std::fclose(f);
    std::fprintf(stderr, "[вимір] звіт: %s\n", path);

    // СИРІ РЯДИ поруч зі зведенням.
    //
    // Зведення відповідає на "скільки", але не на "як це виглядало в
    // часі". Питання накшталт "фаза справді блукає чи це петля ганяється
    // за шумом виміру" по перцентилях не вирішуються в принципі: там уже
    // немає ні порядку подій, ні того, на якому вікні їх усереднили.
    //
    // Тому поруч лягають самі мітки, у наносекундах монотонного
    // годинника. Далі з ними можна робити що завгодно — рахувати фазу
    // покадрово, дивитись спектр, розкладати на повільне й швидке.
    {
        std::string base(path);
        {
            std::string p = base + ".flips.csv";
            if (std::FILE* g = std::fopen(p.c_str(), "w")) {
                std::fprintf(g, "vblank_ns\n");
                for (int64_t t : flips_) std::fprintf(g, "%lld\n", (long long)t);
                std::fclose(g);
                std::fprintf(stderr, "[вимір] розгортки: %s\n", p.c_str());
            }
        }
        for (const Channel& c : ch_) {
            std::string p = base + "." + c.name + ".csv";
            if (std::FILE* g = std::fopen(p.c_str(), "w")) {
                std::fprintf(g, "in_ns,out_ns,shown_ns\n");
                for (const Rec& r : c.recs) {
                    std::fprintf(g, "%lld,%lld,%lld\n", (long long)r.in_ns,
                                 (long long)r.out_ns, (long long)r.shown_ns);
                }
                std::fclose(g);
                std::fprintf(stderr, "[вимір] канал %s: %s\n", c.name.c_str(), p.c_str());
            }
        }
    }
}

} // namespace vrx::diag

// Точки виміру. У вимкненому стані від них не лишається нічого.
#define VRX_PM_CHANNEL(name)          ::vrx::diag::PathMeter::get().channel(name)
#define VRX_PM_IN(ch, ns)             ::vrx::diag::PathMeter::get().in((ch), (ns))
#define VRX_PM_OUT(ch, out_ns, in_ns) ::vrx::diag::PathMeter::get().out((ch), (out_ns), (in_ns))
#define VRX_PM_SHOWN(ch, prod, vbl)   ::vrx::diag::PathMeter::get().shown((ch), (prod), (vbl))
#define VRX_PM_FLIP(ns)               ::vrx::diag::PathMeter::get().flip((ns))
#define VRX_PM_REPORT()                                                        \
    do {                                                                       \
        const char* p = getenv("VRX_MEASURE_OUT");                             \
        ::vrx::diag::PathMeter::get().report(p ? p : "measure.txt");           \
    } while (0)

#else   // VRX_MEASURE == 0

#define VRX_PM_CHANNEL(name)          (-1)
#define VRX_PM_IN(ch, ns)             ((void)0)
#define VRX_PM_OUT(ch, out_ns, in_ns) ((void)0)
#define VRX_PM_SHOWN(ch, prod, vbl)   ((void)0)
#define VRX_PM_FLIP(ns)               ((void)0)
#define VRX_PM_REPORT()               ((void)0)

#endif
