#include "layout_control.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

namespace vrx::control {
namespace {

// Число з каналу -> якір. Порядок рядками, той самий, що в enum Anchor:
// нумерацію тримаємо ОДНУ на весь проєкт, інакше рано чи пізно з'явиться
// друга таблиця відповідності й розійдеться з першою.
layout::Anchor anchor_from(float v) {
    const int i = (int)std::lround(v);
    switch (i) {
        case 0: return layout::Anchor::TopLeft;
        case 1: return layout::Anchor::TopCenter;
        case 2: return layout::Anchor::TopRight;
        case 3: return layout::Anchor::CenterLeft;
        case 4: return layout::Anchor::Center;
        case 5: return layout::Anchor::CenterRight;
        case 6: return layout::Anchor::BottomLeft;
        case 7: return layout::Anchor::BottomCenter;
        case 8: return layout::Anchor::BottomRight;
        default: return layout::Anchor::Center;   // сміття -> найбезпечніше
    }
}

const char* anchor_name(layout::Anchor a) {
    switch (a) {
        case layout::Anchor::TopLeft:      return "лівий верх";
        case layout::Anchor::TopCenter:    return "середина верху";
        case layout::Anchor::TopRight:     return "правий верх";
        case layout::Anchor::CenterLeft:   return "середина ліва";
        case layout::Anchor::Center:       return "центр";
        case layout::Anchor::CenterRight:  return "середина права";
        case layout::Anchor::BottomLeft:   return "лівий низ";
        case layout::Anchor::BottomCenter: return "середина низу";
        case layout::Anchor::BottomRight:  return "правий низ";
    }
    return "?";
}

bool same(const layout::Placement& a, const layout::Placement& b) {
    auto eq = [](float x, float y) { return std::fabs(x - y) < 1e-4f; };
    return eq(a.x, b.x) && eq(a.y, b.y) && eq(a.w, b.w) && eq(a.h, b.h) &&
           a.anchor == b.anchor && a.z == b.z && a.enabled == b.enabled;
}

} // namespace

struct LayoutControl::Impl {
    Config cfg;
    VtTelemetryStorage* storage = nullptr;
    std::vector<Bound> bounds;
    std::vector<layout::Placement> applied;   // що зараз стоїть у джерела

    std::thread th;
    std::atomic<bool> running{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    explicit Impl(Config c) : cfg(std::move(c)) {}

    // Одне поле з каналу. Повертає false, якщо каналу НЕ БУЛО ЖОДНОГО
    // РАЗУ — тоді діє вкомпільоване значення.
    //
    // get_value(), а не перевірка протухання, і це головне рішення тут:
    // телеметрія в польоті регулярно замовкає на секунди, і якби
    // мовчання означало "даних немає", картинка перекидалась би на типову
    // розкладку на кожному пробої лінка. Останнє відоме значення —
    // єдина розумна поведінка: пілот виставив розкладку, вона стоїть.
    bool read(uint8_t ch, float* out) const {
        return storage->get_value(ch, out);
    }

    void tick() {
        // СЦЕНАРІЙ "ОСТАННІЙ ВЦІЛІЛИЙ". Спершу рахуємо, скільки джерел
        // мають сигнал ПРЯМО ЗАРАЗ, і хто саме єдиний. Робиться раз на
        // такт, до застосування розкладок.
        int with_signal = 0;
        int solo_idx = -1;
        for (size_t i = 0; i < bounds.size(); ++i) {
            if (bounds[i].source && bounds[i].source->has_signal()) {
                with_signal++;
                solo_idx = (int)i;
            }
        }
        const bool solo_mode = cfg.solo_fullscreen && with_signal == 1;

        for (size_t i = 0; i < bounds.size(); ++i) {
            const Bound& b = bounds[i];
            layout::Placement p = b.fallback;

            float v = 0.f;
            if (read(b.ch_w, &v)) p.w = v;
            if (read(b.ch_h, &v)) p.h = v;
            if (read(b.ch_x, &v)) p.x = v;
            if (read(b.ch_y, &v)) p.y = v;
            if (read(b.ch_anchor, &v)) p.anchor = anchor_from(v);

            // Числа прийшли по радіо: перед застосуванням приводимо їх до
            // придатного вигляду, а не сподіваємось на відправника.
            p = layout::sanitize(p);

            // Єдиний вцілілий потік — на весь екран, поверх розкладки.
            // z лишаємо з fallback, щоб порядок був осмислений, якщо друге
            // джерело саме зараз оживає між двома тактами.
            const bool is_solo = solo_mode && (int)i == solo_idx;
            if (is_solo) {
                p.x = 0.5f; p.y = 0.5f;
                p.w = 1.0f; p.h = 1.0f;
                p.anchor = layout::Anchor::Center;
                p.enabled = true;
            }

            if (same(p, applied[i])) continue;
            applied[i] = p;
            if (b.source) b.source->set_placement(p);

            std::fprintf(stderr,
                "[розкладка] %s: %s коробка %.3fx%.3f, якір %s у (%.3f, %.3f)\n",
                b.name.c_str(), is_solo ? "САМ НА ЕКРАНІ —" : "",
                p.w, p.h, anchor_name(p.anchor), p.x, p.y);
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

LayoutControl::LayoutControl(Config cfg) : impl_(new Impl(std::move(cfg))) {}

LayoutControl::~LayoutControl() { stop(); }

void LayoutControl::bind(Bound b) {
    impl_->bounds.push_back(std::move(b));
    // Порожнє розміщення як "ще нічого не застосовано": перший такт
    // неодмінно відрізнить його від будь-якого справжнього і застосує.
    layout::Placement none;
    none.w = -1.f;
    impl_->applied.push_back(none);
}

bool LayoutControl::start(VtTelemetryStorage& storage) {
    if (impl_->running.load()) return true;
    impl_->storage = &storage;
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[розкладка] керування з телеметрії: %zu джерел,"
                 " опитування раз на %d мс\n",
                 impl_->bounds.size(), impl_->cfg.period_ms);
    return true;
}

void LayoutControl::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->wake_cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace vrx::control
