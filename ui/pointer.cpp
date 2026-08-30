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
#include <utility>
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
    std::vector<std::string> fd_paths;   // паралельно fds — щоб не відкрити те саме двічі
    std::thread th;
    std::atomic<bool> running{false};

    mutable std::mutex mtx;
    PointerState st{};
    int w = 0, h = 0;

    // Розміри всіх екранів у порядку ролей. Порожньо або один — кільця
    // немає, курсор просто впирається в край, як було завжди.
    std::vector<std::pair<int, int>> screens;

    explicit Impl(Config c) : cfg(c) {}

    // Перебирає /dev/input і відкриває НОВІ пристрої-вказівники, яких ще
    // немає в fds. Повертає, скільки додав. Викликається і на старті, і
    // періодично з циклу — щоб мишу, під'єднану вже під час роботи,
    // підхопило само (udev/X тут немає, слухати нікого).
    int scan_once(bool verbose) {
        DIR* d = ::opendir("/dev/input");
        if (!d) {
            if (verbose) std::fprintf(stderr, "[миша] /dev/input не читається\n");
            return 0;
        }
        int added = 0;
        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.compare(0, 5, "event") != 0) continue;
            const std::string path = "/dev/input/" + name;

            // Уже відкритий — пропускаємо (інакше при кожному скані
            // накопичували б дублікати того самого пристрою).
            bool known = false;
            for (const auto& p : fd_paths) if (p == path) { known = true; break; }
            if (known) continue;

            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            if (!looks_like_mouse(fd)) { ::close(fd); continue; }

            char dev_name[256] = {};
            ioctl(fd, EVIOCGNAME(sizeof(dev_name) - 1), dev_name);
            std::fprintf(stderr, "[миша] %s: %s\n", path.c_str(),
                         dev_name[0] ? dev_name : "?");
            fds.push_back(fd);
            fd_paths.push_back(path);
            added++;
        }
        ::closedir(d);

        std::lock_guard<std::mutex> lk(mtx);
        st.present = !fds.empty();
        return added;
    }

    void open_devices() {
        if (scan_once(true) == 0) {
            std::fprintf(stderr, "[миша] пристроїв поки немає — чекаю підключення\n");
        }
    }

    // Прибрати fd за індексом: закрити й викинути з fds/fd_paths.
    void drop_index(size_t i) {
        ::close(fds[i]);
        fds.erase(fds.begin() + i);
        fd_paths.erase(fd_paths.begin() + i);
    }

    void loop() {
        std::vector<pollfd> pfds;
        auto rebuild_pfds = [&] {
            pfds.clear();
            pfds.reserve(fds.size());
            for (int fd : fds) pfds.push_back(pollfd{fd, POLLIN, 0});
        };
        rebuild_pfds();

        auto next_scan = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        while (running.load(std::memory_order_relaxed)) {
            // Досканування — ЛИШЕ КОЛИ МИШІ НЕМАЄ. scan_once() відкриває й
            // опитує ioctl'ами всі /dev/input/* — це десятки мілісекунд, і
            // на цей час потік НЕ ЧИТАЄ подій: курсор завмирав рівно на
            // секунду (період скану), а кліки прилітали пачкою після.
            // Поки миша працює, її від'єднання ловить POLLHUP нижче, тож
            // сканувати нема чого — і читання подій ніщо не перериває.
            if (pfds.empty()) {
                const auto now_tp = std::chrono::steady_clock::now();
                if (now_tp >= next_scan) {
                    next_scan = now_tp + std::chrono::seconds(1);
                    if (scan_once(false) > 0) rebuild_pfds();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            // Таймаут, а не вічне чекання: без нього зупинка висіла б до
            // першого руху мишею.
            const int rc = ::poll(pfds.data(), pfds.size(), 100);
            if (rc <= 0) continue;

            // Від'єднаний пристрій: ядро віддає POLLHUP/POLLERR. Закриваємо
            // й перебудовуємо список — щоб при повторному під'єднанні
            // (інший event-вузол) скан підхопив його як новий.
            bool dropped = false;
            for (size_t i = pfds.size(); i-- > 0;) {
                if (pfds[i].revents & (POLLHUP | POLLERR)) { drop_index(i); dropped = true; }
            }
            if (dropped) {
                rebuild_pfds();
                std::lock_guard<std::mutex> lk(mtx);
                st.present = !fds.empty();
                continue;
            }

            int dx = 0, dy = 0, dwheel = 0;
            bool btn_changed = false, btn_down = false;
            bool rbtn_changed = false, rbtn_down = false;
            bool mbtn_changed = false, mbtn_down = false;

            for (auto& p : pfds) {
                if (!(p.revents & POLLIN)) continue;
                input_event ev[32];
                const ssize_t n = ::read(p.fd, ev, sizeof(ev));
                if (n <= 0) continue;
                for (size_t i = 0; i < n / sizeof(input_event); ++i) {
                    if (ev[i].type == EV_REL) {
                        if (ev[i].code == REL_X) dx += ev[i].value;
                        else if (ev[i].code == REL_Y) dy += ev[i].value;
                        else if (ev[i].code == REL_WHEEL) dwheel += ev[i].value;
                    } else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) {
                        btn_changed = true;
                        btn_down = ev[i].value != 0;
                    } else if (ev[i].type == EV_KEY && ev[i].code == BTN_RIGHT) {
                        rbtn_changed = true;
                        rbtn_down = ev[i].value != 0;
                    } else if (ev[i].type == EV_KEY && ev[i].code == BTN_MIDDLE) {
                        mbtn_changed = true;
                        mbtn_down = ev[i].value != 0;
                    }
                }
            }

            if (!dx && !dy && !dwheel && !btn_changed && !rbtn_changed
                && !mbtn_changed) continue;

            std::lock_guard<std::mutex> lk(mtx);
            st.wheel += dwheel;
            if (w > 0 && h > 0) {
                st.x += (int)(dx * cfg.speed);
                st.y += (int)(dy * cfg.speed);

                // ПЕРЕХІД МІЖ ЕКРАНАМИ ЧЕРЕЗ БІЧНИЙ КРАЙ.
                //
                // Кільце: вийшов ліворуч — з'явився праворуч на
                // сусідньому. Так не треба знати, який монітор де стоїть
                // фізично, і обійти всі екрани можна в будь-який бік.
                //
                // Вертикаль переносимо ЧАСТКОЮ, а не пікселями: екрани
                // різної висоти, і перехід на середині має лишатись
                // серединою.
                const int n = (int)screens.size();
                if (n > 1) {
                    while (st.x < 0 || st.x > w - 1) {
                        const double fy = (h > 1) ? double(st.y) / double(h - 1) : 0.0;
                        const bool left = st.x < 0;
                        st.screen = left ? (st.screen - 1 + n) % n : (st.screen + 1) % n;
                        w = screens[(size_t)st.screen].first;
                        h = screens[(size_t)st.screen].second;
                        st.x = left ? w - 1 : 0;
                        st.y = (int)(fy * double(h - 1) + 0.5);
                    }
                }

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
            if (rbtn_changed) {
                if (rbtn_down && !st.right) st.rclicks++;
                st.right = rbtn_down;
            }
            if (mbtn_changed) {
                if (mbtn_down && !st.middle) st.mclicks++;
                st.middle = mbtn_down;
            }
        }
    }
};

// ---------------------------------------------------------------------

Pointer::Pointer(Config cfg) : impl_(new Impl(cfg)) {}
Pointer::Pointer() : impl_(new Impl(Config())) {}

Pointer::~Pointer() { stop(); }

void Pointer::set_screens(const std::vector<std::pair<int, int>>& sizes) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->screens = sizes;
    if (impl_->screens.empty()) return;
    if (impl_->st.screen >= (int)impl_->screens.size()) {
        // Екран, на якому стояв курсор, зник — забираємо його на перший.
        impl_->st.screen = 0;
    }
    const auto& sc = impl_->screens[(size_t)impl_->st.screen];
    impl_->w = sc.first;
    impl_->h = sc.second;
    if (impl_->st.x > impl_->w - 1) impl_->st.x = impl_->w - 1;
    if (impl_->st.y > impl_->h - 1) impl_->st.y = impl_->h - 1;
}

void Pointer::set_bounds(int width, int height) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->w = width;
    impl_->h = height;
    // Перший раз — ставимо курсор у нижній лівий кут, а не в центр.
    // Миша тут лише для налаштування (кнопка редактора у верхньому куті);
    // посеред екрана курсор просто заважав би дивитись відео. Кут —
    // найменш помітне місце, звідки його легко підхопити, коли треба.
    if (impl_->st.x == 0 && impl_->st.y == 0) {
        impl_->st.x = 2;
        impl_->st.y = height - 3;
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
    impl_->fd_paths.clear();
}

PointerState Pointer::state() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

} // namespace vrx::ui
