#include "storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

namespace vrx::record {
namespace {

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && ::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (i < path.size()) cur += path[i];
    }
    return true;
}

// Чи змонтований блоковий пристрій. Читаємо /proc/mounts, а не питаємо
// систему: це просто текстовий файл у пам'яті ядра, і він не може
// зависнути на мертвому носії.
bool device_mounted(const std::string& dev_path) {
    std::ifstream f("/proc/mounts");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, dev_path.size(), dev_path) == 0 &&
            line.size() > dev_path.size() && line[dev_path.size()] == ' ') {
            return true;
        }
    }
    return false;
}

} // namespace

Storage::Storage(Config cfg) : cfg_(std::move(cfg)) {
    struct stat st;
    if (::stat("/", &st) == 0) root_dev_ = (uint64_t)st.st_dev;
}

Storage::~Storage() { stop(); }

bool Storage::start() {
    if (running_.exchange(true)) return true;
    probe_thread_ = std::thread([this] { probe_loop(); });
    sync_thread_ = std::thread([this] { sync_loop(); });
    std::fprintf(stderr, "[носій] стежу за флешкою\n");
    return true;
}

void Storage::stop() {
    if (!running_.exchange(false)) return;
    sync_cv_.notify_all();
    // probe_thread_ може висіти в D-стані на мертвому носії. join()
    // тоді не повернеться ніколи, тож потік відпускаємо: процес однаково
    // завершується, а ядро добиває його самé.
    if (sync_thread_.joinable()) sync_thread_.join();
    if (probe_thread_.joinable()) probe_thread_.detach();
}

DriveState Storage::state() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    DriveState s = state_;
    if (last_probe_ok_ns_ > 0) s.age_ms = (now_ns() - last_probe_ok_ns_) / 1000000;
    return s;
}

bool Storage::is_external(const std::string& path) const {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return (uint64_t)st.st_dev != root_dev_;
}

void Storage::mount_removable() {
    DIR* blk = ::opendir("/sys/block");
    if (!blk) return;

    while (dirent* e = ::readdir(blk)) {
        const std::string dev = e->d_name;
        if (dev.compare(0, 2, "sd") != 0) continue;

        // Розділи лежать піддиректоріями з тим самим префіксом: sda -> sda1.
        std::vector<std::string> parts;
        if (DIR* d = ::opendir(("/sys/block/" + dev).c_str())) {
            while (dirent* s = ::readdir(d)) {
                const std::string n = s->d_name;
                if (n.size() > dev.size() && n.compare(0, dev.size(), dev) == 0) {
                    parts.push_back(n);
                }
            }
            ::closedir(d);
        }
        if (parts.empty()) parts.push_back(dev);   // без таблиці розділів

        for (const std::string& p : parts) {
            const std::string node = "/dev/" + p;
            if (device_mounted(node)) continue;    // мовчки: це сталий стан

            std::fprintf(stderr, "[носій] монтую %s\n", node.c_str());
            const std::string cmd = "udisksctl mount -b " + node + " 2>&1";
            if (FILE* pipe = ::popen(cmd.c_str(), "r")) {
                char line[256];
                while (::fgets(line, sizeof(line), pipe)) {
                    std::fprintf(stderr, "[носій]   %s", line);
                }
                ::pclose(pipe);
            }
        }
    }
    ::closedir(blk);
}

