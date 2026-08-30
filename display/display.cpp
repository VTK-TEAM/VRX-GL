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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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

// Ім'я типу коннектора так, як його пише ядро в /sys/class/drm.
// Потрібне не для краси: повне ім'я ("HDMI-A-1") — сталий ключ, за яким
// зберігаються розкладки вікон конкретного екрана.
const char* connector_type_name(uint32_t t) {
    switch (t) {
        case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_eDP:         return "eDP";
        case DRM_MODE_CONNECTOR_DSI:         return "DSI";
        case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
        case DRM_MODE_CONNECTOR_VGA:         return "VGA";
        case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
        default:                             return "conn";
    }
}

// ПРАВИЛО РОЛЕЙ. Менше число — вагоміша заявка на "основний".
//
// Роль визначає ТИП КОННЕКТОРА, а не історія підключень: HDMI основний
// завжди, коли він є. Лишився один вивід — він основний, який би не був.
//
// Ціна цього правила прийнята свідомо: повернення HDMI розжалує DP назад,
// а кожна зміна основного скидає петлю фази, і камера захоплюється заново
// кілька секунд. Липка промоція цього не мала б, але вона суперечила б
// самому правилу.
int role_rank(uint32_t connector_type) {
    switch (connector_type) {
        case DRM_MODE_CONNECTOR_HDMIA:
        case DRM_MODE_CONNECTOR_HDMIB:       return 0;
        case DRM_MODE_CONNECTOR_DisplayPort:
        case DRM_MODE_CONNECTOR_eDP:         return 1;
        default:                             return 2;
    }
}

} // namespace

// ---------------------------------------------------------------------

struct Display::Impl {
    explicit Impl(Config c) : cfg(std::move(c)) {}

    Config cfg;
    int fd = -1;
    bool master_taken = false;
    struct gbm_device* gbm = nullptr;

    // П'ять станів буфера, і кожен означає конкретну заборону:
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

    // ОДИН ВИВІД: коннектор, CRTC, плейн і ВЛАСНЕ кільце буферів.
    //
    // Кільце саме власне, а не спільне: розміри екранів різні, а буфер
    // сканується тим CRTC, під який виділявся. Спільний пул означав би
    // або буфери за найбільшим екраном (марна смуга пам'яті), або
    // перевиділення на кожен показ.
    //
    // Вимір частоти розгортки теж тут: у кожного виводу свій кварц, і
    // спільного лічильника vblank'ів не існує в принципі.
    struct Output {
        Impl* owner = nullptr;      // маршрутизація подій: flip знає лише цей вказівник
        int slot_no = -1;

        uint32_t connector_id = 0;
        uint32_t connector_type = 0;
        uint32_t crtc_id = 0;
        uint32_t plane_id = 0;
        drmModeModeInfo mode{};
        uint32_t mode_blob = 0;
        PropTable plane_props, crtc_props, conn_props;

        OutputState state{};
        PresentStats st{};

        std::vector<RingBuf> ring;
        int idx_pending = -1, idx_in_flight = -1, idx_current = -1;
        bool modeset_done = false;

        int64_t hz_anchor_ns = 0;
        uint32_t hz_anchor_seq = 0;
        bool hz_seq_valid = false;

        bool live() const { return state.generation != 0; }
    };

    // СЛОТИ, А НЕ СПИСОК, І ВОНИ НІКОЛИ НЕ ПЕРЕНУМЕРОВУЮТЬСЯ.
    //
    // Слот, що звільнився (монітор від'єднали), лишається порожнім і
    // чекає на наступного — замість того щоб зсувати сусідів. Причина
    // проста: номер слота лежить у виданому рендереру Target, і зсув
    // номерів повернув би буфер у ЧУЖЕ кільце. Ролі при цьому
    // переставляються скільки завгодно — вони живуть окремо, у
    // primary_slot.
    std::vector<std::unique_ptr<Output>> outs;
    int primary_slot = -1;

    // БУФЕРИ, ЯКІ РОЗІБРАЛИ З-ПІД РЕНДЕРЕРА.
    //
    // Монітор можуть висмикнути в будь-яку мить, зокрема посеред проходу
    // GPU. Знищити буфер у стані Drawing означало б закрити дескриптор і
    // віддати пам'ять, поки в неї пишуть. Тому такі буфери переїжджають
    // сюди й чекають, поки рендерер поверне їх через present().
    //
    // Список СПІЛЬНИЙ на всі виводи: ключ тут дескриптор dmabuf, а він
    // унікальний, і кільця, з якого буфер прийшов, на той момент уже
    // немає.
    std::vector<RingBuf> retired;

    // Скільки буферів виділено й ще не звільнено. У сталому режимі —
    // сума глибин кілець усіх живих виводів; якщо після замін монітора
    // росте, значить тече.
    int live_bufs = 0;

    mutable std::mutex mtx;

    std::thread event_thread;
    std::atomic<bool> running{false};

    int udev_fd = -1;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> refreshing{false};

    // Скільки виводів піднято. Атомарне, бо його читає рендерер поза
    // локом, щоб не будити м'ютекс на кожен кадр.
    std::atomic<int> live_count{0};

    FlipCallback on_flip_cb;

    int64_t last_poll_ns = 0;

    // КОННЕКТОРИ, ЯКІ НЕ ВДАЛОСЯ ПІДНЯТИ, і коли пробувати знову.
    //
    // Без цього кожні дві секунди ми повторювали б підняття коннектора,
    // якому не знайшлося ні вільного CRTC, ні плейна — а разом із ним і
    // скидання кешу EDID, тобто пів секунди сну на коннектор ПРЯМО в
    // потоці подій. Той самий клас, що вже лікували паузою в сторожі
    // джерела й у монтуванні носія.
    std::map<uint32_t, int64_t> add_retry_ns;

