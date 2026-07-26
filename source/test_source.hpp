#pragma once

// Найпростіше джерело: нічого не декодує, лише повідомляє розмір.
//
// Потрібне, поки немає декодера — щоб перевірити реєстрацію, розміщення,
// вписування за пропорцією і порядок за z на живому екрані. Кадру як
// буфера тут немає, тож рендерер поки заливає його місце кольором.
//
// Заразом дає перемикач наявності сигналу: present(false) має прибрати
// джерело з екрана повністю, без жодної заглушки.

#include "frame_source.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace vrx::source {

class TestSource final : public FrameSource {
public:
    TestSource(std::string name, int width, int height, float pixel_aspect = 1.0f)
        : name_(std::move(name)), w_(width), h_(height), par_(pixel_aspect) {}

    ~TestSource() override { stop(); }

    const char* name() const override { return name_.c_str(); }

    bool start() override {
        if (running_.exchange(true)) return true;
        thread_ = std::thread([this] {
            // Власний потік джерела. Справжнє джерело тут крутило б
            // GStreamer; це лише імітує темп появи кадрів.
            while (running_.load(std::memory_order_relaxed)) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    st_.produced++;
                    if (!ready_taken_) st_.dropped++;   // попередній не забрали
                    ready_taken_ = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
        return true;
    }

    void stop() override {
        if (running_.exchange(false) && thread_.joinable()) thread_.join();
    }

    bool acquire(SourceFrame& out) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!present_) return false;      // сигналу немає -> не малюємо нічого

        out.image = display::Frame{};
        out.image.width = w_;
        out.image.height = h_;
        out.pixel_aspect = par_;
        out.where = p_;

        st_.taken++;
        ready_taken_ = true;
        return true;
    }

    layout::Placement placement() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return p_;
    }

    void set_placement(const layout::Placement& p) override {
        std::lock_guard<std::mutex> lk(mtx_);
        p_ = p;
    }

    SourceStats stats() const override {
        std::lock_guard<std::mutex> lk(mtx_);
        return st_;
    }

    // --- керування для перевірки ---

    void set_present(bool present) {
        std::lock_guard<std::mutex> lk(mtx_);
        present_ = present;
    }

    // Імітує зміну роздільності на ходу — те, що робить електронна
    // стабілізація, ріжучи кадр кропом.
    void set_size(int w, int h) {
        std::lock_guard<std::mutex> lk(mtx_);
        w_ = w; h_ = h;
    }

private:
    std::string name_;
    mutable std::mutex mtx_;

    int w_, h_;
    float par_;
    bool present_ = true;
    bool ready_taken_ = true;
    layout::Placement p_{};
    SourceStats st_{};

    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace vrx::source
