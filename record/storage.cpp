#include "storage.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>

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

// Розекранування шляхів із /proc: пробіли й табуляції в шляхах ідуть як
// \040 і \011. Без цього точка монтування з пробілом у назві тому — а
// такі трапляються — не збіглася б із реальним шляхом.
std::string unescape_mount(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i+1] >= '0' && s[i+1] <= '7' &&
            s[i+2] >= '0' && s[i+2] <= '7' &&
            s[i+3] >= '0' && s[i+3] <= '7') {
            out += (char)((s[i+1]-'0')*64 + (s[i+2]-'0')*8 + (s[i+3]-'0'));
            i += 3;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Точка монтування справжнього блокового пристрою.
struct MountInfo {
    std::string dev;    // джерело: /dev/sda1
    long id = 0;        // номер монтування — НОВИЙ на кожне монтування
};

// Читаємо /proc/self/mountinfo, а не /proc/mounts, заради першого поля —
// НОМЕРА МОНТУВАННЯ.
//
// Навіщо він. Носій розпізнавався шляхом, і поки шлях той самий,
// вважалося, що флешка та сама. Але точка монтування — це ім'я тому, і
// після відвалювання й повернення (поганий контакт, просідання живлення,
// перерахування USB) udisks монтує ту саму флешку тим самим іменем.
// Шлях збігається, а файлова система вже ІНША: відкритий дескриптор
// рекордера показує на попередню, якої вже немає, і запис іде в
// порожнечу за живим fd. Ловилось наживо — станція перемонтувала носій
// сама, і жоден лічильник цього не побачив.
//
// st_dev тут не годиться: для того самого блокового пристрою він
// однаковий до й після перемонтування. Номер монтування — єдиний
// ідентифікатор, який ядро видає саме ЕКЗЕМПЛЯРУ монтування.
//
// Заразом — чому взагалі /proc, а не stat: це текстовий файл у пам'яті
// ядра, і він не може зависнути на мертвому носії. І чому саме блокові
// пристрої: інакше носієм вважалась би будь-яка чужа файлова система,
// зокрема tmpfs. У списку точок пошуку стоїть /run/media, а /run це
// tmpfs — порожня директорія там пройшла б усі перевірки, і statvfs
// віддав би вільну ОПЕРАТИВНУ ПАМ'ЯТЬ як місце на носії.
std::map<std::string, MountInfo> block_mounts() {
    std::map<std::string, MountInfo> out;
    std::ifstream f("/proc/self/mountinfo");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::vector<std::string> tok;
        std::string t;
        while (is >> t) tok.push_back(t);
        if (tok.size() < 7) continue;

        // Між шостим полем і роздільником "-" стоїть змінна кількість
        // необов'язкових полів, тож джерело шукаємо від роздільника.
        size_t sep = 6;
        while (sep < tok.size() && tok[sep] != "-") ++sep;
        if (sep + 2 >= tok.size()) continue;

        const std::string& src = tok[sep + 2];
        if (src.compare(0, 5, "/dev/") != 0) continue;
        out[unescape_mount(tok[4])] = MountInfo{src, std::atol(tok[0].c_str())};
    }
    return out;
}

bool device_mounted(const std::map<std::string, MountInfo>& mounts,
                    const std::string& dev_path) {
    for (const auto& kv : mounts) {
        if (kv.second.dev == dev_path) return true;
    }
    return false;
}

} // namespace

// Увесь стан і вся робота — тут. Storage лишається тонкою оболонкою, яка
// цим станом володіє РАЗОМ із потоками: див. коментар про shared_ptr у
// заголовку.
struct Storage::Impl {
    Config cfg;

    mutable std::mutex state_mtx;
    DriveState state;
    int64_t last_probe_ok_ns = 0;

    // Номер ЕКЗЕМПЛЯРА монтування знайденого носія. Живе поруч зі станом
    // і під тим самим локом: саме його зміна означає, що флешку
    // перемонтували, навіть якщо шлях лишився той самий.
    long mount_id = 0;

    // st_dev кореня. Рахується один раз: усе, що на тому самому
    // пристрої, — не знімний носій, а сама система.
    uint64_t root_dev = 0;

    std::atomic<bool> running{false};
    std::thread probe_thread;

    std::thread sync_thread;
    std::mutex sync_mtx;
    std::condition_variable sync_cv;
    bool sync_requested = false;
    std::atomic<bool> sync_busy{false};

    // Придушення спаму: попередній надрукований стан.
    std::string last_log;

