#!/usr/bin/env python3
"""Розбір сирих рядів, які лишає VRX_MEASURE.

    ./scripts/analyse_measure.py measure.txt [ще_один.txt ...]

Приймає ШЛЯХ ЗВІТУ (той, що в VRX_MEASURE_OUT); поруч із ним лежать
measure.txt.flips.csv і measure.txt.<канал>.csv, які й читаються. Кілька
аргументів — це A/B: рядки лягають в одну таблицю.

ЧОМУ ЦЕ ОКРЕМИЙ СКРИПТ, А НЕ ЧАСТИНА ЗВІТУ. Звіт у програмі рахує
розподіли — те, що потрібно завжди. Тут же те, що потрібно, коли щось
розбираєш: розділення "фаза справді рухається" і "це шум виміру", а воно
вимагає рішень, які не варто вбудовувати в бойовий код (де межа
захоплення, на якому вікні знімати тренд).

ЯК РАХУЄТЬСЯ РУХ ФАЗИ — головне число цього скрипта.

  1. Фаза кожного кадру = скільки минуло від попередньої розгортки до
     виходу кадру з декодера.
  2. Кадри зводяться у вікна по 250 мс — рівно такі, які бачить
     контролер, — і в кожному береться КРУГОВЕ середнє. Кругове, бо фаза
     живе на колі: 0.1 мс і 16.6 мс сусіди, а не протилежності. Звичайне
     середнє тут дає сміття, щойно вікно ляже на обгортку, і саме на цьому
     я вже раз обпікся, порівнюючи прогін із вимкненою петлею.
  3. Ряд середніх розгортається (додаванням кроку по найкоротшій дузі).
  4. На блоках по 10 с знімається ЛІНІЙНИЙ ТРЕНД. Тренд — це залишковий
     дрейф частоти, він цікавий окремо; блукання — це те, що лишається.
  5. З дисперсії залишку віднімається дисперсія ШУМУ САМОГО ВИМІРУ
     (σ_вікна / √n). Без цього кроку "рух" ніколи не буде нулем, навіть
     якщо фаза стоїть як укопана.

Результат нижче шуму друкується як 0.000 і означає рівно це: блукання
менше за те, що ми здатні розгледіти цим інструментом.
"""

import bisect
import math
import statistics as st
import sys

WINDOW_S = 0.25        # вікно усереднення, як у PhaseMeter
DETREND_S = 10.0       # на якому відрізку знімати лінійний дрейф
SETTLED_S = 180.0      # від якої секунди вважати режим усталеним
ACQUIRE_S = 60.0       # скільки перших секунд вважати захопленням


def load(base, channel):
    with open(base + ".flips.csv") as f:
        flips = [int(x) for x in f.read().split()[1:]]
    rows = []
    with open("%s.%s.csv" % (base, channel)) as f:
        for i, line in enumerate(f):
            if i == 0:
                continue
            a = line.strip().split(",")
            rows.append((int(a[0]), int(a[1]), int(a[2])))
    return flips, rows


