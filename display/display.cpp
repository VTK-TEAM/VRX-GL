#include "display.hpp"

#include <gbm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <cstdlib>
#include <ctime>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace vrx::display {
namespace {

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

const char* color_format_name(ColorFormat f) {
    switch (f) {
        case ColorFormat::RGB:      return "rgb";
        case ColorFormat::YCbCr444: return "ycbcr444";
        case ColorFormat::YCbCr422: return "ycbcr422";
        case ColorFormat::YCbCr420: return "ycbcr420";
        default:                    return "unknown";
    }
}

// Набір властивостей об'єкта, знайдених за іменем ОДИН РАЗ на відкритті.
// Atomic API оперує числовими id, і шукати їх за іменем 60 разів на
// секунду — марна робота.
struct PropTable {
    std::map<std::string, uint32_t> id;
    std::map<std::string, uint64_t> value;

    bool has(const char* name) const { return id.count(name) != 0; }
    uint32_t operator[](const char* name) const {
        auto it = id.find(name);
        return it == id.end() ? 0 : it->second;
    }
};

PropTable read_props(int fd, uint32_t obj_id, uint32_t obj_type) {
    PropTable t;
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return t;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        t.id[p->name] = p->prop_id;
        t.value[p->name] = props->prop_values[i];
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return t;
}

// Значення enum-властивості за іменем варіанта. Потрібне, бо вендорні
// властивості Rockchip задають кольорові режими саме перелічуваннями
// ("rgb", "ycbcr444"), а не числами з фіксованим значенням.
bool enum_value_by_name(int fd, uint32_t prop_id, const char* want, uint64_t* out) {
    drmModePropertyRes* p = drmModeGetProperty(fd, prop_id);
    if (!p) return false;
    bool found = false;
    if (p->flags & DRM_MODE_PROP_ENUM) {
        for (int i = 0; i < p->count_enums; ++i) {
            if (std::strcmp(p->enums[i].name, want) == 0) {
                *out = p->enums[i].value;
                found = true;
                break;
            }
        }
    }
    drmModeFreeProperty(p);
    return found;
}

// Сокет ядра з подіями гарячого підключення. Беремо саме uevent, а не
// libudev: потрібна рівно одна подія ("щось сталося в підсистемі drm"),
// а тягнути залежність заради неї нема сенсу.
int open_uevent_socket() {
    int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                      NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return -1;

    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;                 // 1 = повідомлення від ядра
    if (::bind(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Вичитує всі накопичені події й каже, чи була серед них drm-подія.
// Читаємо ДО КІНЦЯ навіть після першого збігу: не вичитані повідомлення
// лишили б сокет готовим до читання, і poll крутився б вхолосту.
bool drain_uevents(int fd) {
    bool drm = false;
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = 0;
        // Повідомлення — послідовність рядків, розділених нулями.
        for (ssize_t i = 0; i < n; ) {
            const char* line = buf + i;
            if (std::strstr(line, "drm") || std::strstr(line, "DRM")) drm = true;
            i += (ssize_t)std::strlen(line) + 1;
        }
    }
    return drm;
}

// Примусово скидає кеш EDID у драйвері.
//
// НАВІЩО. Драйвер HDMI цього ядра тримає прочитаний EDID у себе й
// оновлює його лише на СПРАВЖНЬОМУ переході коннектора в disconnected.
// Повторний зонд — хоч drmModeGetConnector, хоч "echo detect" — віддає
// кеш. Отже після заміни монітора, якщо HPD не встиг просісти (заміна
// на ходу, або поки програма не працювала), ми отримаємо список режимів
// ПОПЕРЕДНЬОГО монітора й піднімемо його режим на новому.
//
// Заміряно на залізі: DELL SE2216H (рідні 1920x1080) читався як
// Philips 196VL (1366x768) і працював у 1366x768.
//
// Ліки — прогнати коннектор через примусовий disconnected: запис "off"
// у sysfs-атрибут status, потім "detect" повертає звичайне визначення й
// зонд уже читає EDID із дроту. Атрибут стандартний для DRM, не
// вендорний.
void force_edid_refresh(const std::string& card_path) {
    const std::string card = card_path.substr(card_path.find_last_of('/') + 1);
    const std::string base = "/sys/class/drm/";

    DIR* dir = ::opendir(base.c_str());
    if (!dir) return;

    while (dirent* e = ::readdir(dir)) {
        const std::string name = e->d_name;
        // Цікавлять лише коннектори цієї карти: "card0-HDMI-A-1".
        if (name.rfind(card + "-", 0) != 0) continue;

        const std::string path = base + name + "/status";
        // Чіпаємо лише те, що зараз під'єднане: писати в порожні
        // коннектори немає сенсу, а зайві uevent-и коштують.
        {
            FILE* f = ::fopen(path.c_str(), "r");
            if (!f) continue;
            char buf[32] = {};
            size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
            ::fclose(f);
            if (n == 0 || std::strncmp(buf, "connected", 9) != 0) continue;
        }

        auto write_status = [&path](const char* v) {
            FILE* f = ::fopen(path.c_str(), "w");
            if (!f) return false;
            ::fputs(v, f);
            ::fclose(f);
            return true;
        };

        if (!write_status("off")) continue;      // немає прав — просто живемо з кешем
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        write_status("detect");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    ::closedir(dir);
}

} // namespace

// ---------------------------------------------------------------------

struct Display::Impl {
    explicit Impl(Config c) : cfg(std::move(c)) {}

    Config cfg;
    int fd = -1;
    bool master_taken = false;

    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    drmModeModeInfo mode{};
    uint32_t mode_blob = 0;

    PropTable plane_props, crtc_props, conn_props;

    OutputState state_{};


    // КІЛЬЦЕ ЦІЛЬОВИХ БУФЕРІВ.
    //
    // Володіє ними дисплей, і це не питання зручності: вимоги до
    // сканування (пристрій, формати, вирівнювання) знає він, і він же
    // єдиний знає, який буфер зараз читає сканер. Раніше буфери виділяв
    // рендерер — і для цього відкривав ту саму карту ДРУГИМ дескриптором,
    // робив на ній свій GBM, експортував dmabuf, а дисплей імпортував їх
    // назад. Уся ця подорож існувала лише тому, що володіння було
    // розділене навпіл.
    //
    // П'ять станів, і кожен означає конкретну заборону:
    //   Free      нічий, можна віддати рендереру
    //   Drawing   рендерер малює прямо зараз
    //   Pending   намальований, чекає своєї черги на коміт
    //   InFlight  закомічений, чекає підтвердження розгортки
    //   Current   на екрані, сканер його читає — чіпати не можна
    enum class Slot { Free, Drawing, Pending, InFlight, Current };

    struct RingBuf {
        struct gbm_bo* bo = nullptr;
        int fd = -1;
        uint32_t stride = 0;
        uint32_t fb_id = 0;
        Slot slot = Slot::Free;
    };

    struct gbm_device* gbm = nullptr;
    std::vector<RingBuf> ring;
    int idx_pending = -1, idx_in_flight = -1, idx_current = -1;

    // БУФЕРИ, ЯКІ РОЗІБРАЛИ З-ПІД РЕНДЕРЕРА.
    //
    // Монітор можуть висмикнути в будь-яку мить, зокрема посеред проходу
    // GPU. Знищити буфер у стані Drawing означало б закрити дескриптор і
    // віддати пам'ять, поки в неї пишуть. Сьогодні це не падає лише тому,
    // що імпорт dmabuf у EGL тримає власне посилання — тобто інваріант
    // тримається на поведінці драйвера, а не на нашому коді.
    //
    // Тому такі буфери переїжджають сюди й чекають, поки рендерер поверне
    // їх через present(). Він поверне обов'язково: цикл віддає ціль на
    // кожній ітерації, а розбіжність generation він побачить уже після.
    std::vector<RingBuf> retired;

    // Скільки буферів виділено й ще не звільнено. Рівно глибина кільця в
    // сталому режимі; якщо після замін монітора росте — тече.
    int live_bufs = 0;

    mutable std::mutex mtx;
    PresentStats st{};
    

    // Анкер вікна, на якому міряється точна частота розгортки.
    int64_t hz_anchor_ns = 0;
    uint32_t hz_anchor_seq = 0;
    bool hz_seq_valid = false;

    std::thread event_thread;
    std::atomic<bool> running{false};
    bool modeset_done = false;

    // Гаряча заміна монітора.
    int udev_fd = -1;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> has_output{false};
    std::atomic<bool> refreshing{false};

    FlipCallback on_flip_cb;

    bool configure_output();

    // Знімає поточний вивід, лишаючи карту відкритою. Саме цим
    // відрізняється від close(): дескриптор картки, майстер і потік
    // подій переживають заміну монітора.
    void teardown_output() {
        if (fd >= 0 && plane_id && plane_props.has("FB_ID")) {
            drmModeAtomicReq* req = drmModeAtomicAlloc();
            drmModeAtomicAddProperty(req, plane_id, plane_props["FB_ID"], 0);
            drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_ID"], 0);
            drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            drmModeAtomicFree(req);
        }
        if (fd >= 0) {
            if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
        }
        mode_blob = 0;

        {
            std::lock_guard<std::mutex> lock(mtx);
            // Буфери належать конкретному траєкту виводу: після заміни
            // монітора вони недійсні, навіть якщо роздільність збіглася.
            destroy_ring();
            // Вимір частоти прив'язаний до конкретного тракту розгортки:
            // після заміни монітора продовжувати старе вікно не можна.
            hz_seq_valid = false;
            st.measured_hz = 0;
            st.last_present_ns = 0;
        }

        connector_id = crtc_id = plane_id = 0;
        plane_props = crtc_props = conn_props = PropTable{};
        mode = drmModeModeInfo{};
        modeset_done = false;
        state_ = OutputState{};
        state_.name = "немає виводу";
        has_output.store(false, std::memory_order_release);
    }

    const OutputState& info() const { return state_; }

    // Опис буфера кільця у вигляді звичайного кадру — тим самим типом,
    // яким описуються кадри джерел, щоб рендерер не мав окремої гілки.
    Frame frame_of(int i) const {
        Frame f;
        f.fourcc = state_.fourcc;
        f.modifier = state_.modifier;
        f.width = state_.width;
        f.height = state_.height;
        f.n_planes = 1;
        f.fd[0] = ring[i].fd;
        f.stride[0] = ring[i].stride;
        f.offset[0] = 0;
        return f;
    }

    bool acquire(Target& out) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!has_output.load(std::memory_order_acquire)) return false;

        for (size_t i = 0; i < ring.size(); ++i) {
            if (ring[i].slot != Slot::Free) continue;
            ring[i].slot = Slot::Drawing;
            out.index = (int)i;
            out.generation = state_.generation;
            out.frame = frame_of((int)i);
            return true;
        }
        return false;      // усі зайняті — чекати на розгортку
    }

    // Намальоване стає pending; віддати залізу спробує try_commit().
    bool queue(const Target& t) {
        {
            std::lock_guard<std::mutex> lock(mtx);

            // ЗВІЛЬНЕННЯ ПЕРШЕ, перевірка наявності виводу після. Порядок
            // не косметичний: коли монітор висмикнули, has_output вже
            // false, і рання відмова лишила б буфер із розібраного кільця
            // висіти до самого close(). А розбіжність generation цей
            // випадок ловить сама — teardown обнуляє стан.
            if (t.generation != state_.generation) {
                // Буфер від попередньої конфігурації виводу. Показувати
                // його вже нікуди — але саме зараз він нарешті вільний
                // від GPU, тож це єдиний момент, коли його можна чесно
                // звільнити.
                release_retired(t.frame.fd[0]);
                return false;
            }
            if (!has_output.load(std::memory_order_acquire)) return false;
            if (t.index < 0 || (size_t)t.index >= ring.size()) return false;
            if (ring[t.index].slot != Slot::Drawing) return false;

            // Попередній непоказаний кадр витісняється — рахуємо як дроп,
            // щоб деградацію було видно, а не доводилось здогадуватись.
            if (idx_pending >= 0) {
                ring[idx_pending].slot = Slot::Free;
                st.dropped++;
            }
            ring[t.index].slot = Slot::Pending;
            idx_pending = t.index;
        }
        try_commit();
        return true;
    }

    // Віддати залізу наступний кадр, якщо є що і якщо можна.
    //
    // Кличеться з ДВОХ місць: із queue() і з on_flip(). На один CRTC може
    // бути лише ОДИН flip у польоті — другий коміт до підтвердження
    // першого поверне EBUSY. Тому коли queue() застав зайнято, кадр чекає
    // в pending, і штовхнути його має саме підтвердження попереднього.
    // Без цього кадр лишався б у черзі до наступного виклику ззовні.
    void try_commit() {
        int idx = -1;
        bool need_modeset = false;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (idx_in_flight >= 0 || idx_pending < 0) return;
            idx = idx_pending;
            idx_pending = -1;
            ring[idx].slot = Slot::InFlight;
            idx_in_flight = idx;
            need_modeset = !modeset_done;
        }

        // Коміт робиться БЕЗ локу: це ioctl, і тримати на ньому м'ютекс,
        // який потрібен потоку подій, означало б їх зіштовхнути.
        if (!commit(idx, need_modeset)) {
            std::lock_guard<std::mutex> lock(mtx);
            ring[idx].slot = Slot::Free;
            idx_in_flight = -1;
            return;
        }
        modeset_done = true;
    }

