#include "link_monitor.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace vrx::diag {
namespace {

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

std::string wall_time() {
    std::time_t t = std::time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

} // namespace

struct LinkMonitor::Impl {
    Config cfg;
    std::thread th;
    std::atomic<bool> running{false};
    int fd = -1;
    FILE* log = nullptr;

    mutable std::mutex mtx;
    LinkStats st{};

    // Очікуваний номер наступного пакета. 16 біт із переповненням, тож
    // порівнювати треба ЗНАКОВОЮ різницею, а не звичайною: між 65535 і 0
    // відстань один, а не 65535.
    uint16_t expect = 0;
    bool have_expect = false;
    int64_t last_arrival = 0;

    explicit Impl(Config c) : cfg(std::move(c)) {}

    bool open_socket() {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((uint16_t)cfg.udp_port);
        if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            std::fprintf(stderr, "[лінк %s] не став на порт %d: %s\n",
                         cfg.name.c_str(), cfg.udp_port, std::strerror(errno));
            ::close(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    void report(const char* fmt, ...) {
        char line[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        std::fprintf(stderr, "[лінк %s] %s\n", cfg.name.c_str(), line);
        if (log) {
            std::fprintf(log, "%s %s\n", wall_time().c_str(), line);
            std::fflush(log);   // інцидент має пережити зникнення живлення
        }
    }

    void loop() {
        char buf[2048];

        while (running.load(std::memory_order_relaxed)) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, 200) <= 0) continue;

            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n < 12) continue;               // не RTP

            const int64_t now = now_ms();
            const uint16_t seq = ntohs(*(uint16_t*)(buf + 2));

            // Пауза в ПРИХОДІ. Оцінюємо ДО оновлення очікуваного номера:
            // саме перший пакет після тиші й каже, хто винен.
            int64_t gap = 0;
            if (last_arrival > 0) gap = now - last_arrival;
            last_arrival = now;

            std::lock_guard<std::mutex> lk(mtx);
            st.packets++;

            bool lost_here = false;
            if (!have_expect) {
                have_expect = true;
            } else {
                const int16_t delta = (int16_t)(seq - expect);
                if (delta > 0) {
                    st.lost += (uint64_t)delta;
                    lost_here = true;
                } else if (delta < 0) {
                    // Або запізнілий, або дубль. Розрізняти тонше сенсу
                    // немає: обидва означають те саме — порядок порушено.
                    if (delta == -1) st.dups++;
                    else             st.reordered++;
                }
            }
            expect = (uint16_t)(seq + 1);

            if (st.packets > 0) {
                st.loss_percent = 100.0 * double(st.lost) / double(st.lost + st.packets);
            }

            if (gap >= cfg.gap_ms) {
                st.gaps++;
                st.last_gap_ms = gap;
                st.last_gap_had_loss = lost_here;
                if (gap > st.worst_gap_ms) st.worst_gap_ms = gap;

                if (lost_here) {
                    st.gaps_with_loss++;
                    report("тиша %lld мс, у нумерації ДІРА -> пакети загубились у дорозі (лінк)",
                           (long long)gap);
                } else {
                    st.gaps_clean++;
                    report("тиша %lld мс, нумерація ЦІЛА -> камера нічого не слала (борт)",
                           (long long)gap);
                }
            }
        }
    }
};

LinkMonitor::LinkMonitor(Config cfg) : impl_(new Impl(std::move(cfg))) {}

LinkMonitor::~LinkMonitor() { stop(); }

bool LinkMonitor::start() {
    if (impl_->running.load()) return true;
    if (!impl_->open_socket()) return false;

    if (!impl_->cfg.log_path.empty()) {
        impl_->log = ::fopen(impl_->cfg.log_path.c_str(), "a");
        if (!impl_->log) {
            std::fprintf(stderr, "[лінк %s] журнал %s не відкрився: %s\n",
                         impl_->cfg.name.c_str(), impl_->cfg.log_path.c_str(),
                         std::strerror(errno));
        }
    }

    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[лінк %s] стежу за портом %d, поріг тиші %d мс\n",
                 impl_->cfg.name.c_str(), impl_->cfg.udp_port, impl_->cfg.gap_ms);
    return true;
}

void LinkMonitor::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
    if (impl_->log) { ::fclose(impl_->log); impl_->log = nullptr; }
}

LinkStats LinkMonitor::stats() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->st;
}

} // namespace vrx::diag