    // Діагностика: примусове розбирання виводу за таймером.
    int test_teardown_ms = getenv("VRX_TEST_TEARDOWN")
                         ? atoi(getenv("VRX_TEST_TEARDOWN")) : 0;
    int64_t last_test_ns = 0;

    // --- доступ до виводів (усе під mtx, якщо не сказано інакше) ---

    Output* at(int slot) {
        if (slot < 0 || (size_t)slot >= outs.size()) return nullptr;
        Output* o = outs[(size_t)slot].get();
        return (o && o->live()) ? o : nullptr;
    }

    Output* primary() { return at(primary_slot); }

    // --- кільце буферів ---

    bool create_ring(Output& o) {
        destroy_ring(o);
        if (!gbm) return false;

        const int n = cfg.buffers > 0 ? cfg.buffers : 2;
        o.ring.resize(n);
        for (int i = 0; i < n; ++i) {
            RingBuf& b = o.ring[i];

            // SCANOUT обов'язковий: без нього буфер намалюється, але
            // показати його не вийде — у сканування інші вимоги до
            // вирівнювання й розміщення. RENDERING — щоб у нього могло
            // писати GL.
            b.bo = gbm_bo_create(gbm, o.state.width, o.state.height, o.state.fourcc,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
            if (!b.bo) {
                std::fprintf(stderr,
                    "[дисплей] %s: gbm_bo_create(%dx%d, 0x%08x) провалився\n",
                    o.state.connector.c_str(), o.state.width, o.state.height,
                    o.state.fourcc);
                destroy_ring(o);
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
                destroy_ring(o);
                return false;
            }
            strides[0] = b.stride;
            if (drmModeAddFB2(fd, o.state.width, o.state.height, o.state.fourcc,
                              handles, strides, offsets, &b.fb_id, 0) != 0) {
                std::fprintf(stderr, "[дисплей] drmModeAddFB2: %s\n",
                             std::strerror(errno));
                destroy_ring(o);
                return false;
            }
        }
        o.idx_pending = o.idx_in_flight = o.idx_current = -1;
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

    void destroy_ring(Output& o) {
        int held = 0;
        if (!o.ring.empty()) {
            std::string st_str;
            for (const RingBuf& b : o.ring) { st_str += slot_name(b.slot); st_str += " "; }
            std::fprintf(stderr, "[дисплей] %s: стан кільця на розбиранні: %s\n",
                         o.state.connector.c_str(), st_str.c_str());
        }
        for (RingBuf& b : o.ring) {
            if (b.slot == Slot::Drawing) {
                // У ньому ЗАРАЗ малюють — звільнимо, коли повернуть.
                retired.push_back(b);
                held++;
                continue;
            }
            free_buf(b);
        }
        if (!o.ring.empty()) {
            std::fprintf(stderr,
                "[дисплей] %s: кільце розібрано: %d звільнено одразу, %d чекає рендерера"
                " | живих буферів %d\n",
                o.state.connector.c_str(), (int)o.ring.size() - held, held, live_bufs);
        }
        o.ring.clear();
        o.idx_pending = o.idx_in_flight = o.idx_current = -1;
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

    // --- видача й показ ---

    // Опис буфера кільця у вигляді звичайного кадру — тим самим типом,
    // яким описуються кадри джерел, щоб рендерер не мав окремої гілки.
    Frame frame_of(const Output& o, int i) const {
        Frame f;
        f.fourcc = o.state.fourcc;
        f.modifier = o.state.modifier;
        f.width = o.state.width;
        f.height = o.state.height;
        f.n_planes = 1;
        f.fd[0] = o.ring[i].fd;
        f.stride[0] = o.ring[i].stride;
        f.offset[0] = 0;
        return f;
    }

    bool acquire(int slot, Target& out) {
        std::lock_guard<std::mutex> lock(mtx);
        Output* o = at(slot);
        if (!o) return false;

        for (size_t i = 0; i < o->ring.size(); ++i) {
            if (o->ring[i].slot != Slot::Free) continue;
            o->ring[i].slot = Slot::Drawing;
            out.index = (int)i;
            out.screen = o->slot_no;
            out.generation = o->state.generation;
            out.frame = frame_of(*o, (int)i);
            return true;
        }
        return false;      // усі зайняті — чекати на розгортку
    }

    // Намальоване стає pending; віддати залізу спробує try_commit().
    bool queue(const Target& t) {
        Output* o = nullptr;
        {
            std::lock_guard<std::mutex> lock(mtx);

            // ЗВІЛЬНЕННЯ ПЕРШЕ, перевірка наявності виводу після. Порядок
            // не косметичний: коли монітор висмикнули, виводу вже немає, і
            // рання відмова лишила б буфер із розібраного кільця висіти до
            // самого close(). А розбіжність generation цей випадок ловить
            // сама — teardown обнуляє стан.
            Output* cand = (t.screen >= 0 && (size_t)t.screen < outs.size())
                         ? outs[(size_t)t.screen].get() : nullptr;
            if (!cand || t.generation != cand->state.generation) {
                // Буфер від попередньої конфігурації виводу. Показувати
                // його вже нікуди — але саме зараз він нарешті вільний
                // від GPU, тож це єдиний момент, коли його можна чесно
                // звільнити.
                release_retired(t.frame.fd[0]);
                return false;
            }
            if (t.index < 0 || (size_t)t.index >= cand->ring.size()) return false;
            if (cand->ring[t.index].slot != Slot::Drawing) return false;

            // Попередній непоказаний кадр витісняється — рахуємо як дроп,
            // щоб деградацію було видно, а не доводилось здогадуватись.
            if (cand->idx_pending >= 0) {
                cand->ring[cand->idx_pending].slot = Slot::Free;
                cand->st.dropped++;
            }
            cand->ring[t.index].slot = Slot::Pending;
            cand->idx_pending = t.index;
            o = cand;
        }
        try_commit(*o);
        return true;
    }

    // Віддати залізу наступний кадр, якщо є що і якщо можна.
    //
    // Кличеться з ДВОХ місць: із queue() і з on_flip(). На один CRTC може
    // бути лише ОДИН flip у польоті — другий коміт до підтвердження
    // першого поверне EBUSY. Тому коли queue() застав зайнято, кадр чекає
    // в pending, і штовхнути його має саме підтвердження попереднього.
    void try_commit(Output& o) {
        // ВСЕ, ЩО ЧИТАЄ КІЛЬЦЕ, ЗНІМАЄТЬСЯ ТУТ, ПІД ЛОКОМ.
        //
        // Сам ioctl робиться без лока — тримати на ньому м'ютекс, який
        // потрібен потоку подій, означало б їх зіштовхнути. Але тоді й
        // читати ring[idx] у commit() не можна: поки ми поза локом, потік
        // подій може розібрати кільце (висмикнули монітор), і ми читали б
        // звільнену пам'ять вектора. Тому копіюємо ВСЕ потрібне ще під
        // локом, а далі працюємо зі значеннями.
        int idx = -1;
        uint32_t fb_id = 0;
        bool need_modeset = false;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (o.idx_in_flight >= 0 || o.idx_pending < 0) return;
            idx = o.idx_pending;
            o.idx_pending = -1;
            o.ring[idx].slot = Slot::InFlight;
            o.idx_in_flight = idx;
            fb_id = o.ring[idx].fb_id;
            need_modeset = !o.modeset_done;
        }

        if (!commit(o, fb_id, need_modeset)) {
            std::lock_guard<std::mutex> lock(mtx);
            if (idx >= 0 && (size_t)idx < o.ring.size()) o.ring[idx].slot = Slot::Free;
            o.idx_in_flight = -1;
            return;
        }
        o.modeset_done = true;
    }

    bool commit(Output& o, uint32_t fb_id, bool allow_modeset) {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (!req) return false;

        // Цільовий буфер завжди на весь екран: кроп буває лише в
        // кадрів від декодера, а ці ми виділяємо самі.
        const Rect vis{0, 0, o.state.width, o.state.height};

        if (allow_modeset) {
            drmModeAtomicAddProperty(req, o.crtc_id, o.crtc_props["MODE_ID"], o.mode_blob);
            drmModeAtomicAddProperty(req, o.crtc_id, o.crtc_props["ACTIVE"], 1);
            drmModeAtomicAddProperty(req, o.connector_id, o.conn_props["CRTC_ID"], o.crtc_id);
        }

        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["FB_ID"], fb_id);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_ID"], o.crtc_id);

        // CRTC_* — звичайні пікселі екрана.
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_X"], 0);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_Y"], 0);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_W"], o.state.width);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_H"], o.state.height);

        // SRC_* — формат 16.16 з фіксованою комою, тобто <<16. Класична
        // пастка: переплутавши, отримуєш чорний екран без жодної помилки.
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["SRC_X"], (uint64_t)vis.x << 16);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["SRC_Y"], (uint64_t)vis.y << 16);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["SRC_W"], (uint64_t)vis.w << 16);
        drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["SRC_H"], (uint64_t)vis.h << 16);

        uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
        if (allow_modeset) {
            // Зміна режиму несумісна з NONBLOCK: ядро відхилить.
            flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
        }

        // Користувацькі дані події — САМ ВИВІД. Так підтвердження
        // розгортки знаходить своє кільце, не питаючи ні в кого.
        int ret = drmModeAtomicCommit(fd, req, flags, &o);
        drmModeAtomicFree(req);

        if (ret != 0) {
            // Поки ми готували коміт, потік подій міг зняти вивід:
            // монітор висмикнули. Це не помилка, це гонка з гарячою
            // заміною, і кадр однаково не було куди показувати.
            bool still_live;
            {
                std::lock_guard<std::mutex> lock(mtx);
                still_live = o.live();
            }
            if (still_live) {
                std::fprintf(stderr, "[дисплей] %s: atomic commit%s: %s\n",
                             o.state.connector.c_str(),
                             allow_modeset ? " (modeset)" : "", std::strerror(errno));
            }
            return false;
        }
        return true;
    }

    // Викликається з потоку подій після ПІДТВЕРДЖЕННЯ показу.
    // seq — лічильник розгорток від ядра.
    void on_flip(Output& o, int64_t when_ns, unsigned seq) {
        bool is_primary = false;
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
            if (o.hz_seq_valid) {
                const uint32_t dseq = seq - o.hz_anchor_seq;   // беззнакове, коректне через переповнення
                const int64_t span = when_ns - o.hz_anchor_ns;
                if (span > 1000000000LL && dseq > 0) {
                    o.st.measured_hz = double(dseq) * 1e9 / double(span);
                    // Переанкорення: щоб оцінка йшла за повільним дрейфом
                    // кварцу, а не усереднювала весь час роботи.
                    if (span > 30000000000LL) {
                        o.hz_anchor_ns = when_ns;
                        o.hz_anchor_seq = seq;
                    }
                }
            } else {
                o.hz_anchor_ns = when_ns;
                o.hz_anchor_seq = seq;
                o.hz_seq_valid = true;
            }

            // Кадр, що був на екрані, більше не читається сканером —
            // САМЕ ТУТ він звільняється, і саме про цю мить рендереру
            // треба знати. Не після коміту: після коміту буфер ще
            // сканується, і малювати в нього означало б рвати картинку.
            if (o.idx_current >= 0) o.ring[o.idx_current].slot = Slot::Free;
            o.idx_current = o.idx_in_flight;
            if (o.idx_current >= 0) o.ring[o.idx_current].slot = Slot::Current;
            o.idx_in_flight = -1;

            o.st.presented++;
            o.st.last_present_ns = when_ns;

            is_primary = (o.slot_no == primary_slot);
        }

        // Наступний кадр, якщо він уже чекає.
        try_commit(o);

        // КОЛБЕК — ЛИШЕ ВІД ОСНОВНОГО, і поза локом.
        //
        // Він задає такт рендереру: прокидання, дедлайн опиту джерел і
        // вимір фази відлічуються саме від цієї мітки. Додатковий екран
        // має власну розгортку з власним кварцом, і будити нею рендерер
        // означало б вести камеру по двох незалежних сітках одночасно.
        if (!is_primary) return;

        FlipCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx);
            cb = on_flip_cb;
        }
        if (cb) cb(when_ns);
    }

    // --- налаштування виводів ---

    bool configure_all(bool probe_new);
    bool configure_one(drmModeRes* res, drmModeConnector* conn);
    void assign_roles();

    // Знімає ОДИН вивід, лишаючи карту й решту екранів недоторканими.
    void teardown_output(Output& o) {
        if (fd >= 0 && o.plane_id && o.plane_props.has("FB_ID")) {
            drmModeAtomicReq* req = drmModeAtomicAlloc();
            drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["FB_ID"], 0);
            drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_ID"], 0);
            drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            drmModeAtomicFree(req);
        }
        if (fd >= 0 && o.mode_blob) drmModeDestroyPropertyBlob(fd, o.mode_blob);
        o.mode_blob = 0;

        {
            std::lock_guard<std::mutex> lock(mtx);
            // Буфери належать конкретному траєкту виводу: після заміни
            // монітора вони недійсні, навіть якщо роздільність збіглася.
            destroy_ring(o);
            // Вимір частоти прив'язаний до конкретного тракту розгортки.
            o.hz_seq_valid = false;
            o.st = PresentStats{};

            o.connector_id = o.crtc_id = o.plane_id = 0;
            o.plane_props = o.crtc_props = o.conn_props = PropTable{};
            o.mode = drmModeModeInfo{};
            o.modeset_done = false;
            const std::string was = o.state.connector;
            o.state = OutputState{};
            o.state.connector = was;      // ім'я лишаємо для логів
            o.state.name = "немає виводу";
        }
    }

    void teardown_all() {
        for (auto& up : outs) {
            if (up && up->live()) teardown_output(*up);
        }
        std::lock_guard<std::mutex> lock(mtx);
        primary_slot = -1;
        live_count.store(0, std::memory_order_release);
    }

    // --- цикл подій ---

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
            if (!data) return;
            Output* o = static_cast<Output*>(data);
            const int64_t ts = int64_t(tv_sec) * 1000000000LL + int64_t(tv_usec) * 1000LL;
            o->owner->on_flip(*o, ts > 0 ? ts : now_ns(), seq);
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
            //     лишилася б без картинки назавжди.
            {
                const int64_t t = now_ns();
                if (t - last_poll_ns > 2000000000LL) {
                    last_poll_ns = t;
                    recheck = true;
                }
            }

            if (recheck) handle_hotplug();

            // ДІАГНОСТИКА: VRX_TEST_TEARDOWN=<мс> змушує розбирати й
            // піднімати виводи за таймером. Кабелем цей шлях не
            // перевірити: коли монітор висмикують, спершу зупиняються
            // розгортки, рендерер добиває кадр і стає чекати — тобто до
            // розбирання він уже нічого не тримає. А небезпечне вікно
            // існує: буфер у нього в руках ~15 мс із 17.
            if (test_teardown_ms > 0) {
                const int64_t t = now_ns();
                if (t - last_test_ns > (int64_t)test_teardown_ms * 1000000LL) {
                    last_test_ns = t;
                    std::fprintf(stderr, "[тест] примусове розбирання виводів\n");
                    teardown_all();
                    configure_all(true);
                }
            }
        }
    }

    // Реакція на подію підсистеми drm. Подія каже лише "щось сталося",
    // тож стан з'ясовуємо самі й діємо лише на РЕАЛЬНУ зміну: uevent-и
    // сиплються й на власні modeset-и, і зациклитись тут дуже легко.
    //
    // ПОШТУЧНО, А НЕ ЦІЛКОМ. Зникнення додаткового екрана не має
    // торкатися основного: там іде показ, і перебудова його кільця
    // коштувала б пілоту вікна без картинки рівно ні за що.
    void handle_hotplug() {
        if (refreshing.load(std::memory_order_acquire)) return;

        // 1) Хто з наявних відпав. drmModeGetConnectorCurrent НЕ ініціює
        //    зчитування EDID по DDC — саме те, що треба для частої
        //    перевірки.
        bool lost = false;
        for (auto& up : outs) {
            if (!up || !up->live()) continue;
            drmModeConnector* c = drmModeGetConnectorCurrent(fd, up->connector_id);
            const bool alive = c && c->connection == DRM_MODE_CONNECTED;
            if (c) drmModeFreeConnector(c);
            if (alive) continue;

            std::fprintf(stderr, "[дисплей] монітор відключено: %s\n",
                         up->state.connector.c_str());
            teardown_output(*up);
            lost = true;
        }

        // 2) Чи з'явився хтось новий. Повний зонд дорогий, тож робимо
        //    його лише тут — раз на дві секунди або на uevent.
        const bool added = configure_all(true);

        if (lost || added) assign_roles();
    }
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

    // ВИВОДИ ПІДНІМАЮТЬСЯ ДО СТАРТУ ПОТОКУ ПОДІЙ.
    //
    // Раніше потік стартував першим, і configure_output() міг піти
    // одночасно з двох боків: із головного потоку тут і з потоку подій
    // за таймером. Прапорець refreshing прикривав лише зчитування EDID,
    // а решта — зонд, вибір режиму, modeset, створення кільця — цілком
    // могла перетнутися сама з собою.
    if (!d.configure_all(true)) {
        std::fprintf(stderr, "[дисплей] виводу поки немає, чекаю підключення монітора\n");
    }
    d.assign_roles();

    d.running.store(true, std::memory_order_relaxed);
    d.event_thread = std::thread([&d] { d.event_loop(); });
    return true;
}