    // --- кільце буферів ---

    bool create_ring() {
        destroy_ring();
        if (!gbm) return false;

        const int n = cfg.buffers > 0 ? cfg.buffers : 2;
        ring.resize(n);
        for (int i = 0; i < n; ++i) {
            RingBuf& b = ring[i];

            // SCANOUT обов'язковий: без нього буфер намалюється, але
            // показати його не вийде — у сканування інші вимоги до
            // вирівнювання й розміщення. RENDERING — щоб у нього могло
            // писати GL.
            b.bo = gbm_bo_create(gbm, state_.width, state_.height, state_.fourcc,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
            if (!b.bo) {
                std::fprintf(stderr,
                    "[дисплей] gbm_bo_create(%dx%d, 0x%08x) провалився\n",
                    state_.width, state_.height, state_.fourcc);
                destroy_ring();
                return false;
            }
            b.fd = gbm_bo_get_fd(b.bo);
            b.stride = gbm_bo_get_stride(b.bo);
            b.slot = Slot::Free;
            live_bufs++;

            uint32_t handles[4] = {}, strides[4] = {}, offsets[4] = {};
            if (drmPrimeFDToHandle(fd, b.fd, &handles[0]) != 0) {
                std::fprintf(stderr, "[дисплей] drmPrimeFDToHandle: %s\n",
                             std::strerror(errno));
                destroy_ring();
                return false;
            }
            strides[0] = b.stride;
            if (drmModeAddFB2(fd, state_.width, state_.height, state_.fourcc,
                              handles, strides, offsets, &b.fb_id, 0) != 0) {
                std::fprintf(stderr, "[дисплей] drmModeAddFB2: %s\n",
                             std::strerror(errno));
                destroy_ring();
                return false;
            }
        }
        idx_pending = idx_in_flight = idx_current = -1;
        return true;
    }

    void free_buf(RingBuf& b) {
        if (b.fb_id && fd >= 0) drmModeRmFB(fd, b.fb_id);
        if (b.fd >= 0) ::close(b.fd);
        if (b.bo) { gbm_bo_destroy(b.bo); live_bufs--; }
        b = RingBuf{};
    }

    static const char* slot_name(Slot s) {
        switch (s) {
            case Slot::Free:     return "Free";
            case Slot::Drawing:  return "Drawing";
            case Slot::Pending:  return "Pending";
            case Slot::InFlight: return "InFlight";
            case Slot::Current:  return "Current";
        }
        return "?";
    }

    void destroy_ring() {
        int held = 0;
        if (!ring.empty()) {
            std::string st_str;
            for (const RingBuf& b : ring) { st_str += slot_name(b.slot); st_str += " "; }
            std::fprintf(stderr, "[дисплей] стан кільця на розбиранні: %s\n", st_str.c_str());
        }
        for (RingBuf& b : ring) {
            if (b.slot == Slot::Drawing) {
                // У ньому ЗАРАЗ малюють — звільнимо, коли повернуть.
                retired.push_back(b);
                held++;
                continue;
            }
            free_buf(b);
        }
        if (!ring.empty()) {
            std::fprintf(stderr,
                "[дисплей] кільце розібрано: %d звільнено одразу, %d чекає рендерера"
                " | живих буферів %d\n",
                (int)ring.size() - held, held, live_bufs);
        }
        ring.clear();
        idx_pending = idx_in_flight = idx_current = -1;
    }

    // Повернувся буфер від попередньої конфігурації виводу. Ключ —
    // дескриптор, а не номер у кільці: кільця вже немає.
    void release_retired(int dmabuf_fd) {
        for (size_t i = 0; i < retired.size(); ++i) {
            if (retired[i].fd != dmabuf_fd) continue;
            free_buf(retired[i]);
            retired.erase(retired.begin() + (long)i);
            std::fprintf(stderr,
                "[дисплей] відкладений буфер fd=%d повернувся й звільнений"
                " | чекає ще %d | живих %d\n",
                dmabuf_fd, (int)retired.size(), live_bufs);
            return;
        }
    }

    // Рендерер зупинився й уже нічого не поверне.
    void free_all_retired() {
        if (!retired.empty()) {
            std::fprintf(stderr, "[дисплей] примусово звільняю %d відкладених буферів\n",
                         (int)retired.size());
        }
        for (RingBuf& b : retired) free_buf(b);
        retired.clear();
    }

    // --- внутрішнє ---


    bool commit(int idx, bool allow_modeset) {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (!req) return false;

        // Цільовий буфер завжди на весь екран: кроп буває лише в
        // кадрів від декодера, а ці ми виділяємо самі.
        const Rect vis{0, 0, state_.width, state_.height};

        if (allow_modeset) {
            drmModeAtomicAddProperty(req, crtc_id, crtc_props["MODE_ID"], mode_blob);
            drmModeAtomicAddProperty(req, crtc_id, crtc_props["ACTIVE"], 1);
            drmModeAtomicAddProperty(req, connector_id, conn_props["CRTC_ID"], crtc_id);
        }

        drmModeAtomicAddProperty(req, plane_id, plane_props["FB_ID"], ring[idx].fb_id);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_ID"], crtc_id);

        // CRTC_* — звичайні пікселі екрана.
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_X"], 0);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_Y"], 0);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_W"], state_.width);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_H"], state_.height);

        // SRC_* — формат 16.16 з фіксованою комою, тобто <<16. Класична
        // пастка: переплутавши, отримуєш чорний екран без жодної помилки.
        drmModeAtomicAddProperty(req, plane_id, plane_props["SRC_X"], (uint64_t)vis.x << 16);
        drmModeAtomicAddProperty(req, plane_id, plane_props["SRC_Y"], (uint64_t)vis.y << 16);
        drmModeAtomicAddProperty(req, plane_id, plane_props["SRC_W"], (uint64_t)vis.w << 16);
        drmModeAtomicAddProperty(req, plane_id, plane_props["SRC_H"], (uint64_t)vis.h << 16);

        uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
        if (allow_modeset) {
            // Зміна режиму несумісна з NONBLOCK: ядро відхилить.
            flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
        }

        int ret = drmModeAtomicCommit(fd, req, flags, this);
        drmModeAtomicFree(req);

        if (ret != 0) {
            // Поки ми готували коміт, потік подій міг зняти вивід:
            // монітор висмикнули. Це не помилка, це гонка з гарячою
            // заміною, і кадр однаково не було куди показувати.
            if (has_output.load(std::memory_order_acquire)) {
                std::fprintf(stderr, "[дисплей] atomic commit%s: %s\n",
                             allow_modeset ? " (modeset)" : "", std::strerror(errno));
            }
            return false;
        }
        return true;
    }

    // Викликається з потоку подій після ПІДТВЕРДЖЕННЯ показу.
    // seq — лічильник розгорток від ядра.
    void on_flip(int64_t when_ns, unsigned seq) {
        {
            std::lock_guard<std::mutex> lock(mtx);

            // ТОЧНА частота розгортки. Рахується по лічильнику vblank'ів
            // на довгому вікні, а не по сусідніх інтервалах: похибка
            // мітки часу ділиться на довжину вікна, тож уже за секунду
            // виходить краще за 0.0005 Гц. Це принципово — камера
            // підстроюється кроком 1 мГц, і міряти грубіше за її крок
            // означало б ганяти контролер по шуму.
            //
            // Лічильник, а не кількість подій: якщо ми пропустили
            // розгортку, seq це врахує, а підрахунок подій — ні.
            if (hz_seq_valid) {
                const uint32_t dseq = seq - hz_anchor_seq;   // беззнакове, коректне через переповнення
                const int64_t span = when_ns - hz_anchor_ns;
                if (span > 1000000000LL && dseq > 0) {
                    st.measured_hz = double(dseq) * 1e9 / double(span);
                    // Переанкорення: щоб оцінка йшла за повільним дрейфом
                    // кварцу, а не усереднювала весь час роботи.
                    if (span > 30000000000LL) {
                        hz_anchor_ns = when_ns;
                        hz_anchor_seq = seq;
                    }
                }
            } else {
                hz_anchor_ns = when_ns;
                hz_anchor_seq = seq;
                hz_seq_valid = true;
            }
            // Кадр, що був на екрані, більше не читається сканером —
            // САМЕ ТУТ він звільняється, і саме про цю мить рендереру
            // треба знати. Не після коміту: після коміту буфер ще
            // сканується, і малювати в нього означало б рвати картинку.
            if (idx_current >= 0) ring[idx_current].slot = Slot::Free;
            idx_current = idx_in_flight;
            if (idx_current >= 0) ring[idx_current].slot = Slot::Current;
            idx_in_flight = -1;

            st.presented++;
            st.last_present_ns = when_ns;
        }

        // Наступний кадр, якщо він уже чекає. Робиться тут, а не в
        // present(): один CRTC тримає лише один flip у польоті, тож
        // штовхнути чергу може саме підтвердження попереднього.
        try_commit();

        // Колбек ОСТАННІМ і поза локом: він будить рендерер, а той одразу
        // піде питати буфер — стан має бути вже узгоджений.
        FlipCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx);
            cb = on_flip_cb;
        }
        if (cb) cb(when_ns);
    }

    void event_loop() {
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = [](int, unsigned seq, unsigned tv_sec,
                                   unsigned tv_usec, void* data) {
            // Мітка ЯДРА — момент самої розгортки, а не момент, коли цей
            // потік прокинувся її обробляти. Різниця — затримка доставки
            // події, сотні мікросекунд із власним джитером; вона однаково
            // отруювала б і вимір частоти, і вимір фази, і розрахунок
            // дедлайну опиту, бо всі три відлічуються звідси.
            // Годинник той самий, CLOCK_MONOTONIC (DRM_CAP_TIMESTAMP_MONOTONIC).
            const int64_t ts = int64_t(tv_sec) * 1000000000LL + int64_t(tv_usec) * 1000LL;
            static_cast<Impl*>(data)->on_flip(ts > 0 ? ts : now_ns(), seq);
        };

        while (running.load(std::memory_order_relaxed)) {
            struct pollfd pfd[2];
            pfd[0] = {fd, POLLIN, 0};
            pfd[1] = {udev_fd, POLLIN, 0};
            const int n = udev_fd >= 0 ? 2 : 1;

            int r = poll(pfd, n, 200);

            if (r > 0 && (pfd[0].revents & POLLIN)) {
                drmHandleEvent(fd, &ctx);
            }

            bool recheck = false;
            if (r > 0 && n > 1 && (pfd[1].revents & POLLIN)) {
                recheck = drain_uevents(udev_fd);
            }

            // ПЕРЕВІРКА ЗА ТАЙМЕРОМ, а не лише на подію. Дві різні
            // причини, і обидві реальні:
            //
            //   - сокета uevent може не бути взагалі (немає прав) —
            //     тоді це єдиний спосіб помітити монітор;
            //   - виводу може не бути, хоч кабель на місці. Наприклад
            //     зонд конектора не вдався при старті. Події тоді не
            //     буде НІКОЛИ — кабель ніхто не чіпає, — і станція
            //     лишилася б без картинки назавжди. Спіймано на
            //     примусовому розбиранні: вивід не піднявся жодного разу
            //     за 24 секунди, бо чекати не було на що.
            {
                const int64_t t = now_ns();
                const int64_t every = has_output.load(std::memory_order_acquire)
                                    ? 5000000000LL    // вивід є: рідка страховка
                                    : 1000000000LL;   // виводу немає: пробуємо частіше
                if (udev_fd < 0 || !has_output.load(std::memory_order_acquire)) {
                    if (t - last_poll_ns > every) {
                        last_poll_ns = t;
                        recheck = true;
                    }
                }
            }

            if (recheck) handle_hotplug();

            // ПЕРЕВІРКА ВІДКЛАДЕНОГО ЗВІЛЬНЕННЯ, лише для діагностики:
            // VRX_TEST_TEARDOWN=<мс> змушує розбирати й піднімати вивід
            // за таймером.
            //
            // Кабелем цей шлях не перевірити: коли монітор висмикують,
            // спершу зупиняються розгортки, рендерер добиває кадр і стає
            // чекати — тобто до розбирання він уже нічого не тримає.
            // А небезпечне вікно існує: буфер у нього в руках ~15 мс із
            // 17, і uevent може прилетіти саме туди. Таймер б'є в
            // випадковий момент циклу й влучає в нього майже завжди.
            if (test_teardown_ms > 0) {
                const int64_t t = now_ns();
                if (t - last_test_ns > (int64_t)test_teardown_ms * 1000000LL) {
                    last_test_ns = t;

                    // ЗАТРИМКИ ТУТ БУТИ НЕ МОЖЕ. Спроба зсунути момент
                    // розбирання через usleep у цьому потоці зіпсувала
                    // сам вимір: поки він спить, flip'и не обробляються,
                    // дедлайн опиту зсувається, і рендерер починає
                    // малювати одразу після захоплення — буфер проскакує
                    // Drawing за мілісекунди. 52 розбирання, нуль влучань.
                    //
                    // Тримати буфер довше має РЕНДЕРЕР: VRX_TEST_HOLD_MS.

                    std::fprintf(stderr, "[тест] примусове розбирання виводу\n");
                    teardown_output();
                    configure_output();
                }
            }
        }
    }

    // Реакція на подію підсистеми drm. Подія каже лише "щось сталося",
    // тож стан з'ясовуємо самі й діємо лише на РЕАЛЬНУ зміну: uevent-и
    // сиплються й на власні modeset-и, і зациклитись тут дуже легко.
    void handle_hotplug() {
        if (refreshing.load(std::memory_order_acquire)) return;
        const bool have = has_output.load(std::memory_order_acquire);

        if (have) {
            // Наш коннектор ще на місці? drmModeGetConnectorCurrent НЕ
            // ініціює зчитування EDID по DDC — саме те, що треба для
            // частої перевірки.
            drmModeConnector* c = drmModeGetConnectorCurrent(fd, connector_id);
            const bool alive = c && c->connection == DRM_MODE_CONNECTED;
            if (c) drmModeFreeConnector(c);
            if (alive) return;

            std::fprintf(stderr, "[дисплей] монітор відключено\n");
            teardown_output();
            return;
        }

        // Виводу немає — пробуємо підняти. Якщо ще нічого не під'єднано,
        // configure_output() тихо повернеться з невдачею.
        if (configure_output()) {
            std::fprintf(stderr, "[дисплей] монітор підключено: %s\n", state_.name.c_str());
        }
    }

    int64_t last_poll_ns = 0;

    // Діагностика: примусове розбирання виводу за таймером.
    int test_teardown_ms = getenv("VRX_TEST_TEARDOWN")
                         ? atoi(getenv("VRX_TEST_TEARDOWN")) : 0;
    int64_t last_test_ns = 0;
};

