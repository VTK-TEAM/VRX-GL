#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <exception>

// Загальний незалежний UDP-слухач — жодного знання про формат payload
// (JSON, бінарний кадр з CRC8 тощо): просто приймає датаграму і віддає її
// викликачу як є. std::string тут лише контейнер байтів (конструюється з
// (ptr, len), вбудовані '\0' не обрізають дані) — підходить і для
// текстових, і для бінарних протоколів.
//
// Один екземпляр = один порт = один робочий потік. Використовується скрізь
// у проєкті, де потрібен незалежний прийом UDP: JSON-протокол лейауту
// (main.cpp, порт 6000), легасі pip/split-керування (MonitorControlService,
// порт 9000), бінарна телеметрія нового протоколу (VtTelemetryListener,
// порт 50122). Валідація формату — відповідальність викликача через
// set_callback(), не цього класу.
using udp_callback_t = std::function<void(const std::string&)>;

class udp_listener {
public:
    explicit udp_listener(int port) : port_(port) {}

    ~udp_listener() {
        stop();
    }

    udp_listener(const udp_listener&) = delete;
    udp_listener& operator=(const udp_listener&) = delete;

    bool start() {
        if (socket_fd_ >= 0) return true;

        socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (socket_fd_ < 0) {
            std::perror("udp_listener: socket failed");
            return false;
        }

        int optval = 1;
        setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::perror("udp_listener: bind failed");
            close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        stop_requested_.store(false, std::memory_order_relaxed);
        worker_thread_ = std::thread(&udp_listener::worker_loop, this);

        std::fprintf(stderr, "udp_listener: слухаю порт %d\n", port_);
        return true;
    }

    void stop() {
        stop_requested_.store(true, std::memory_order_relaxed);
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (socket_fd_ >= 0) {
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    void set_callback(udp_callback_t cb) {
        callback_ = std::move(cb);
    }

private:
    void worker_loop() {
        char buffer[MAX_UDP_PACKET_SIZE];

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            struct pollfd pfd{};
            pfd.fd = socket_fd_;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 100);

            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            if (ret > 0 && (pfd.revents & POLLIN)) {
                ssize_t bytes_read = recv(socket_fd_, buffer, sizeof(buffer), 0);
                if (bytes_read > 0 && callback_) {
                    // Колбек може парсити НЕДОВІРЕНИЙ мережевий ввід (напр.
                    // JSON layout). Битий/чужий пакет із полем неправильного
                    // типу інакше кинув би nlohmann::type_error аж із цього
                    // потоку -> std::terminate -> смерть усього застосунку
                    // (і відео) в польоті. Ловимо ЦЕНТРАЛЬНО тут: один
                    // поганий пакет не має валити систему, просто відкидаємо.
                    try {
                        callback_(std::string(buffer, static_cast<size_t>(bytes_read)));
                    } catch (const std::exception& e) {
                        std::fprintf(stderr,
                            "udp_listener[порт %d]: виняток у колбеку, пакет відкинуто: %s\n",
                            port_, e.what());
                    } catch (...) {
                        std::fprintf(stderr,
                            "udp_listener[порт %d]: невідомий виняток у колбеку, пакет відкинуто\n",
                            port_);
                    }
                }
            }
        }
    }

    int port_;
    int socket_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
    std::thread worker_thread_;
    udp_callback_t callback_;

    static constexpr size_t MAX_UDP_PACKET_SIZE = 4096;
};
