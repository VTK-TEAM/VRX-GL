#include "pointer.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vrx::ui {
namespace {

// Чи має пристрій те, що робить його вказівником: відносні осі X/Y і
// ліву кнопку. Питаємо саме здатності, а не ім'я — імена в мишей різні,
// а поводяться вони однаково.
bool looks_like_mouse(int fd) {
    unsigned long ev_bits[(EV_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) return false;
    auto has = [](const unsigned long* b, int bit) {
        return (b[bit / (8 * sizeof(long))] >> (bit % (8 * sizeof(long)))) & 1UL;
    };
    if (!has(ev_bits, EV_REL) || !has(ev_bits, EV_KEY)) return false;

    unsigned long rel_bits[(REL_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0) return false;
    if (!has(rel_bits, REL_X) || !has(rel_bits, REL_Y)) return false;

    unsigned long key_bits[(KEY_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) return false;
    return has(key_bits, BTN_LEFT);
}

} // namespace

struct Pointer::Impl {
    Config cfg;

    std::vector<int> fds;
    std::thread th;
    std::atomic<bool> running{false};

    mutable std::mutex mtx;
    PointerState st{};
    int w = 0, h = 0;

    explicit Impl(Config c) : cfg(c) {}

    void open_devices() {
        DIR* d = ::opendir("/dev/input");
        if (!d) {
            std::fprintf(stderr, "[миша] /dev/input не читається\n");
            return;
        }
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.compare(0, 5, "event") != 0) continue;
            const std::string path = "/dev/input/" + name;

            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (!looks_like_mouse(fd)) { ::close(fd); continue; }

            char dev_name[256] = {};
            ioctl(fd, EVIOCGNAME(sizeof(dev_name) - 1), dev_name);
            std::fprintf(stderr, "[миша] %s: %s\n", path.c_str(),
                         dev_name[0] ? dev_name : "?");
            fds.push_back(fd);
        }
        ::closedir(d);

        std::lock_guard<std::mutex> lk(mtx);
        st.present = !fds.empty();
        if (fds.empty()) {
            std::fprintf(stderr, "[миша] пристроїв не знайдено\n");
        }
    }

    void loop() {
        std::vector<pollfd> pfds;
        pfds.reserve(fds.size());
        for (int fd : fds) pfds.push_back(pollfd{fd, POLLIN, 0});

        while (running.load(std::memory_order_relaxed)) {
            if (pfds.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            // Таймаут, а не вічне чекання: без нього зупинка висіла б до
            // першого руху мишею.
            const int rc = ::poll(pfds.data(), pfds.size(), 100);
            if (rc <= 0) continue;

            int dx = 0, dy = 0;
            bool btn_changed = false, btn_down = false;

            for (auto& p : pfds) {
                if (!(p.revents & POLLIN)) continue;
                input_event ev[32];
                const ssize_t n = ::read(p.fd, ev, sizeof(ev));
                if (n <= 0) continue;
                for (size_t i = 0; i < n / sizeof(input_event); ++i) {
                    if (ev[i].type == EV_REL) {
                        if (ev[i].code == REL_X) dx += ev[i].value;
                        else if (ev[i].code == REL_Y) dy += ev[i].value;
                    } else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) {
                        btn_changed = true;
                        btn_down = ev[i].value != 0;
                    }
                }
            }

            if (!dx && !dy && !btn_changed) continue;

            std::lock_guard<std::mutex> lk(mtx);
            if (w > 0 && h > 0) {
                st.x += (int)(dx * cfg.speed);
                st.y += (int)(dy * cfg.speed);
                if (st.x < 0) st.x = 0;
                if (st.y < 0) st.y = 0;
                if (st.x > w - 1) st.x = w - 1;
                if (st.y > h - 1) st.y = h - 1;
            }
            if (btn_changed) {
                // Рахуємо саме НАТИСКАННЯ: відпускання подією не є, а
                // подвійне спрацювання на одному кліку — класика.
                if (btn_down && !st.left) st.clicks++;
                st.left = btn_down;
            }
        }
    }
};

// ---------------------------------------------------------------------

Pointer::Pointer(Config cfg) : impl_(new Impl(cfg)) {}
Pointer::Pointer() : impl_(new Impl(Config())) {}

Pointer::~Pointer() { stop(); }

void Pointer::set_bounds(int width, int height) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->w = width;
    impl_->h = height;
    // Перший раз — ставимо курсор у центр, щоб його не довелося шукати.
    if (impl_->st.x == 0 && impl_->st.y == 0) {
        impl_->st.x = width / 2;
        impl_->st.y = height / 2;
    }
    if (impl_->st.x > width - 1) impl_->st.x = width - 1;
    if (impl_->st.y > height - 1) impl_->st.y = height - 1;
}

bool Pointer::start() {
    if (impl_->running.load()) return true;
    impl_->open_devices();
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    return true;
}

void Pointer::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
    for (int fd : impl_->fds) ::close(fd);
    impl_->fds.clear();
}

PointerState Pointer::state() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

} // namespace vrx::ui
