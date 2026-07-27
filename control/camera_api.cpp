#include "camera_api.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace vrx::control {
namespace {

// Неблокуюче з'єднання з таймаутом. Звичайний connect() на мертвому
// лінку висить десятками секунд — тут це неприйнятно.
int connect_timeout(const char* host, int port, int timeout_ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Запит крихітний; вимикаємо Нейгла, щоб він пішов негайно.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    int r = ::connect(fd, (sockaddr*)&addr, sizeof(addr));
    if (r == 0) return fd;
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    pollfd pfd{fd, POLLOUT, 0};
    if (::poll(&pfd, 1, timeout_ms) <= 0) {
        ::close(fd);
        return -1;
    }

    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

CameraApi::CameraApi(Config cfg) : cfg_(std::move(cfg)) {}

bool CameraApi::set(const std::string& key, int value) {
    requests_++;

    const int fd = connect_timeout(cfg_.host.c_str(), cfg_.port, cfg_.connect_timeout_ms);
    if (fd < 0) {
        failures_++;
        std::fprintf(stderr, "[cam] %s=%d: не з'єдналося з %s:%d\n",
                     key.c_str(), value, cfg_.host.c_str(), cfg_.port);
        return false;
    }

    char req[512];
    const int n = std::snprintf(req, sizeof(req),
        "GET /api/v1/set?%s=%d HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        key.c_str(), value, cfg_.host.c_str());

    bool ok = false;
    if (::send(fd, req, (size_t)n, MSG_NOSIGNAL) == n) {
        // Відповідь читаємо не заради вмісту, а щоб побачити код: камера
        // валідує діапазони й відхиляє значення поза межами.
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, cfg_.io_timeout_ms) > 0) {
            char buf[256] = {};
            const ssize_t got = ::recv(fd, buf, sizeof(buf) - 1, 0);
            if (got > 0) {
                ok = (std::strstr(buf, " 200 ") != nullptr);
                if (!ok) {
                    // Обрізаємо до першого рядка — там і код, і причина.
                    char* nl = std::strpbrk(buf, "\r\n");
                    if (nl) *nl = '\0';
                    std::fprintf(stderr, "[cam] %s=%d відхилено: %s\n",
                                 key.c_str(), value, buf);
                }
            }
        }
    }

    ::close(fd);
    if (!ok) failures_++;
    return ok;
}

} // namespace vrx::control
