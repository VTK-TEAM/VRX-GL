#include "record/blackbox.hpp"
#include "record/storage.hpp"
#include "osd/telemetry/vt_telemetry_frame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace vrx::record {
namespace {

constexpr uint8_t kPipeBlackbox = 102;
constexpr uint8_t kFlagStart    = 0x01;
constexpr uint8_t kFlagEnd      = 0x02;

// Заголовок payload: лічильник кадрів і прапори.
constexpr size_t kHeadSize = 3;

int64_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

} // namespace

struct Blackbox::Impl {
    Config cfg;
    Storage& drive;

    int fd = -1;                       // сокет
    std::atomic<bool> running{false};
    std::thread rx, wr;

    // Черга між прийомом і записом.
    struct Chunk {
        uint8_t flags = 0;
        std::vector<uint8_t> data;
    };
    mutable std::mutex q_mtx;
    std::condition_variable q_cv;
    std::deque<Chunk> queue;
    size_t queue_bytes = 0;

    mutable std::mutex st_mtx;
    Stats st;

    // Стан приймача: лічильник кадрів для пошуку розривів.
    bool have_seq = false;
    uint16_t last_seq = 0;

    Impl(Config c, Storage& d) : cfg(std::move(c)), drive(d) {}

    // ── прийом ───────────────────────────────────────────────────────────