    // Розділи, які не змонтувались, і коли пробувати знову. Живе лише в
    // потоці опитування, тож без локу.
    struct MountFail {
        int streak = 0;
        int64_t next_try_ns = 0;
    };
    std::map<std::string, MountFail> mount_fails;

    explicit Impl(Config c) : cfg(std::move(c)) {
        struct stat st;
        if (::stat("/", &st) == 0) root_dev = (uint64_t)st.st_dev;
    }

    void probe_loop();
    void sync_loop();
    void probe_once();
    void mount_removable();
    bool is_external(const std::string& path) const;
};

Storage::Storage(Config cfg) : impl_(std::make_shared<Impl>(std::move(cfg))) {}

Storage::~Storage() { stop(); }

bool Storage::start() {
    if (impl_->running.exchange(true)) return true;
    // Потоки тримають стан ЗА shared_ptr: відв'язаний потік опитування
    // інакше пережив би об'єкт і прокинувся в звільненій пам'яті.
    auto g = impl_;
    impl_->probe_thread = std::thread([g] { g->probe_loop(); });
    impl_->sync_thread = std::thread([g] { g->sync_loop(); });
    std::fprintf(stderr, "[носій] стежу за флешкою\n");
    return true;
}

void Storage::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->sync_cv.notify_all();
    // Потік опитування може висіти в D-стані на мертвому носії. join()
    // тоді не повернеться ніколи, тож потік відпускаємо — а стан лишиться
    // живим доти, доки живий він сам.
    if (impl_->sync_thread.joinable()) impl_->sync_thread.join();
    if (impl_->probe_thread.joinable()) impl_->probe_thread.detach();
}

bool Storage::sync_in_progress() const {
    return impl_->sync_busy.load(std::memory_order_acquire);
}

DriveState Storage::state() const {
    std::lock_guard<std::mutex> lk(impl_->state_mtx);
    DriveState s = impl_->state;
    if (impl_->last_probe_ok_ns > 0) {
        s.age_ms = (now_ns() - impl_->last_probe_ok_ns) / 1000000;
    }
    return s;
}

bool Storage::Impl::is_external(const std::string& path) const {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return (uint64_t)st.st_dev != root_dev;
}

void Storage::Impl::mount_removable() {
    const auto mounts = block_mounts();

    DIR* blk = ::opendir("/sys/block");
    if (!blk) return;

    const int64_t now = now_ns();

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
            if (device_mounted(mounts, node)) {
                mount_fails.erase(node);          // мовчки: це сталий стан
                continue;
            }

            // ПРОГРЕСИВНА ПАУЗА, 1/2/4/8 секунд.
            //
            // Розділ, який udisks не бере — чужа файлова система, немає
            // прав, брудний том, — інакше давав би popen плюс рядок у лог
            // ЩОСЕКУНДИ до кінця польоту. Той самий випадок, що вже
            // лікували паузою в сторожі джерела: стала помилка не
            // виправляється повторенням, вона виправляється рідшим
            // повторенням.
            MountFail& mf = mount_fails[node];
            if (mf.next_try_ns > now) continue;

            std::fprintf(stderr, "[носій] монтую %s%s\n", node.c_str(),
                         mf.streak ? " (повторно)" : "");
            const std::string cmd = "udisksctl mount -b " + node + " 2>&1";
            if (FILE* pipe = ::popen(cmd.c_str(), "r")) {
                char line[256];
                while (::fgets(line, sizeof(line), pipe)) {
                    std::fprintf(stderr, "[носій]   %s", line);
                }
                ::pclose(pipe);
            }

            // Успіх перевіряємо ФАКТОМ появи в /proc/mounts, а не кодом
            // повернення udisksctl: він і сам по собі не завжди чесний, а
            // через popen до нас доїжджає код оболонки.
            if (device_mounted(block_mounts(), node)) {
                mount_fails.erase(node);
            } else {
                if (mf.streak < 4) mf.streak++;
                int64_t back = 1000;
                for (int i = 1; i < mf.streak; ++i) back *= 2;
                mf.next_try_ns = now_ns() + back * 1000000LL;
                std::fprintf(stderr, "[носій] %s не змонтувався (%d-та спроба),"
                             " пауза %lld мс\n", node.c_str(), mf.streak, (long long)back);
            }
        }
    }
    ::closedir(blk);
}

