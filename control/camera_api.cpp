#include "camera_api.hpp"

#include "../osd/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vrx::control {
namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

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

// ТІЛО ВИРІЗАЄТЬСЯ ВІД ПЕРШОЇ `{` ДО ОСТАННЬОЇ `}`, а не за
// Content-Length чи розбором chunked.
//
// Причина практична. Відповіді контракту — один невеликий об'єкт JSON,
// отже за chunked він поїде одним куском, і між фігурними дужками
// службових байтів не буде. Повний розбір chunked тут додав би код, який
// ніде більше не використовується, і власні помилки в ньому — заради
// випадку, якого ця прошивка не створює. Якщо колись створить — падіння
// буде гучним (JSON не розбереться), а не тихим.
std::string carve_json(const std::string& raw) {
    const size_t b = raw.find('{');
    const size_t e = raw.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e < b) return {};
    return raw.substr(b, e - b + 1);
}

int status_code(const std::string& raw) {
    // "HTTP/1.1 409 Conflict"
    const size_t sp = raw.find(' ');
    if (sp == std::string::npos) return 0;
    return std::atoi(raw.c_str() + sp + 1);
}

void parse_phase(const nlohmann::json& o, CameraApi::Phase* p, bool* has) {
    if (!o.is_object()) return;
    if (!o.contains("trimMilliHz")) return;
    p->trim_mhz = o.value("trimMilliHz", 0);
    // Межі приходять разом зі станом. Якщо їх немає — смуга лишається
    // невідомою, і викликач має право не довіряти нулям.
    if (o.contains("rangeLo") && o.contains("rangeHi")) {
        p->band.lo_mhz = o.value("rangeLo", 0);
        p->band.hi_mhz = o.value("rangeHi", 0);
        p->band.known = true;
    }
    *has = true;
}

} // namespace

CameraApi::CameraApi(Config cfg) : cfg_(std::move(cfg)) {}

int CameraApi::http_get(const std::string& path, std::string* out_raw) {
    requests_++;

    const int fd = connect_timeout(cfg_.host.c_str(), cfg_.port, cfg_.connect_timeout_ms);
    if (fd < 0) return 0;

    char req[512];
    const int n = std::snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path.c_str(), cfg_.host.c_str());

    std::string raw;
    if (n > 0 && ::send(fd, req, (size_t)n, MSG_NOSIGNAL) == n) {
        // Читаємо до закриття (ми просили Connection: close), а не один
        // recv: конверт цілком нам потрібен, а він не гарантовано
        // приходить одним пакетом.
        const int64_t deadline = now_ms() + cfg_.io_timeout_ms;
        for (;;) {
            const int left = (int)(deadline - now_ms());
            if (left <= 0) break;
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, left) <= 0) break;
            char buf[1024];
            const ssize_t got = ::recv(fd, buf, sizeof(buf), 0);
            if (got <= 0) break;            // 0 = камера закрила, все прийшло
            raw.append(buf, (size_t)got);
            if (raw.size() > 64u * 1024u) break;   // стеля на випадок дурні
        }
    }
    ::close(fd);

    if (out_raw) *out_raw = raw;
    return status_code(raw);
}

void CameraApi::note_legacy() {
    if (dialect_ != Dialect::kLegacy) {
        dialect_ = Dialect::kLegacy;
        std::fprintf(stderr, "[cam] стара камера без JSON-контракту — "
                             "керую через /api/v1/set?video0.fpsTrimMilliHz\n");
    }
}

void CameraApi::lock_new() {
    if (dialect_ == Dialect::kAuto) {
        dialect_ = Dialect::kNew;
        std::fprintf(stderr, "[cam] камера з JSON-контрактом /api/v1/phase\n");
    }
}

CameraApi::Reply CameraApi::call(const std::string& path, std::string* out_body) {
    Reply rep;

    std::string raw;
    rep.http = http_get(path, &raw);

    const std::string body = carve_json(raw);
    if (out_body) *out_body = body;

    if (rep.http == 0) {
        failures_++;
        rep.error = "no_response";
        std::fprintf(stderr, "[cam] %s: нема відповіді від %s:%d\n",
                     path.c_str(), cfg_.host.c_str(), cfg_.port);
        return rep;
    }

    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        // Код є, конверта немає: це не наш контракт (стара прошивка, чужий
        // httpd, проксі). Не вигадуємо успіх з коду 200 — але викликач
        // вищого рівня з цього розпізнає стару камеру й перемкнеться.
        failures_++;
        rep.error = "bad_envelope";
        std::fprintf(stderr, "[cam] %s: HTTP %d без конверта JSON\n",
                     path.c_str(), rep.http);
        return rep;
    }

    rep.ok = j.value("ok", false);
    rep.error = j.value("error", std::string());
    rep.detail = j.value("detail", std::string());
    parse_phase(j, &rep.phase, &rep.has_phase);

    if (!rep.ok) failures_++;
    return rep;
}