    bool open_socket() {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return false;

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        // ПРИЙМАЛЬНИЙ БУФЕР ВЕЛИКИЙ, і це не перестраховка: запис на
        // флешку зависає на сотні мілісекунд, і весь цей час кадри мусять
        // десь чекати. Ядро подвоює значення, тож фактично буде вдвічі
        // більше за просимо.
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cfg.rcvbuf_bytes, sizeof(cfg.rcvbuf_bytes));

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)cfg.port);
        if (::bind(fd, (sockaddr*)&a, sizeof(a)) != 0) {
            std::fprintf(stderr, "[скринька] порт %d не зайнявся: %s\n",
                         cfg.port, std::strerror(errno));
            ::close(fd); fd = -1;
            return false;
        }

        int got = 0; socklen_t gl = sizeof(got);
        ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &gl);
        std::fprintf(stderr, "[скринька] слухаю порт %d, буфер сокета %d КБ\n",
                     cfg.port, got / 1024);
        return true;
    }

    void push(uint8_t flags, const uint8_t* data, size_t n) {
        std::lock_guard<std::mutex> lk(q_mtx);

        // Черга переповнилась — ВИКИДАЄМО і рахуємо. Блокувати прийом не
        // можна: тоді переповниться буфер сокета, і втрата буде тихою.
        if (queue_bytes + n > cfg.queue_max_bytes) {
            std::lock_guard<std::mutex> sk(st_mtx);
            st.dropped++;
            return;
        }
        queue.push_back({flags, std::vector<uint8_t>(data, data + n)});
        queue_bytes += n;
        q_cv.notify_one();
    }

    void rx_loop() {
        std::vector<uint8_t> buf(2048);
        while (running.load(std::memory_order_relaxed)) {
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0) continue;

            const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
            if (n <= 0) continue;

            uint8_t pipe = 0;
            const uint8_t* payload = nullptr;
            uint8_t psize = 0;
            if (!vt_telemetry::parse_frame(buf.data(), (uint16_t)n, &pipe, &payload, &psize) ||
                pipe != kPipeBlackbox || psize < kHeadSize) {
                std::lock_guard<std::mutex> sk(st_mtx);
                st.bad++;
                continue;
            }

            const uint16_t seq = (uint16_t)(payload[0] | (payload[1] << 8));
            const uint8_t flags = payload[2];
            const uint8_t* data = payload + kHeadSize;
            const size_t dn = psize - kHeadSize;

            // РОЗРИВ ЛІЧИЛЬНИКА — втрачені кадри. Пишемо далі (Explorer
            // ресинхронізується), але діра має бути видимою, інакше лог
            // виглядає цілим, а він не цілий.
            if (have_seq) {
                const uint16_t expect = (uint16_t)(last_seq + 1);
                if (seq != expect) {
                    const uint16_t miss = (uint16_t)(seq - expect);
                    std::lock_guard<std::mutex> sk(st_mtx);
                    st.gaps++;
                    st.lost += miss;
                    std::fprintf(stderr, "[скринька] РОЗРИВ: чекали %u, прийшов %u"
                                 " — втрачено %u кадрів\n", expect, seq, miss);
                }
            }
            last_seq = seq;
            have_seq = true;

            {
                std::lock_guard<std::mutex> sk(st_mtx);
                st.frames++;
            }
            push(flags, data, dn);
        }
    }

    // ── запис ────────────────────────────────────────────────────────────

    int out = -1;
    std::vector<uint8_t> acc;          // накопичувач блоку
    int64_t last_data_ms = 0;
    uint32_t drive_gen = 0;

    bool open_file() {
        close_file(false);
        const std::string path = drive.make_path("Blackbox", "bbl");
        if (path.empty()) return false;
        out = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (out < 0) {
            std::fprintf(stderr, "[скринька] не відкрився %s: %s\n",
                         path.c_str(), std::strerror(errno));
            return false;
        }
        drive_gen = drive.state().generation;
        std::lock_guard<std::mutex> sk(st_mtx);
        st.files++;
        st.open = true;
        std::fprintf(stderr, "[скринька] пишу %s\n", path.c_str());
        return true;
    }

    void flush_acc() {
        if (out < 0 || acc.empty()) return;
        const ssize_t w = ::write(out, acc.data(), acc.size());
        if (w < 0) {
            std::fprintf(stderr, "[скринька] запис не вдався: %s\n", std::strerror(errno));
            close_file(false);
        } else {
            std::lock_guard<std::mutex> sk(st_mtx);
            st.bytes += (uint64_t)w;
        }
        acc.clear();
    }

    void close_file(bool graceful) {
        if (out < 0) return;
        flush_acc();
        // fsync лише тут: на кожному блоці він з'їв би сенс блокового
        // запису, а тут файл усе одно завершено.
        if (graceful) ::fsync(out);
        ::close(out);
        out = -1;
        std::lock_guard<std::mutex> sk(st_mtx);
        st.open = false;
    }

    void wr_loop() {
        while (running.load(std::memory_order_relaxed) || !queue.empty()) {
            Chunk c;
            bool got = false;
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_cv.wait_for(lk, std::chrono::milliseconds(200),
                              [this] { return !queue.empty() ||
                                              !running.load(std::memory_order_relaxed); });
                if (!queue.empty()) {
                    c = std::move(queue.front());
                    queue.pop_front();
                    queue_bytes -= c.data.size();
                    got = true;
                }
            }

            if (!got) {
                // ТИША ПІСЛЯ ДАНИХ — закриваємо за таймаутом. Лінк міг
                // зникнути, живлення зняти; обрізаний .bbl валідний.
                if (out >= 0 && last_data_ms > 0 &&
                    now_ms() - last_data_ms > cfg.idle_close_ms) {
                    std::fprintf(stderr, "[скринька] тиша %d мс — закриваю файл\n",
                                 cfg.idle_close_ms);
                    close_file(true);
                }
                continue;
            }

            // Носій зник або його підмінили — файл більше не наш.
            const DriveState ds = drive.state();
            if (out >= 0 && (!ds.usable() || ds.generation != drive_gen)) close_file(false);

            if (c.flags & kFlagStart) open_file();

            // КАДР БЕЗ ВІДКРИТОГО ФАЙЛУ — станція піднялась посеред логу.
            // Початок утрачено, але решта читається, тож відкриваємо й
            // пишемо, а не викидаємо.
            if (out < 0 && !c.data.empty() && ds.usable()) open_file();

            if (out >= 0 && !c.data.empty()) {
                acc.insert(acc.end(), c.data.begin(), c.data.end());
                last_data_ms = now_ms();
                if (acc.size() >= cfg.block_bytes) flush_acc();
            }

            if (c.flags & kFlagEnd) {
                std::fprintf(stderr, "[скринька] кінець логу\n");
                close_file(true);
            }
        }
        close_file(true);
    }
};

Blackbox::Blackbox(Config cfg, Storage& storage)
    : impl_(std::make_unique<Impl>(std::move(cfg), storage)) {}
Blackbox::~Blackbox() { stop(); }

bool Blackbox::start() {
    if (impl_->running.load()) return true;
    if (!impl_->open_socket()) return false;
    impl_->running.store(true);
    impl_->rx = std::thread([this] { impl_->rx_loop(); });
    impl_->wr = std::thread([this] { impl_->wr_loop(); });
    return true;
}

void Blackbox::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->q_cv.notify_all();
    if (impl_->rx.joinable()) impl_->rx.join();
    if (impl_->wr.joinable()) impl_->wr.join();
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
}

Blackbox::Stats Blackbox::stats() const {
    std::lock_guard<std::mutex> lk(impl_->st_mtx);
    return impl_->st;
}

} // namespace vrx::record