// Перебирає коннектори й піднімає ті, що під'єднані, але ще не мають
// виводу. Повертає true, якщо хоч один додано.
bool Display::Impl::configure_all(bool probe_new) {
    if (fd < 0) return false;

    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        std::fprintf(stderr, "[дисплей] drmModeGetResources: %s\n", std::strerror(errno));
        return false;
    }

    // Кого ми вже ведемо — щоб не піднімати вдруге.
    std::set<uint32_t> known;
    for (auto& up : outs) {
        if (up && up->live()) known.insert(up->connector_id);
    }

    // Кеш EDID у драйвері скидаємо ДО зонда, інакше піднімемо режим
    // попереднього монітора. Прапорець тримає handle_hotplug осторонь:
    // ці записи самі породжують uevent-и, і без нього ми б нескінченно
    // переналаштовувались на власні ж події.
    //
    // Робиться лише коли справді щось шукаємо: скидання коштує пів
    // секунди на коннектор, і робити його раз на дві секунди просто так
    // означало б тримати екран у постійному перезонді.
    bool refreshed = false;
    bool added = false;

    for (int i = 0; i < res->count_connectors; ++i) {
        if (!probe_new) break;

        // Дешева перевірка перед дорогою: чи взагалі є що піднімати.
        drmModeConnector* cur = drmModeGetConnectorCurrent(fd, res->connectors[i]);
        const bool maybe = cur && cur->connection == DRM_MODE_CONNECTED;
        const uint32_t cid = cur ? cur->connector_id : 0;
        if (cur) drmModeFreeConnector(cur);
        if (!maybe || known.count(cid)) continue;

        const auto rt = add_retry_ns.find(cid);
        if (rt != add_retry_ns.end() && rt->second > now_ns()) continue;

        if (!refreshed) {
            refreshing.store(true, std::memory_order_release);
            force_edid_refresh(cfg.card);
            refreshing.store(false, std::memory_order_release);
            refreshed = true;
        }

        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            if (configure_one(res, conn)) {
                added = true;
                add_retry_ns.erase(cid);
            } else {
                add_retry_ns[cid] = now_ns() + 5000000000LL;   // 5 с
            }
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return added;
}