def period_ms(flips):
    d = sorted(flips[i + 1] - flips[i] for i in range(len(flips) - 1))
    return d[len(d) // 2] / 1e6


def windows(flips, rows, t0, t1):
    """Кругові середні по вікнах + оцінка шуму середнього.

    Мінімум кадрів у вікні береться від ЧАСТОТИ КАНАЛУ, а не константою.
    Жорстке "не менше 8" мовчки викидало весь другий канал: на 25 к/с у
    вікно 250 мс потрапляє ~6 кадрів, жодне вікно не проходило, і функція
    чесно повертала нуль — який у таблиці читався як "блукання немає".
    Нуль, що означає "не порахувалось", гірший за відсутність рядка.
    """
    per = period_ms(flips)
    buckets = {}
    for _, produced, _ in rows:
        i = bisect.bisect_right(flips, produced) - 1
        if i < 0:
            continue
        ph = (produced - flips[i]) / 1e6
        t = (produced - flips[0]) / 1e9
        if ph < per * 1.5 and t0 <= t < t1:
            buckets.setdefault(int(t / WINDOW_S), []).append(ph)

    if not buckets:
        return [], [], 0.0, per
    expected = st.mean([len(v) for v in buckets.values()])
    min_n = max(4, int(expected * 0.7))
    keys = sorted(k for k in buckets if len(buckets[k]) >= min_n)
    means, var, ns = [], [], []
    for k in keys:
        vs = buckets[k]
        n = len(vs)
        s = sum(math.sin(2 * math.pi * v / per) for v in vs) / n
        c = sum(math.cos(2 * math.pi * v / per) for v in vs) / n
        m = math.atan2(s, c) * per / (2 * math.pi)
        means.append(m + per if m < 0 else m)
        R = min(max(math.hypot(s, c), 1e-9), 1.0)
        sd = math.sqrt(-2 * math.log(R)) * per / (2 * math.pi)
        var.append(sd * sd)
        ns.append(n)

    if not means:
        return [], [], 0.0, per

    # Розгортка: крок береться по найкоротшій дузі.
    unwrapped, acc, prev = [], 0.0, None
    for m in means:
        if prev is not None:
            d = m - prev
            if d > per * 0.5:
                d -= per
            if d < -per * 0.5:
                d += per
            acc += d
        prev = m
        unwrapped.append(acc)

    noise = math.sqrt(st.mean(var) / st.mean(ns))
    return [k * WINDOW_S for k in keys], unwrapped, noise, per


def motion_and_drift(flips, rows, t0, t1):
    """Повертає (рух, дрейф, шум) або None, якщо вікон замало."""
    ts, un, noise, _ = windows(flips, rows, t0, t1)
    if len(un) < 8:
        return None
    drift = (un[-1] - un[0]) / (ts[-1] - ts[0])
    res, block = [], int(DETREND_S / WINDOW_S)
    for s0 in range(0, len(un) - block, block):
        xs, ys = ts[s0:s0 + block], un[s0:s0 + block]
        mx, my = st.mean(xs), st.mean(ys)
        den = sum((x - mx) ** 2 for x in xs)
        k = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0
        res += [y - (my + k * (x - mx)) for x, y in zip(xs, ys)]
    spread = st.pstdev(res) if len(res) > 1 else 0.0
    return math.sqrt(max(spread * spread - noise * noise, 0.0)), drift, noise


def operational(rows, per, t0, t1):
    """Що з цього вийшло для глядача, а не для регулятора."""
    start = rows[0][1]
    sel = [r for r in rows if t0 <= (r[1] - start) / 1e9 < t1]
    if not sel:
        return None
    shown = [r for r in sel if r[2] > 0]
    if not shown:
        return 0.0, 0.0, 0.0
    lat = [(r[2] - r[1]) / 1e6 for r in shown]
    sh = sorted(r[2] for r in shown)
    steps = [(sh[i + 1] - sh[i]) / 1e6 for i in range(len(sh) - 1)]
    # "Крок у нормі" — кадр прожив стільки, скільки для цього каналу
    # типово: не повторився зайвий раз і не пропав.
    #
    # Норма береться від ВЛАСНОГО типового кроку каналу, а не від періоду
    # розгортки. Для основного каналу це те саме — він іде кадр у кадр із
    # розгорткою. Для другого (25 к/с на 59 Гц) кадр законно живе то дві
    # розгортки, то три, і мірка "рівно один період" давала 0.06% норми,
    # тобто описувала арифметику, а не якість.
    typical = sorted(steps)[len(steps) // 2] if steps else per
    ok = sum(1 for s in steps if typical * 0.5 <= s < typical * 1.5)
    return (100.0 * len(shown) / len(sel), st.mean(lat),
            100.0 * ok / len(steps) if steps else 0.0)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    for channel in ("h265", "pip"):
        header = False
        for base in argv[1:]:
            try:
                flips, rows = load(base, channel)
            except OSError:
                continue
            if not header:
                print("\nКАНАЛ %s" % channel)
                if channel != "h265":
                    print("  (канал не підпорядкований петлі — синхронізуватись можна")
                    print("   рівно з одним передавачем; велике \"рух\" тут норма)")
                print("%-26s | %-30s | %-30s" % ("", "ЗАХОПЛЕННЯ 0..%.0f с" % ACQUIRE_S,
                                                 "УСТАЛЕНИЙ від %.0f с" % SETTLED_S))
                print("%-26s | %7s %8s %7s %6s | %7s %8s %7s %6s"
                      % ("прогін", "екран%", "затримка", "крок%", "рух",
                         "екран%", "затримка", "крок%", "рух"))
                header = True

            per = period_ms(flips)
            end = (rows[-1][1] - rows[0][1]) / 1e9
            a = operational(rows, per, 0, ACQUIRE_S)
            b = operational(rows, per, SETTLED_S, end)
            acq = motion_and_drift(flips, rows, 0, ACQUIRE_S)
            set_ = motion_and_drift(flips, rows, SETTLED_S, end)
            fmt = lambda m: "     —" if m is None else "%6.3f" % m[0]
            name = base.rsplit("/", 1)[-1]
            print("%-26s | %7.2f %8.2f %7.2f %s | %7.2f %8.2f %7.2f %s"
                  % (name, a[0], a[1], a[2], fmt(acq), b[0], b[1], b[2], fmt(set_)))
            if set_ is not None:
                print("%-26s   залишковий дрейф %+.2f мс/с | шум виміру %.3f мс | період %.4f мс"
                      % ("", set_[1], set_[2], per))
            else:
                print("%-26s   вікон замало для оцінки руху фази | період %.4f мс"
                      % ("", per))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