void Storage::probe_once() {
    if (cfg_.auto_mount) mount_removable();

    std::string found;
    uint64_t free_b = 0, total_b = 0;

    for (const std::string& root : cfg_.mount_roots) {
        DIR* d = ::opendir(root.c_str());
        if (!d) continue;

        while (dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;

            const std::string path = root + "/" + name;

            // Точка монтування користувача: /media/<user>/<том>. Якщо
            // сама вона не на іншій ФС — спускаємось на рівень нижче.
            std::vector<std::string> candidates{path};
            if (!is_external(path)) {
                if (DIR* sub = ::opendir(path.c_str())) {
                    while (dirent* s = ::readdir(sub)) {
                        const std::string sn = s->d_name;
                        if (sn == "." || sn == "..") continue;
                        candidates.push_back(path + "/" + sn);
                    }
                    ::closedir(sub);
                }
            }

            for (const std::string& c : candidates) {
                if (!is_external(c)) continue;
                if (::access(c.c_str(), W_OK) != 0) continue;

                struct statvfs vfs;
                if (::statvfs(c.c_str(), &vfs) != 0) continue;

                const uint64_t fb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
                if (fb <= cfg_.reserve_bytes) continue;

                found = c;
                free_b = fb;
                total_b = (uint64_t)vfs.f_blocks * vfs.f_frsize;
                break;
            }
            if (!found.empty()) break;
        }
        ::closedir(d);
        if (!found.empty()) break;
    }

    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        // Номер росте лише на РЕАЛЬНІЙ зміні носія. Той самий шлях із
        // тим самим станом — та сама флешка, номер не чіпаємо.
        const bool changed = (found.empty() != state_.root.empty()) || (found != state_.root);
        if (changed) state_.generation++;

        state_.present = !found.empty();
        state_.root = found;
        state_.free_bytes = free_b;
        state_.total_bytes = total_b;
        last_probe_ok_ns_ = now_ns();
    }

    char buf[256];
    if (found.empty()) {
        std::snprintf(buf, sizeof(buf), "немає");
    } else {
        std::snprintf(buf, sizeof(buf), "%s, вільно %.1f ГБ з %.1f",
                      found.c_str(), free_b / 1e9, total_b / 1e9);
    }
    if (last_log_ != buf) {
        last_log_ = buf;
        std::fprintf(stderr, "[носій] %s\n", buf);
    }
}

void Storage::probe_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        probe_once();
        for (int slept = 0; slept < cfg_.poll_ms && running_.load(); slept += 100) {
            struct timespec ts{0, 100 * 1000000L};
            ::nanosleep(&ts, nullptr);
        }
    }
}

void Storage::request_sync() {
    if (sync_busy_.load(std::memory_order_acquire)) return;   // попередній ще йде
    {
        std::lock_guard<std::mutex> lk(sync_mtx_);
        sync_requested_ = true;
    }
    sync_cv_.notify_one();
}

void Storage::sync_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        std::string root;
        {
            std::unique_lock<std::mutex> lk(sync_mtx_);
            sync_cv_.wait(lk, [this] {
                return sync_requested_ || !running_.load(std::memory_order_relaxed);
            });
            if (!running_.load(std::memory_order_relaxed)) return;
            sync_requested_ = false;
        }
        {
            std::lock_guard<std::mutex> lk(state_mtx_);
            root = state_.root;
        }
        if (root.empty()) continue;

        sync_busy_.store(true, std::memory_order_release);
        // syncfs, а не sync: скидаємо ЛИШЕ файлову систему носія.
        // Загальний sync() зачепив би й системний диск, а це на
        // навантаженій платі десятки мілісекунд у найкращому разі.
        const int fd = ::open(root.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::syncfs(fd);
            ::close(fd);
        }
        sync_busy_.store(false, std::memory_order_release);
    }
}

std::string Storage::make_path(const std::string& stream_name,
                               const std::string& ext) const {
    std::string root;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        root = state_.root;
    }
    if (root.empty()) return {};

    std::time_t t = std::time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    char day[32], stamp[32];
    std::strftime(day, sizeof(day), "%Y-%m-%d", &tm_buf);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm_buf);

    const std::string dir = root + "/" + day;
    if (!mkdir_p(dir)) {
        std::fprintf(stderr, "[носій] не створилася директорія %s: %s\n",
                     dir.c_str(), std::strerror(errno));
        return {};
    }
    return dir + "/" + stamp + "_" + stream_name + "." + ext;
}

} // namespace vrx::record