// Піднімає ОДИН вивід на вже підтвердженому коннекторі.
bool Display::Impl::configure_one(drmModeRes* res, drmModeConnector* conn) {
    // Куди його покласти: вільний слот або новий у кінці. Слоти не
    // перенумеровуються, тож зайняті пропускаємо.
    int slot = -1;
    for (size_t i = 0; i < outs.size(); ++i) {
        if (!outs[i] || !outs[i]->live()) { slot = (int)i; break; }
    }
    if (slot < 0) {
        slot = (int)outs.size();
        outs.push_back(nullptr);
    }
    if (!outs[(size_t)slot]) outs[(size_t)slot] = std::make_unique<Output>();

    Output& o = *outs[(size_t)slot];
    o.owner = this;
    o.slot_no = slot;
    o.connector_id = conn->connector_id;
    o.connector_type = conn->connector_type;

    char cname[64];
    std::snprintf(cname, sizeof(cname), "%s-%u",
                  connector_type_name(conn->connector_type), conn->connector_type_id);

    // --- режим: той, що просить дисплей ---
    const drmModeModeInfo* chosen = nullptr;
    if (cfg.want_width > 0 && cfg.want_height > 0) {
        for (int i = 0; i < conn->count_modes; ++i) {
            const drmModeModeInfo& m = conn->modes[i];
            if (m.hdisplay == cfg.want_width && m.vdisplay == cfg.want_height) {
                chosen = &m;
                break;
            }
        }
        if (!chosen) {
            std::fprintf(stderr,
                "[дисплей] %s: режим %dx%d не знайдено, беру PREFERRED\n",
                cname, cfg.want_width, cfg.want_height);
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

    // --- стеля роздільності ---
    const bool asked_explicitly = (cfg.want_width > 0 && cfg.want_height > 0 &&
                                   chosen->hdisplay == cfg.want_width &&
                                   chosen->vdisplay == cfg.want_height);
    const bool over_cap = cfg.max_width > 0 && cfg.max_height > 0 &&
                          (chosen->hdisplay > cfg.max_width ||
                           chosen->vdisplay > cfg.max_height);
    if (over_cap && !asked_explicitly) {
        // Найбільший режим у межах стелі; за рівної площі — з вищою
        // частотою. Площа, а не ширина: 1920x1080 має перемагати
        // 1920x800, хоч вони й однакової ширини.
        const drmModeModeInfo* best = nullptr;
        long best_area = -1;
        int best_hz = -1;
        for (int i = 0; i < conn->count_modes; ++i) {
            const drmModeModeInfo& m = conn->modes[i];
            if (m.hdisplay > cfg.max_width || m.vdisplay > cfg.max_height) continue;
            const long area = (long)m.hdisplay * m.vdisplay;
            const int hz = (int)m.vrefresh;
            if (area > best_area || (area == best_area && hz > best_hz)) {
                best = &m;
                best_area = area;
                best_hz = hz;
            }
        }
        if (best) {
            std::fprintf(stderr,
                "[дисплей] %s: монітор просить %dx%d — це понад стелю %dx%d, беру %dx%d@%d\n",
                cname, chosen->hdisplay, chosen->vdisplay,
                cfg.max_width, cfg.max_height,
                best->hdisplay, best->vdisplay, best->vrefresh);
            chosen = best;
        } else {
            // Панель узагалі не має режиму в межах стелі. Працюємо на
            // тому, що є: чорний екран гірший за дорогий.
            std::fprintf(stderr,
                "[дисплей] %s: жодного режиму в межах %dx%d, лишаю %dx%d\n",
                cname, cfg.max_width, cfg.max_height, chosen->hdisplay, chosen->vdisplay);
        }
    }

    o.mode = *chosen;

    // --- опускаємо частоту розгортки, не чіпаючи піксельний клок ---
    //
    // Робиться на ОБОХ виводах, а не лише на основному, і це навмисно:
    // додатковий може стати основним просто тому, що HDMI помер. Якби
    // його розгортку не було підготовано заздалегідь, промоція тягла б
    // за собою modeset — тобто зміну режиму й перестворення буферів
    // рівно тоді, коли пілот щойно втратив монітор.
    if (cfg.target_refresh_hz > 1.0 && o.mode.htotal && o.mode.vtotal) {
        const uint16_t was = o.mode.vtotal;
        const double line_hz = double(o.mode.clock) * 1000.0 / o.mode.htotal;
        const double before = line_hz / was;
        const long want = std::lround(line_hz / cfg.target_refresh_hz);
        const long limit = std::lround(o.mode.vtotal * (1.0 + cfg.vtotal_max_growth));

        if (want <= o.mode.vtotal) {
            std::fprintf(stderr,
                "[дисплей] %s: %.3f Гц не нижче за поточні %.3f — режим лишаю як є\n",
                cname, cfg.target_refresh_hz, before);
        } else if (want > limit) {
            std::fprintf(stderr,
                "[дисплей] %s: %.3f Гц вимагає vtotal %ld проти %u — надто далеко\n",
                cname, cfg.target_refresh_hz, want, was);
        } else {
            o.mode.vtotal = (uint16_t)want;
            const double after = line_hz / o.mode.vtotal;
            o.mode.vrefresh = (uint32_t)(after + 0.5);
            std::fprintf(stderr,
                "[дисплей] %s: vtotal %u -> %ld (+%ld рядків), частота %.3f -> %.3f Гц"
                " | рядкова %.3f кГц і клок %u кГц незмінні\n",
                cname, was, want, want - was, before, after,
                line_hz / 1000.0, o.mode.clock);
        }
    }

    // --- CRTC: той, що може живити цей коннектор і ще ВІЛЬНИЙ ---
    //
    // Зайнятість перевіряється явно: сусідній екран уже тримає свій
    // CRTC, і забрати його означало б погасити працюючий монітор.
    std::set<uint32_t> busy_crtc, busy_plane;
    for (auto& up : outs) {
        if (!up || up.get() == &o || !up->live()) continue;
        busy_crtc.insert(up->crtc_id);
        busy_plane.insert(up->plane_id);
    }

    o.crtc_id = 0;
    for (int i = 0; i < conn->count_encoders && !o.crtc_id; ++i) {
        drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc) continue;
        for (int j = 0; j < res->count_crtcs; ++j) {
            if (!(enc->possible_crtcs & (1u << j))) continue;
            if (busy_crtc.count(res->crtcs[j])) continue;
            o.crtc_id = res->crtcs[j];
            break;
        }
        drmModeFreeEncoder(enc);
    }
    if (!o.crtc_id) {
        std::fprintf(stderr, "[дисплей] %s: вільного CRTC немає\n", cname);
        return false;
    }

    // Індекс CRTC у масиві — саме він, а не id, стоїть у масках
    // possible_crtcs у плейнів.
    int crtc_index = -1;
    for (int j = 0; j < res->count_crtcs; ++j) {
        if (res->crtcs[j] == o.crtc_id) { crtc_index = j; break; }
    }

    // --- плейн: primary на цьому CRTC ---
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    std::vector<uint32_t> formats;
    o.plane_id = 0;
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes && !o.plane_id; ++i) {
            drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
            if (!p) continue;
            const bool on_our_crtc = crtc_index >= 0 && (p->possible_crtcs & (1u << crtc_index));
            if (on_our_crtc && !busy_plane.count(p->plane_id)) {
                PropTable pp = read_props(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
                auto it = pp.value.find("type");
                const bool is_primary = it != pp.value.end() && it->second == DRM_PLANE_TYPE_PRIMARY;
                if (is_primary) {
                    o.plane_id = p->plane_id;
                    for (uint32_t k = 0; k < p->count_formats; ++k) {
                        formats.push_back(p->formats[k]);
                    }
                }
            }
            drmModeFreePlane(p);
        }
        drmModeFreePlaneResources(planes);
    }
    if (!o.plane_id) {
        std::fprintf(stderr, "[дисплей] %s: не знайшовся primary-плейн на CRTC %u\n",
                     cname, o.crtc_id);
        o.crtc_id = 0;
        return false;
    }

    o.plane_props = read_props(fd, o.plane_id, DRM_MODE_OBJECT_PLANE);
    o.crtc_props  = read_props(fd, o.crtc_id,  DRM_MODE_OBJECT_CRTC);
    o.conn_props  = read_props(fd, o.connector_id, DRM_MODE_OBJECT_CONNECTOR);

    if (drmModeCreatePropertyBlob(fd, &o.mode, sizeof(o.mode), &o.mode_blob) != 0) {
        std::fprintf(stderr, "[дисплей] %s: drmModeCreatePropertyBlob: %s\n",
                     cname, std::strerror(errno));
        o.crtc_id = o.plane_id = 0;
        return false;
    }

    // --- пінимо колір ---
    // Імена властивостей різні: mainline дає стандартну "max bpc", ядра
    // Rockchip BSP — вендорні "color_depth"/"color_format".
    {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        bool any = false;

        if (o.conn_props.has("max bpc")) {
            drmModeAtomicAddProperty(req, o.connector_id, o.conn_props["max bpc"],
                                     cfg.color_depth_bits);
            any = true;
        } else if (o.conn_props.has("color_depth")) {
            const char* want = cfg.color_depth_bits >= 10 ? "30bit" : "24bit";
            uint64_t v = 0;
            if (enum_value_by_name(fd, o.conn_props["color_depth"], want, &v)) {
                drmModeAtomicAddProperty(req, o.connector_id, o.conn_props["color_depth"], v);
                any = true;
            }
        }

        if (o.conn_props.has("color_format")) {
            uint64_t v = 0;
            if (enum_value_by_name(fd, o.conn_props["color_format"],
                                   color_format_name(cfg.color_format), &v)) {
                drmModeAtomicAddProperty(req, o.connector_id, o.conn_props["color_format"], v);
                any = true;
            }
        }

        if (any) {
            if (drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) != 0) {
                std::fprintf(stderr,
                    "[дисплей] %s: не вдалося запінити колір (%s) — лишаю як є\n",
                    cname, std::strerror(errno));
            }
        }
        drmModeAtomicFree(req);
    }

    // --- метадані шару. Пишуться ПІД ЛОКОМ: state читають з інших
    //     потоків, а name і connector це std::string. ---
    {
        std::lock_guard<std::mutex> lock(mtx);
        o.state.width = o.mode.hdisplay;
        o.state.height = o.mode.vdisplay;
        // vrefresh у drmModeModeInfo — округлені герци; рахуємо точніше з
        // піксельного клока й повних розмірів, бо саме дробова частина
        // визначає дрейф відносно джерела.
        if (o.mode.htotal && o.mode.vtotal) {
            o.state.refresh_mhz =
                (int)((int64_t)o.mode.clock * 1000000LL / (o.mode.htotal * o.mode.vtotal));
        } else {
            o.state.refresh_mhz = o.mode.vrefresh * 1000;
        }
        o.state.supported_formats = formats;
        o.state.fourcc = DRM_FORMAT_XRGB8888;
        o.state.modifier = DRM_FORMAT_MOD_LINEAR;
        o.state.colorspace = ColorSpace::BT709;
        o.state.color_range = ColorRange::Full;
        o.state.color_format = cfg.color_format;
        o.state.connector = cname;

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s %dx%d@%.3f",
                      cname, o.state.width, o.state.height, o.state.refresh_hz());
        o.state.name = buf;

        // Буфери під цю геометрію. Робиться ТУТ, а не при відкритті: до
        // появи монітора невідомо ні розміру, ні формату.
        if (!create_ring(o)) {
            // Нічого не звільняємо руками: teardown_output зробить це
            // сам, а він бере цей самий лок — тому кличемо його ПІСЛЯ
            // виходу зі scope.
            o.state.generation = 0;
        } else {
            // Рендерер стежить саме за цим номером: змінився — значить
            // геометрія виводу інша, і буфери треба перестворити.
            o.state.generation = generation_.fetch_add(1, std::memory_order_release) + 1;
        }
    }

    if (!o.live()) {
        std::fprintf(stderr, "[дисплей] %s: кільце буферів не виділилось\n", cname);
        teardown_output(o);
        return false;
    }

    // РЕЖИМ СТАВИМО ОДРАЗУ, А ПЛЕЙН ГАСИМО.
    //
    // Дві причини, і обидві практичні.
    //
    // Перша: поки в цей вивід нічого не показали, він сканує те, що
    // лишив попередній власник карти — X або попередня сесія. На
    // додатковому екрані це видно найкраще: VOP лишався ACTIVE із вікном
    // на весь екран і НУЛЬОВОЮ адресою буфера, тобто сканував порожнечу.
    //
    // Друга, важливіша: опущені до 59 Гц режими не мають сенсу, поки їх
    // не застосовано. Якби modeset відкладався до першого показаного
    // кадру, то додатковий екран стояв би на своїх стокових 60 Гц — і
    // промоція його в основні (помер HDMI) означала б зміну режиму рівно
    // тоді, коли пілот щойно втратив монітор. Саме цього ми й уникали,
    // опускаючи обидва.
    //
    // CRTC вмикається з режимом, але БЕЗ плейна: екран чорний, розгортка
    // вже правильна, а перший кадр буде звичайним фліпом.
    {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        bool done = false;
        if (req) {
            drmModeAtomicAddProperty(req, o.crtc_id, o.crtc_props["MODE_ID"], o.mode_blob);
            drmModeAtomicAddProperty(req, o.crtc_id, o.crtc_props["ACTIVE"], 1);
            drmModeAtomicAddProperty(req, o.connector_id, o.conn_props["CRTC_ID"], o.crtc_id);
            drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["FB_ID"], 0);
            drmModeAtomicAddProperty(req, o.plane_id, o.plane_props["CRTC_ID"], 0);
            done = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
            drmModeAtomicFree(req);
        }

        if (done) {
            // Режим уже стоїть — першому кадру modeset не потрібен.
            o.modeset_done = true;
        } else {
            // Драйвер не прийняв активний CRTC без плейна. Не біда:
            // просто гасимо плейн, а режим поставиться першим кадром.
            std::fprintf(stderr,
                "[дисплей] %s: CRTC без плейна не прийнято (%s) —"
                " режим стане з першим кадром\n", cname, std::strerror(errno));
            drmModeAtomicReq* off = drmModeAtomicAlloc();
            if (off) {
                drmModeAtomicAddProperty(off, o.plane_id, o.plane_props["FB_ID"], 0);
                drmModeAtomicAddProperty(off, o.plane_id, o.plane_props["CRTC_ID"], 0);
                drmModeAtomicCommit(fd, off, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
                drmModeAtomicFree(off);
            }
        }
    }

    std::fprintf(stderr,
        "[дисплей] %s | crtc=%u plane=%u | %d форматів | колір: %s %d біт"
        " | бюджет кадру %.2f мс | слот %d\n",
        o.state.name.c_str(), o.crtc_id, o.plane_id, (int)formats.size(),
        color_format_name(cfg.color_format), cfg.color_depth_bits,
        o.state.frame_time_ns() / 1e6, slot);
    return true;
}