// СТАРИЙ КОНТРАКТ. Один GET, успіх = рівно HTTP 200 (тіла немає). Це те,
// що робила колишня set("video0.fpsTrimMilliHz", v): абсолютне значення,
// без читання й без меж. Багато камер у полі знають лише його.
CameraApi::Reply CameraApi::set_legacy(int millihz) {
    char path[80];
    std::snprintf(path, sizeof(path),
                  "/api/v1/set?video0.fpsTrimMilliHz=%d", millihz);

    Reply rep;
    rep.http = http_get(path, nullptr);
    rep.ok = (rep.http == 200);   // 200 = застосовано; інше = відхилено
    rep.has_phase = false;        // стану стара камера не повертає
    if (!rep.ok) {
        failures_++;
        rep.error = (rep.http == 0) ? "unreachable" : "legacy_http";
        std::fprintf(stderr, "[cam] (legacy) fpsTrimMilliHz=%d: HTTP %d\n",
                     millihz, rep.http);
    }
    return rep;
}

CameraApi::Reply CameraApi::read_phase() {
    if (dialect_ == Dialect::kLegacy) {
        // Стара камера читати фазу не вміє — петля працює без читання
        // (пише абсолют). Не стукаємо даремно й не псуємо лічильник відмов.
        Reply rep; rep.error = "legacy_no_read"; return rep;
    }
    Reply rep = call("/api/v1/phase", nullptr);
    if (rep.error == "bad_envelope") {
        note_legacy();
        Reply r; r.error = "legacy_no_read"; return r;
    }
    if (rep.http != 0) lock_new();
    return rep;
}

CameraApi::Reply CameraApi::set_phase(int millihz) {
    if (dialect_ == Dialect::kLegacy) return set_legacy(millihz);

    char path[64];
    std::snprintf(path, sizeof(path), "/api/v1/phase?millihz=%d", millihz);
    Reply rep = call(path, nullptr);

    // HTTP є, конверта нема — це стара камера. Перемикаємось назавжди й
    // одразу повторюємо запис старим контрактом, щоб команда не пропала.
    if (rep.error == "bad_envelope") {
        note_legacy();
        return set_legacy(millihz);
    }
    if (rep.http != 0) lock_new();

    if (!rep.ok) {
        std::fprintf(stderr, "[cam] phase=%d відхилено: HTTP %d, %s%s%s\n",
                     millihz, rep.http,
                     rep.error.empty() ? "?" : rep.error.c_str(),
                     rep.detail.empty() ? "" : " — ",
                     rep.detail.c_str());
    }
    return rep;
}

CameraApi::Reply CameraApi::force_idr() {
    // Стара камера примусового IDR цим контрактом не має — тихо пропускаємо,
    // щоб не сипати «без конверта» щоразу.
    if (dialect_ == Dialect::kLegacy) { Reply r; r.error = "legacy_no_idr"; return r; }
    Reply rep = call("/api/v1/idr", nullptr);
    if (rep.error == "bad_envelope") { note_legacy(); Reply r; r.error = "legacy_no_idr"; return r; }
    if (rep.http != 0) lock_new();
    return rep;
}

CameraApi::Status CameraApi::read_status() {
    Status st;
    if (dialect_ == Dialect::kLegacy) return st;  // стара камера JSON-статусу не дає
    std::string body;
    const Reply rep = call("/api/v1/status", &body);
    if (rep.error == "bad_envelope") { note_legacy(); return st; }
    if (rep.http != 0) lock_new();
    if (!rep.ok) return st;

    const auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return st;

    st.version = j.value("version", std::string());

    if (j.contains("video") && j["video"].is_object()) {
        const auto& v = j["video"];
        st.codec = v.value("codec", std::string());
        st.width = v.value("width", 0);
        st.height = v.value("height", 0);
        st.fps_requested = v.value("fpsRequested", 0);
        st.fps_live = v.value("fpsLive", 0.0);
        st.bitrate_kbit = v.value("bitrate", 0);
        st.gop_s = v.value("gopSeconds", 0.0);
    }
    if (j.contains("outgoing") && j["outgoing"].is_object()) {
        const auto& o = j["outgoing"];
        st.outgoing_enabled = o.value("enabled", false);
        st.outgoing_server = o.value("server", std::string());
    }
    if (j.contains("phase")) {
        bool has = false;
        parse_phase(j["phase"], &st.phase, &has);
    }

    st.valid = true;
    return st;
}

} // namespace vrx::control
