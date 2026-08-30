#include "source/mkv_feeder.hpp"
#include "record/mkv_seek.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vrx::source {

namespace {
// З запасом на заголовок: заміряно 391 байт, але від налаштувань муксера
// він може підрости. Шістнадцять кілобайтів покривають будь-який реальний
// випадок і коштують нічого.
constexpr size_t kHeaderProbe = 16u * 1024;
}

MkvFeeder::~MkvFeeder() { close(); }

bool MkvFeeder::open(const std::string& path) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;

    struct stat st {};
    if (::fstat(fd_, &st) == 0) size_ = st.st_size;

    std::vector<uint8_t> probe(kHeaderProbe);
    const ssize_t got = ::pread(fd_, probe.data(), probe.size(), 0);
    if (got <= 0) { close(); return false; }

    const size_t hs = record::mkv_header_size(probe.data(), (size_t)got);
    if (hs == 0) { close(); return false; }      // кластера не видно — не наш файл

    header_.assign(probe.begin(), probe.begin() + hs);
    header_sent_ = 0;
    pos_ = (int64_t)hs;
    return true;
}

void MkvFeeder::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    header_.clear();
    header_sent_ = 0;
    pos_ = size_ = 0;
    limit_ = -1;
}

void MkvFeeder::set_limit(int64_t bytes) { limit_ = bytes; }

bool MkvFeeder::seek(int64_t byte_hint) {
    if (fd_ < 0) return false;
    const int64_t end = limit_ >= 0 ? std::min(limit_, size_) : size_;
    const int64_t at = record::mkv_find_cluster(fd_, byte_hint, end);
    if (at < 0) return false;
    pos_ = at;
    header_sent_ = 0;        // після стрибка демультиплексор починає з нуля
    return true;
}

size_t MkvFeeder::read(uint8_t* out, size_t max) {
    if (fd_ < 0 || max == 0) return 0;

    // Заголовок іде першим і цілком: демультиплексор має побачити опис
    // доріжок раніше за будь-який кластер.
    if (header_sent_ < header_.size()) {
        const size_t n = std::min(max, header_.size() - header_sent_);
        std::memcpy(out, header_.data() + header_sent_, n);
        header_sent_ += n;
        return n;
    }

    const int64_t end = limit_ >= 0 ? std::min(limit_, size_) : size_;
    if (pos_ >= end) return 0;

    const size_t want = (size_t)std::min<int64_t>((int64_t)max, end - pos_);
    const ssize_t got = ::pread(fd_, out, want, pos_);
    if (got <= 0) return 0;
    pos_ += got;
    return (size_t)got;
}

} // namespace vrx::source