// Роль визначає ТИП коннектора: HDMI основний, коли він є; лишився один
// вивід — він основний, який би не був.
void Display::Impl::assign_roles() {
    std::lock_guard<std::mutex> lock(mtx);

    int best = -1, best_rank = 99;
    int live = 0;
    for (auto& up : outs) {
        if (!up || !up->live()) continue;
        live++;
        const int r = role_rank(up->connector_type);
        if (r < best_rank) { best_rank = r; best = up->slot_no; }
    }

    const int was = primary_slot;
    primary_slot = best;
    live_count.store(live, std::memory_order_release);

    for (auto& up : outs) {
        if (up) up->state.primary = (up->slot_no == primary_slot);
    }

    if (was != primary_slot) {
        const Output* p = (primary_slot >= 0) ? outs[(size_t)primary_slot].get() : nullptr;
        std::fprintf(stderr, "[дисплей] ОСНОВНИЙ тепер: %s (екранів живих %d)\n",
                     p ? p->state.name.c_str() : "немає", live);
    }
}

void Display::close() {
    Impl& d = *impl_;

    if (d.running.exchange(false)) {
        if (d.event_thread.joinable()) d.event_thread.join();
    }

    if (d.fd >= 0) {
        // Вимикаємо плейни явно, інакше на екранах лишиться останній
        // кадр і наступна сесія побачить "застряглі" плейни.
        for (auto& up : d.outs) {
            if (up && up->live()) d.teardown_output(*up);
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

    {
        std::lock_guard<std::mutex> lock(d.mtx);
        d.outs.clear();
        d.primary_slot = -1;
        d.live_count.store(0, std::memory_order_release);
        d.free_all_retired();
    }
    if (d.gbm) { gbm_device_destroy(d.gbm); d.gbm = nullptr; }
}

int Display::screen_count() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return (int)impl_->outs.size();
}

int Display::primary_screen() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->primary_slot;
}

