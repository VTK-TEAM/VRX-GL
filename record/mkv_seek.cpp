#include "record/mkv_seek.hpp"

#include <algorithm>
#include <cstring>
#include <vector>
#include <unistd.h>

namespace vrx::record {
namespace {

constexpr uint8_t kCluster[4]  = {0x1f, 0x43, 0xb6, 0x75};
constexpr uint8_t kTimecodeId  = 0xe7;   // поле часу всередині кластера

// Число змінної довжини EBML. Довжину задають провідні нулі першого байта:
// 1xxxxxxx — один байт, 01xxxxxx — два, і так далі. Для РОЗМІРУ провідну
// мітку знімаємо, бо вона службова й до значення не належить.
//
// Повертає false, якщо байтів не вистачає або перший байт нульовий —
// такого в коректному потоці не буває, і це сам по собі знак, що ми
// дивимось не на структуру, а на дані.
bool read_vint(const uint8_t* p, size_t avail, uint64_t* val, int* len) {
    if (avail == 0 || p[0] == 0) return false;
    int n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && !(p[0] & mask)) { mask >>= 1; ++n; }
    if (n > 8 || (size_t)n > avail) return false;

    uint64_t v = p[0] & (mask - 1);      // знімаємо провідну мітку
    for (int i = 1; i < n; ++i) v = (v << 8) | p[i];
    *val = v;
    *len = n;
    return true;
}

// Чи справді тут починається кластер. За міткою мусить іти розмір, а
// одразу за ним — поле часу. Розмір буває "невідомий" (усі біти в
// одиницях): муксер так пише, коли не повертається назад, тобто саме наш
// випадок, і це нормально.
bool looks_like_cluster(const uint8_t* p, size_t avail) {
    if (avail < 6 || std::memcmp(p, kCluster, 4) != 0) return false;
    uint64_t size = 0;
    int len = 0;
    if (!read_vint(p + 4, avail - 4, &size, &len)) return false;
    if (avail < 4 + (size_t)len + 1) return false;
    return p[4 + len] == kTimecodeId;
}

} // namespace

size_t mkv_header_size(const uint8_t* buf, size_t n) {
    for (size_t i = 0; i + 6 <= n; ++i)
        if (looks_like_cluster(buf + i, n - i)) return i;
    return 0;
}

int64_t mkv_find_cluster(int fd, int64_t from, int64_t file_size) {
    if (from < 0) from = 0;

    // Вікно з нахлестом: мітка може лежати на межі двох читань, і без
    // нахлесту ми б її саме там і губили.
    constexpr size_t kWin = 1u << 20;
    constexpr size_t kOverlap = 16;
    std::vector<uint8_t> buf(kWin);

    for (int64_t pos = from; pos < file_size; pos += kWin - kOverlap) {
        const size_t want = (size_t)std::min<int64_t>(kWin, file_size - pos);
        ssize_t got = ::pread(fd, buf.data(), want, pos);
        if (got <= 0) return -1;

        for (size_t i = 0; i + 6 <= (size_t)got; ++i)
            if (looks_like_cluster(buf.data() + i, (size_t)got - i))
                return pos + (int64_t)i;
    }
    return -1;
}

} // namespace vrx::record