// ---------------------------------------------------------------------

Display::Display() : Display(Config{}) {}

Display::Display(Config cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

Display::~Display() { close(); }

bool Display::open() {
    Impl& d = *impl_;
    if (d.fd >= 0) return true;

    d.fd = ::open(d.cfg.card.c_str(), O_RDWR | O_CLOEXEC);
    if (d.fd < 0) {
        std::fprintf(stderr, "[дисплей] open(%s): %s\n",
                     d.cfg.card.c_str(), std::strerror(errno));
        return false;
    }

    if (d.cfg.become_master) {
        if (drmSetMaster(d.fd) != 0) {
            std::fprintf(stderr,
                "[дисплей] drmSetMaster: %s — DRM master уже зайнятий "
                "(графічна сесія? інша копія?)\n", std::strerror(errno));
            close();
            return false;
        }
        d.master_taken = true;
    }

    // Без ATOMIC немає atomic commit; без UNIVERSAL_PLANES ядро ховає
    // плейни, лишаючи легасі-АПІ.
    if (drmSetClientCap(d.fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0 ||
        drmSetClientCap(d.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        std::fprintf(stderr, "[дисплей] драйвер не підтримує atomic/universal planes\n");
        close();
        return false;
    }

    // GBM-пристрій на ТОМУ САМОМУ дескрипторі. Через нього виділяються
    // цільові буфери, і він же віддається рендереру як нативний хендл
    // для EGL — щоб на всю програму лишився один пристрій і один
    // власник, а не два дескриптори однієї карти, як було раніше.
    d.gbm = gbm_create_device(d.fd);
    if (!d.gbm) {
        std::fprintf(stderr, "[дисплей] gbm_create_device провалився\n");
        close();
        return false;
    }

    // Гаряча заміна: слухаємо uevent-и підсистеми drm. Без сокета
    // програма працює, просто без реакції на перепідключення.
    d.udev_fd = open_uevent_socket();
    if (d.udev_fd < 0) {
        std::fprintf(stderr, "[дисплей] uevent-сокет не відкрився — гарячої заміни не буде\n");
    }

    d.running.store(true, std::memory_order_relaxed);
    d.event_thread = std::thread([&d] { d.event_loop(); });

    // Дисплея може ще не бути: увімкнуть монітор — підхопимо самі.
    if (!d.configure_output()) {
        std::fprintf(stderr, "[дисплей] виводу поки немає, чекаю підключення монітора\n");
    }
    return true;
}

// Підбирає коннектор, режим, CRTC і плейн та робить modeset.
// Викликається і на старті, і при кожній гарячій заміні монітора.
bool Display::Impl::configure_output() {
    Impl& d = *this;
    if (d.fd < 0) return false;

    // Починаємо з чистого аркуша: після заміни монітора id коннектора,
    // CRTC і плейна майже напевно інші, а пошук нижче спирається на те,
    // що вони обнулені.
    d.teardown_output();

    // Кеш EDID у драйвері скидаємо ДО зонда, інакше піднімемо режим
    // попереднього монітора. Прапорець тримає handle_hotplug осторонь:
    // ці записи самі породжують uevent-и, і без нього ми б нескінченно
    // переналаштовувались на власні ж події.
    d.refreshing.store(true, std::memory_order_release);
    force_edid_refresh(d.cfg.card);
    d.refreshing.store(false, std::memory_order_release);

    drmModeRes* res = drmModeGetResources(d.fd);
    if (!res) {
        std::fprintf(stderr, "[дисплей] drmModeGetResources: %s\n", std::strerror(errno));
        teardown_output();
        return false;
    }

    // --- коннектор: перший підключений ---
    drmModeConnector* conn = nullptr;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* c = drmModeGetConnector(d.fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) {
        // МОВЧКИ: сюди ми приходимо щоразу, поки монітора немає.
        drmModeFreeResources(res);
        return false;
    }
    d.connector_id = conn->connector_id;

    // --- режим: той, що просить дисплей ---
    const drmModeModeInfo* chosen = nullptr;
    if (d.cfg.want_width > 0 && d.cfg.want_height > 0) {
        for (int i = 0; i < conn->count_modes; ++i) {
            const drmModeModeInfo& m = conn->modes[i];
            if (m.hdisplay == d.cfg.want_width && m.vdisplay == d.cfg.want_height) {
                chosen = &m;
                break;
            }
        }
        if (!chosen) {
            std::fprintf(stderr,
                "[дисплей] режим %dx%d не знайдено, беру PREFERRED\n",
                d.cfg.want_width, d.cfg.want_height);
        }
    }
    if (!chosen) {
        for (int i = 0; i < conn->count_modes; ++i) {
            if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                chosen = &conn->modes[i];
                break;
            }
        }
    }
    if (!chosen) chosen = &conn->modes[0];   // PREFERRED не позначений — беремо перший
    d.mode = *chosen;

    // --- опускаємо частоту розгортки, не чіпаючи піксельний клок ---
    //
    // Рядкова частота (clock/htotal) лишається незмінною — саме до неї
    // підлаштовується приймач сигналу. Росте лише кількість рядків у
    // кадрі, тобто задній порт. Деталі й причина — у Config.
    if (d.cfg.target_refresh_hz > 1.0 && d.mode.htotal && d.mode.vtotal) {
        const uint16_t was = d.mode.vtotal;
        const double line_hz = double(d.mode.clock) * 1000.0 / d.mode.htotal;
        const double before = line_hz / was;
        const long want = std::lround(line_hz / d.cfg.target_refresh_hz);
        const long limit = std::lround(d.mode.vtotal * (1.0 + d.cfg.vtotal_max_growth));

        if (want <= d.mode.vtotal) {
            // Підняти частоту так не можна: рядки лише додаються.
            std::fprintf(stderr,
                "[дисплей] %.3f Гц не нижче за поточні %.3f — режим лишаю як є\n",
                d.cfg.target_refresh_hz, before);
        } else if (want > limit) {
            std::fprintf(stderr,
                "[дисплей] %.3f Гц вимагає vtotal %ld проти %u — надто далеко, лишаю як є\n",
                d.cfg.target_refresh_hz, want, was);
        } else {
            d.mode.vtotal = (uint16_t)want;
            const double after = line_hz / d.mode.vtotal;
            d.mode.vrefresh = (uint32_t)(after + 0.5);
            std::fprintf(stderr,
                "[дисплей] vtotal %u -> %ld (+%ld рядків), частота %.3f -> %.3f Гц"
                " | рядкова %.3f кГц і клок %u кГц незмінні\n",
                was, want, want - was, before, after,
                line_hz / 1000.0, d.mode.clock);
        }
    }

    // --- CRTC: той, що може живити цей коннектор ---
    for (int i = 0; i < conn->count_encoders && !d.crtc_id; ++i) {
        drmModeEncoder* enc = drmModeGetEncoder(d.fd, conn->encoders[i]);
        if (!enc) continue;
        for (int j = 0; j < res->count_crtcs; ++j) {
            if (enc->possible_crtcs & (1u << j)) {
                d.crtc_id = res->crtcs[j];
                break;
            }
        }
        drmModeFreeEncoder(enc);
    }
    if (!d.crtc_id) {
        std::fprintf(stderr, "[дисплей] не знайшовся CRTC для коннектора\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        teardown_output();
        return false;
    }

    // Індекс CRTC у масиві — саме він, а не id, стоїть у масках
    // possible_crtcs у плейнів.
    int crtc_index = -1;
    for (int j = 0; j < res->count_crtcs; ++j) {
        if (res->crtcs[j] == d.crtc_id) { crtc_index = j; break; }
    }

    // --- плейн: primary на цьому CRTC ---
    drmModePlaneRes* planes = drmModeGetPlaneResources(d.fd);
    std::vector<uint32_t> formats;
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes && !d.plane_id; ++i) {
            drmModePlane* p = drmModeGetPlane(d.fd, planes->planes[i]);
            if (!p) continue;
            bool on_our_crtc = crtc_index >= 0 && (p->possible_crtcs & (1u << crtc_index));
            if (on_our_crtc) {
                PropTable pp = read_props(d.fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
                auto it = pp.value.find("type");
                bool is_primary = it != pp.value.end() && it->second == DRM_PLANE_TYPE_PRIMARY;
                if (is_primary) {
                    d.plane_id = p->plane_id;
                    for (uint32_t k = 0; k < p->count_formats; ++k) {
                        formats.push_back(p->formats[k]);
                    }
                }
            }
            drmModeFreePlane(p);
        }
        drmModeFreePlaneResources(planes);
    }
    if (!d.plane_id) {
        std::fprintf(stderr, "[дисплей] не знайшовся primary-плейн на CRTC %u\n", d.crtc_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        teardown_output();
        return false;
    }

    d.plane_props = read_props(d.fd, d.plane_id, DRM_MODE_OBJECT_PLANE);
    d.crtc_props  = read_props(d.fd, d.crtc_id,  DRM_MODE_OBJECT_CRTC);
    d.conn_props  = read_props(d.fd, d.connector_id, DRM_MODE_OBJECT_CONNECTOR);

    if (drmModeCreatePropertyBlob(d.fd, &d.mode, sizeof(d.mode), &d.mode_blob) != 0) {
        std::fprintf(stderr, "[дисплей] drmModeCreatePropertyBlob: %s\n", std::strerror(errno));
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        teardown_output();
        return false;
    }

    // --- пінимо колір ---
    // Імена властивостей різні: mainline дає стандартну "max bpc", ядра
    // Rockchip BSP — вендорні "color_depth"/"color_format". Шукаємо
    // обидва варіанти й спокійно живемо, якщо немає жодного.
    {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        bool any = false;

        if (d.conn_props.has("max bpc")) {
            drmModeAtomicAddProperty(req, d.connector_id, d.conn_props["max bpc"],
                                      d.cfg.color_depth_bits);
            any = true;
        } else if (d.conn_props.has("color_depth")) {
            const char* want = d.cfg.color_depth_bits >= 10 ? "30bit" : "24bit";
            uint64_t v = 0;
            if (enum_value_by_name(d.fd, d.conn_props["color_depth"], want, &v)) {
                drmModeAtomicAddProperty(req, d.connector_id, d.conn_props["color_depth"], v);
                any = true;
            }
        }

        if (d.conn_props.has("color_format")) {
            uint64_t v = 0;
            if (enum_value_by_name(d.fd, d.conn_props["color_format"],
                                    color_format_name(d.cfg.color_format), &v)) {
                drmModeAtomicAddProperty(req, d.connector_id, d.conn_props["color_format"], v);
                any = true;
            }
        }

        if (any) {
            int r = drmModeAtomicCommit(d.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            if (r != 0) {
                std::fprintf(stderr,
                    "[дисплей] не вдалося запінити колір (%s) — лишаю як є\n",
                    std::strerror(errno));
            }
        }
        drmModeAtomicFree(req);
    }

    // --- метадані шару ---
    d.state_.width = d.mode.hdisplay;
    d.state_.height = d.mode.vdisplay;
    // vrefresh у drmModeModeInfo — округлені герци; рахуємо точніше з
    // піксельного клока й повних розмірів, бо саме дробова частина
    // визначає дрейф відносно джерела.
    if (d.mode.htotal && d.mode.vtotal) {
        d.state_.refresh_mhz =
            (int)((int64_t)d.mode.clock * 1000000LL / (d.mode.htotal * d.mode.vtotal));
    } else {
        d.state_.refresh_mhz = d.mode.vrefresh * 1000;
    }
    d.state_.supported_formats = formats;
    d.state_.fourcc = DRM_FORMAT_XRGB8888;
    d.state_.modifier = DRM_FORMAT_MOD_LINEAR;
    d.state_.colorspace = ColorSpace::BT709;
    d.state_.color_range = ColorRange::Full;
    d.state_.color_format = d.cfg.color_format;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s %dx%d@%.3f",
                  conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ? "HDMI-A" : "conn",
                  d.state_.width, d.state_.height, d.state_.refresh_hz());
    d.state_.name = buf;

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);


    std::fprintf(stderr,
        "[дисплей] %s | crtc=%u plane=%u | %d форматів | колір: %s %d біт | бюджет кадру %.2f мс\n",
        d.state_.name.c_str(), d.crtc_id, d.plane_id, (int)formats.size(),
        color_format_name(d.cfg.color_format), d.cfg.color_depth_bits,
        d.state_.frame_time_ns() / 1e6);


    // Буфери під цю геометрію. Робиться ТУТ, а не при відкритті: до
    // появи монітора невідомо ні розміру, ні формату.
    {
        std::lock_guard<std::mutex> lock(d.mtx);
        if (!d.create_ring()) {
            drmModeFreeConnector(conn);
            drmModeFreeResources(res);
            return false;
        }
    }

    // Рендерер стежить саме за цим номером: змінився — значить
    // геометрія виводу інша, і буфери треба перестворити.
    d.state_.generation = generation_.fetch_add(1, std::memory_order_release) + 1;
    d.has_output.store(true, std::memory_order_release);
    return true;
}

void Display::close() {
    Impl& d = *impl_;

    if (d.running.exchange(false)) {
        if (d.event_thread.joinable()) d.event_thread.join();
    }

    if (d.fd >= 0) {
        // Вимикаємо плейн явно, інакше на екрані лишиться останній кадр
        // і наступна сесія побачить "застряглий" плейн.
        if (d.plane_id && d.plane_props.has("FB_ID")) {
            drmModeAtomicReq* req = drmModeAtomicAlloc();
            drmModeAtomicAddProperty(req, d.plane_id, d.plane_props["FB_ID"], 0);
            drmModeAtomicAddProperty(req, d.plane_id, d.plane_props["CRTC_ID"], 0);
            drmModeAtomicCommit(d.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            drmModeAtomicFree(req);
        }

        if (d.mode_blob) {
            drmModeDestroyPropertyBlob(d.fd, d.mode_blob);
            d.mode_blob = 0;
        }
        if (d.master_taken) {
            drmDropMaster(d.fd);
            d.master_taken = false;
        }
    }
    if (d.udev_fd >= 0) {
        ::close(d.udev_fd);
        d.udev_fd = -1;
    }
    if (d.fd >= 0) {
        ::close(d.fd);
        d.fd = -1;
    }

    d.destroy_ring();
    d.free_all_retired();
    if (d.gbm) { gbm_device_destroy(d.gbm); d.gbm = nullptr; }
    d.modeset_done = false;
    d.state_ = {};
}

PresentStats Display::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    PresentStats s = impl_->st;
    s.live_bufs = impl_->live_bufs;
    s.retired_bufs = (int)impl_->retired.size();
    s.generation = impl_->state_.generation;
    return s;
}

OutputState Display::state() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->state_;
}


bool Display::acquire(Target& out) { return impl_->acquire(out); }

bool Display::present(const Target& t) { return impl_->queue(t); }

void Display::set_flip_callback(FlipCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->on_flip_cb = std::move(cb);
}

void* Display::native_handle() const { return impl_->gbm; }

} // namespace vrx::display