OutputState Display::state(int screen) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Output* o = impl_->at(screen);
    return o ? o->state : OutputState{};
}

OutputState Display::state() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Output* o = impl_->primary();
    return o ? o->state : OutputState{};
}

PresentStats Display::stats(int screen) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Output* o = impl_->at(screen);
    PresentStats s = o ? o->st : PresentStats{};
    s.live_bufs = impl_->live_bufs;
    s.retired_bufs = (int)impl_->retired.size();
    s.generation = o ? o->state.generation : 0;
    return s;
}

PresentStats Display::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Output* o = impl_->primary();
    PresentStats s = o ? o->st : PresentStats{};
    s.live_bufs = impl_->live_bufs;
    s.retired_bufs = (int)impl_->retired.size();
    s.generation = o ? o->state.generation : 0;
    return s;
}

bool Display::acquire(int screen, Target& out) { return impl_->acquire(screen, out); }

bool Display::acquire(Target& out) {
    int p;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        p = impl_->primary_slot;
    }
    return p >= 0 && impl_->acquire(p, out);
}

bool Display::present(const Target& t) { return impl_->queue(t); }

void Display::set_flip_callback(FlipCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->on_flip_cb = std::move(cb);
}

void* Display::native_handle() const { return impl_->gbm; }

} // namespace vrx::display
