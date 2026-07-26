#pragma once

// Тимчасове джерело: малює візерунок у справжні dmabuf-буфери.
//
// Потрібне, поки немає декодера. Йде ТИМ САМИМ шляхом, що піде відео:
// dmabuf -> EGLImage -> зовнішня текстура. Тобто перевіряє не заглушку,
// а реальний тракт.
//
// Три буфери, як і має бути в джерела. Кожен помічено власною смугою
// збоку, щоб ротацію було видно оком: якщо смуга стоїть на місці —
// крутиться лише один буфер, і десь помилка.
//
// Візерунок малюється ОДИН РАЗ при старті. Пам'ять dumb-буфера
// некешована, і заливати її щокадру з CPU коштувало б більше, ніж увесь
// бюджет кадру — рівно та проблема, через яку в старому VRX OSD займав
// 70 мс. Справжній декодер писатиме туди залізом, не процесором.

#include "frame_source.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrx::source {

class TestSource final : public FrameSource {
public:
    TestSource(std::string name, int width, int height, float pixel_aspect = 1.0f)
        : name_(std::move(name)), w_(width), h_(height), par_(pixel_aspect) {}

    ~TestSource() override {
        stop();
        for (Buf& b : bufs_) destroy(b);
        if (drm_fd_ >= 0) ::close(drm_fd_);
    }

    const char* name() const override { return name_.c_str(); }

    bool start() override {
        if (running_.load()) return true;
        if (!alloc_buffers()) return false;

        running_.store(true);
        thread_ = std::thread([this] {
            // Власний потік джерела. Справжнє джерело тут крутило б
            // GStreamer; це лише імітує темп появи кадрів.
            while (running_.load(std::memory_order_relaxed)) {
                publish_next();
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
        if (!present_ || ready_ < 0) return false;

        // Звільнення НЕЯВНЕ: рендерер просить новий кадр — отже з
        // попереднім закінчив, і той буфер можна знову віддавати під
        // запис. Безпечно лише тому, що між викликами рендерер чекає
        // fence (див. коментар у FrameSource::acquire).
        if (in_use_ >= 0) free_.push_back(in_use_);
        in_use_ = ready_;
        ready_ = -1;

        out.image = frame_of(bufs_[in_use_]);
        out.pixel_aspect = par_;
        out.where = p_;
        st_.taken++;
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

    // Сигналу немає -> не малюємо нічого.
    void set_present(bool present) {
        std::lock_guard<std::mutex> lk(mtx_);
        present_ = present;
    }

private:
    struct Buf {
        uint32_t handle = 0;
        uint32_t stride = 0;
        uint64_t size = 0;
        int fd = -1;
        uint8_t* px = nullptr;
    };

    display::Frame frame_of(const Buf& b) const {
        display::Frame f;
        f.fourcc = DRM_FORMAT_XRGB8888;
        f.modifier = DRM_FORMAT_MOD_LINEAR;
        f.width = w_;
        f.height = h_;
        f.n_planes = 1;
        f.fd[0] = b.fd;
        f.stride[0] = b.stride;
        f.offset[0] = 0;
        return f;
    }

    bool alloc_buffers() {
        if (!bufs_.empty()) return true;

        drm_fd_ = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) {
            std::fprintf(stderr, "[%s] open(card0): %s\n", name_.c_str(), std::strerror(errno));
            return false;
        }

        bufs_.resize(3);   // writing / ready / in_use
        for (int i = 0; i < 3; ++i) {
            Buf& b = bufs_[i];
            if (drmModeCreateDumbBuffer(drm_fd_, w_, h_, 32, 0,
                                        &b.handle, &b.stride, &b.size) != 0) {
                std::fprintf(stderr, "[%s] CreateDumbBuffer: %s\n",
                             name_.c_str(), std::strerror(errno));
                return false;
            }
            uint64_t off = 0;
            if (drmModeMapDumbBuffer(drm_fd_, b.handle, &off) != 0) return false;
            void* p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd_, (off_t)off);
            if (p == MAP_FAILED) return false;
            b.px = static_cast<uint8_t*>(p);
            if (drmPrimeHandleToFD(drm_fd_, b.handle, DRM_CLOEXEC, &b.fd) != 0) return false;

            paint(b, i);
            free_.push_back(i);
        }
        std::fprintf(stderr, "[%s] 3 буфери %dx%d, крок %u\n",
                     name_.c_str(), w_, h_, bufs_[0].stride);
        return true;
    }

    // Візерунок, за яким видно і межі кадру, і який саме це буфер.
    void paint(Buf& b, int idx) {
        static const uint32_t tint[3] = {0x00301810u, 0x00103018u, 0x00101830u};
        for (int y = 0; y < h_; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(b.px + (size_t)y * b.stride);
            for (int x = 0; x < w_; ++x) {
                // Градієнт: видно, чи не перевернуті UV і чи не дзеркальні.
                uint32_t r = (uint32_t)(255.0f * x / (w_ - 1));
                uint32_t g = (uint32_t)(255.0f * y / (h_ - 1));
                row[x] = (r << 16) | (g << 8) | 0x20u;

                // Рамка в 4 пікселі — видно точні межі кадру на екрані.
                if (x < 4 || y < 4 || x >= w_ - 4 || y >= h_ - 4) row[x] = 0x00FFFFFFu;
            }
            // Смуга-маркер буфера: своя позиція в кожного з трьох.
            const int bar_x = 20 + idx * 40;
            for (int x = bar_x; x < bar_x + 30 && x < w_; ++x) {
                row[x] = tint[idx] | 0x00808080u;
            }
        }
    }

    // Декодер "закінчив кадр": writing -> ready, а старий ready, якщо
    // його не встигли забрати, повертається в обіг. Ось тут і працює
    // "перемагає найсвіжіший".
    void publish_next() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (free_.empty()) return;

        int next = free_.back();
        free_.pop_back();
        if (ready_ >= 0) {
            free_.push_back(ready_);
            st_.dropped++;
        }
        ready_ = next;
        st_.produced++;
    }

    void destroy(Buf& b) {
        if (b.px) { munmap(b.px, b.size); b.px = nullptr; }
        if (b.fd >= 0) { ::close(b.fd); b.fd = -1; }
        if (b.handle && drm_fd_ >= 0) { drmModeDestroyDumbBuffer(drm_fd_, b.handle); b.handle = 0; }
    }

    std::string name_;
    mutable std::mutex mtx_;

    int w_, h_;
    float par_;
    bool present_ = true;
    layout::Placement p_{};
    SourceStats st_{};

    int drm_fd_ = -1;
    std::vector<Buf> bufs_;
    std::vector<int> free_;
    int ready_ = -1;
    int in_use_ = -1;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace vrx::source