void Storage::Impl::probe_once() {
    if (cfg.auto_mount) mount_removable();

    std::string found;
    long found_id = 0;
    uint64_t free_b = 0, total_b = 0;

    // Список справжніх точок монтування знімається ОДИН раз на прохід:
    // це читання /proc, воно не блокується, але й робити його на кожен
    // кандидат немає сенсу.
    const auto mounts = block_mounts();

    for (const std::string& root : cfg.mount_roots) {
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
                // Головна перевірка — саме ця, і вона йде ПЕРШОЮ, бо не
                // блокується: шлях має бути точкою монтування справжнього
                // блокового пристрою. Без неї порожня директорія на tmpfs
                // проходила б як носій (див. коментар до block_mounts).
                const auto it = mounts.find(c);
                if (it == mounts.end()) continue;
                if (!is_external(c)) continue;
                if (::access(c.c_str(), W_OK) != 0) continue;

                struct statvfs vfs;
                if (::statvfs(c.c_str(), &vfs) != 0) continue;

                const uint64_t fb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
                if (fb <= cfg.reserve_bytes) continue;

                found = c;
                found_id = it->second.id;
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
        std::lock_guard<std::mutex> lk(state_mtx);
        // Номер росте на РЕАЛЬНІЙ зміні носія, і ключем тут стоїть саме
        // номер монтування, а не шлях.
        //
        // Шлях — це ім'я тому: та сама флешка після відвалювання й
        // повернення монтується під тим самим іменем, і порівняння
        // шляхів каже "нічого не змінилось". А файлова система вже інша,
        // і відкритий файл рекордера показує на попередню. Ловилось
        // наживо: станція перемонтувала носій сама, запис тривав у
        // порожнечу за живим дескриптором, лічильники були чисті.
        const bool changed = (found != state.root) || (found_id != mount_id);
        if (changed) state.generation++;
        mount_id = found_id;

        state.present = !found.empty();
        state.root = found;
        state.free_bytes = free_b;
        state.total_bytes = total_b;
        last_probe_ok_ns = now_ns();
    }

    char buf[256];
    if (found.empty()) {
        std::snprintf(buf, sizeof(buf), "немає");
    } else {
        // Номер монтування в рядку не для краси: без нього перемонтування
        // тієї самої флешки дає ТОЙ САМИЙ текст, придушення повторів його
        // ковтає, і в лозі не лишається сліду від події, через яку
        // рекордер щойно почав новий файл.
        std::snprintf(buf, sizeof(buf), "%s, вільно %.1f ГБ з %.1f (монтування #%ld)",
                      found.c_str(), free_b / 1e9, total_b / 1e9, found_id);
    }
    if (last_log != buf) {
        last_log = buf;
        std::fprintf(stderr, "[носій] %s\n", buf);
    }
}

void Storage::Impl::probe_loop() {
    while (running.load(std::memory_order_relaxed)) {
        probe_once();
        for (int slept = 0; slept < cfg.poll_ms && running.load(); slept += 100) {
            struct timespec ts{0, 100 * 1000000L};
            ::nanosleep(&ts, nullptr);
        }
    }
}

void Storage::request_sync() {
    if (impl_->sync_busy.load(std::memory_order_acquire)) return;   // попередній ще йде
    {
        std::lock_guard<std::mutex> lk(impl_->sync_mtx);
        impl_->sync_requested = true;
    }
    impl_->sync_cv.notify_one();
}

void Storage::Impl::sync_loop() {
    while (running.load(std::memory_order_relaxed)) {
        std::string root;
        {
            std::unique_lock<std::mutex> lk(sync_mtx);
            sync_cv.wait(lk, [this] {
                return sync_requested || !running.load(std::memory_order_relaxed);
            });
            if (!running.load(std::memory_order_relaxed)) return;
            sync_requested = false;
        }
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            root = state.root;
        }
        if (root.empty()) continue;

        sync_busy.store(true, std::memory_order_release);
        const int64_t t0 = now_ns();
        // syncfs, а не sync: скидаємо ЛИШЕ файлову систему носія.
        // Загальний sync() зачепив би й системний диск, а це на
        // навантаженій платі десятки мілісекунд у найкращому разі.
        const int fd = ::open(root.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::syncfs(fd);
            ::close(fd);
        }
        const int64_t took = (now_ns() - t0) / 1000000;
        {
            std::lock_guard<std::mutex> lk(state_mtx);
            state.last_sync_ms = took;
            if (took > state.max_sync_ms) state.max_sync_ms = took;
        }
        sync_busy.store(false, std::memory_order_release);
    }
}

std::string Storage::make_path(const std::string& stream_name,
                               const std::string& ext) const {
    std::string root;
    {
        std::lock_guard<std::mutex> lk(impl_->state_mtx);
        root = impl_->state.root;
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
