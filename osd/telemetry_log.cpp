#include "osd/telemetry_log.hpp"
#include "osd/telemetry/vt_telemetry_storage.h"
#include "record/storage.hpp"
#include "version.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vrx::osd {
namespace {

int64_t now_wall_us() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

void put_u16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}
void put_i64(uint8_t* p, int64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((uint64_t)v >> (i * 8));
}
void put_f32(uint8_t* p, float v) { std::memcpy(p, &v, 4); }

} // namespace

struct TelemetryLog::Impl {
    Config cfg;
    VtTelemetryStorage& tlm;
    record::Storage& drive;

    std::atomic<bool> running{false};
    std::thread th;
    int fd = -1;
    uint32_t gen = 0;              // покоління носія, з яким відкрито файл
    int64_t epoch_us = 0;
    std::vector<uint8_t> frame;

    Impl(Config c, VtTelemetryStorage& t, record::Storage& d)
        : cfg(std::move(c)), tlm(t), drive(d) {}

    std::string path_for(const std::string& root) const {
        const time_t sec = time(nullptr);
        struct tm tm {};
        localtime_r(&sec, &tm);
        char day[32];
        std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);
        return root + "/" + day + "/session_" + cfg.session + "_tlm.bin";
    }

    bool open_file(const record::DriveState& st) {
        const std::string p = path_for(st.root);

        // Теку дня зазвичай уже зробив рекордер; якщо телеметрія
        // випередила його — робимо самі, це дешево.
        const size_t slash = p.rfind('/');
        if (slash != std::string::npos) ::mkdir(p.substr(0, slash).c_str(), 0755);

        fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) return false;

        epoch_us = now_wall_us();
        const uint16_t n = (uint16_t)cfg.channels;
        const uint16_t flen = (uint16_t)(4 + n * 6);
        frame.assign(flen, 0);

        // Опис короткий і навмисно. Таблиці імен тут немає: набір id
        // сталий, і читач їх знає — що змінюється з часом, так це їхня
        // КІЛЬКІСТЬ. Саме її, разом із кроком і довжиною кадру, файл і несе
        // в собі, щоб лишатись читабельним, коли каналів побільшає.
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "format=VRXT\nversion=1\nsession=%s\nbuild=%s\n"
                      "channels=%u\nframe_ms=%d\nframe_len=%u\nepoch_us=%lld\n",
                      cfg.session.c_str(), VRX_VERSION, n, cfg.frame_ms,
                      flen, (long long)epoch_us);
        const std::string meta = buf;

        uint8_t hdr[32] = {};
        std::memcpy(hdr, "VRXT", 4);
        put_u16(hdr + 4, 1);
        put_u16(hdr + 6, n);
        put_i64(hdr + 8, epoch_us);
        put_u16(hdr + 16, (uint16_t)cfg.frame_ms);
        put_u16(hdr + 18, flen);
        put_u32(hdr + 20, (uint32_t)meta.size());
        if (::write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
            ::write(fd, meta.data(), meta.size()) != (ssize_t)meta.size()) {
            ::close(fd); fd = -1; return false;
        }
        gen = st.generation;
        std::fprintf(stderr, "[телеметрія] лог %s: %u каналів, кадр %u Б кожні %d мс\n",
                     p.c_str(), n, flen, cfg.frame_ms);
        return true;
    }

    void close_file() {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }

    void write_frame() {
        if (fd < 0) return;
        put_u32(frame.data(), (uint32_t)((now_wall_us() - epoch_us) / 1000));

        uint8_t* p = frame.data() + 4;
        for (int id = 0; id < cfg.channels; ++id, p += 6) {
            put_u16(p, (uint16_t)id);
            float v = std::nanf("");          // каналу не було — так і скажемо
            (void)tlm.get_value((uint8_t)id, &v, nullptr);
            put_f32(p + 2, v);
        }
        if (::write(fd, frame.data(), frame.size()) < 0) close_file();
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            const record::DriveState st = drive.state();

            // Носій зник або його підмінили — файл більше не наш.
            if (fd >= 0 && (!st.usable() || st.generation != gen)) close_file();
            if (fd < 0 && st.usable()) open_file(st);
            if (fd >= 0) write_frame();

            for (int i = 0; i < cfg.frame_ms / 20 &&
                            running.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        close_file();
    }
};

TelemetryLog::TelemetryLog(Config cfg, VtTelemetryStorage& storage, record::Storage& drive)
    : impl_(std::make_unique<Impl>(std::move(cfg), storage, drive)) {}
TelemetryLog::~TelemetryLog() { stop(); }

bool TelemetryLog::start() {
    if (impl_->running.exchange(true)) return true;
    impl_->th = std::thread([this] { impl_->loop(); });
    return true;
}

void TelemetryLog::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace vrx::osd
