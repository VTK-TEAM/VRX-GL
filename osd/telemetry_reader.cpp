#include "osd/telemetry_reader.hpp"
#include "osd/telemetry/vt_telemetry_storage.h"

#include <cmath>
#include <ctime>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vrx::osd {
namespace {

uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int64_t get_i64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return (int64_t)v;
}
float get_f32(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

} // namespace

struct TelemetryReader::Impl {
    int fd = -1;
    int channels = 0;
    int frame_ms = 0;
    int frame_len = 0;
    int64_t data_off = 0;
    int64_t epoch_us = 0;
    int64_t last_index = -1;
    int64_t last_fill_ns = 0;
    std::vector<uint8_t> buf;

    ~Impl() { if (fd >= 0) ::close(fd); }
};

TelemetryReader::TelemetryReader() : impl_(std::make_unique<Impl>()) {}
TelemetryReader::~TelemetryReader() = default;

void TelemetryReader::close() {
    if (impl_->fd >= 0) { ::close(impl_->fd); impl_->fd = -1; }
    impl_->frame_len = 0;
    impl_->last_index = -1;
}

bool TelemetryReader::valid() const { return impl_->fd >= 0 && impl_->frame_len > 0; }
int64_t TelemetryReader::epoch_us() const { return impl_->epoch_us; }

bool TelemetryReader::open(const std::string& path) {
    close();
    Impl& d = *impl_;
    d.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (d.fd < 0) return false;

    uint8_t hdr[32];
    if (::pread(d.fd, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr) ||
        std::memcmp(hdr, "VRXT", 4) != 0) {
        close();
        return false;
    }
    d.channels  = get_u16(hdr + 6);
    d.epoch_us  = get_i64(hdr + 8);
    d.frame_ms  = get_u16(hdr + 16);
    d.frame_len = get_u16(hdr + 18);
    d.data_off  = 32 + (int64_t)get_u32(hdr + 20);

    // Довжину кадру беремо З ФАЙЛУ, а не рахуємо з кількості каналів.
    // Колись пара може стати ширшою, і запис, зроблений до того, мусить
    // читатись тим самим кодом.
    if (d.frame_len <= 4 || d.frame_ms <= 0) { close(); return false; }
    d.buf.assign(d.frame_len, 0);
    return true;
}

bool TelemetryReader::fill(int64_t wall_us, VtTelemetryStorage& out) {
    Impl& d = *impl_;
    if (!valid()) return false;

    const int64_t rel_ms = (wall_us - d.epoch_us) / 1000;
    if (rel_ms < 0) return false;
    const int64_t idx = rel_ms / d.frame_ms;

    // Той самий кадр — зазвичай нічого не робимо: сховище вже ним
    // заповнене, а перекладати двісті п'ятдесят шість значень сто разів на
    // секунду ні до чого.
    //
    // АЛЕ НЕ НА ПАУЗІ. Сховище позначає кожне значення часом запису, і OSD
    // ховає ті, що застаріли, — правильна поведінка для ефіру, де мовчазний
    // канал не має показувати давнє число. У записі ж "застаріти" нема
    // чому: значення на цю мить саме таке. Тому раз на п'яту секунди
    // перекладаємо той самий кадр наново — цього досить, щоб він лишався
    // свіжим, і достатньо рідко, щоб не коштувати нічого.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now_ns = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    if (idx == d.last_index && now_ns - d.last_fill_ns < 200000000LL) return true;
    d.last_fill_ns = now_ns;

    struct stat st {};
    if (::fstat(d.fd, &st) != 0) return false;
    const int64_t frames = (st.st_size - d.data_off) / d.frame_len;
    if (frames <= 0) return false;

    const int64_t use = idx >= frames ? frames - 1 : idx;   // за кінцем — останній
    if (::pread(d.fd, d.buf.data(), d.frame_len,
                d.data_off + use * d.frame_len) != (ssize_t)d.frame_len)
        return false;
    d.last_index = idx;

    const int pairs = (d.frame_len - 4) / 6;
    const uint8_t* p = d.buf.data() + 4;
    for (int i = 0; i < pairs; ++i, p += 6) {
        const float v = get_f32(p + 2);
        // NaN означає "каналу не було взагалі" — його й не кладемо, бо
        // інакше він виглядав би як наявний, але порожній. Це різні речі.
        if (std::isnan(v)) continue;
        out.set_value((uint8_t)get_u16(p), v);
    }
    return true;
}

} // namespace vrx::osd
