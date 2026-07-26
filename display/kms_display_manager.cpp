#include "kms_display_manager.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <atomic>
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

} // namespace

// ---------------------------------------------------------------------

struct KmsDisplayManager::Impl : public Layer {
    explicit Impl(Config c) : cfg(std::move(c)) {}

    Config cfg;
    int fd = -1;
    bool master_taken = false;
    std::string desc = "<closed>";

    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    drmModeModeInfo mode{};
    uint32_t mode_blob = 0;

    PropTable plane_props, crtc_props, conn_props;

    LayerInfo info_{};

    // Кеш DRM-фреймбуферів. Рендерер крутить ті самі 2-3 буфери по колу,
    // тож drmModeAddFB2 робиться лише на перших кадрах. Ключ — дескриптор
    // dmabuf; це коректно, доки буфери живі, а вони живі весь час роботи
    // (ними володіє рендерер, ми лише показуємо).
    struct FbEntry {
        uint32_t fb_id = 0;
        int width = 0, height = 0;
        uint32_t fourcc = 0;
        uint64_t modifier = 0;
    };
    std::map<int, FbEntry> fb_cache;

    struct Slot {
        Frame frame;
        uint32_t fb_id = 0;
        bool valid = false;
    };
    Slot pending, in_flight, current;

    mutable std::mutex mtx;
    PresentStats st{};
    PresentCallback on_present;

    std::thread event_thread;
    std::atomic<bool> running{false};
    bool modeset_done = false;

    // --- Layer ---
    const LayerInfo& info() const override { return info_; }

    bool submit(const Frame& f) override {
        if (!f.valid()) {
            std::fprintf(stderr, "[kms] submit: некоректний кадр\n");
            return false;
        }
        if (f.width != info_.width || f.height != info_.height) {
            std::fprintf(stderr,
                "[kms] submit: розмір %dx%d не збігається з шаром %dx%d\n",
                f.width, f.height, info_.width, info_.height);
            return false;
        }
        uint32_t fb = fb_for(f);
        if (!fb) return false;

        std::lock_guard<std::mutex> lock(mtx);
        // Попередній непоказаний кадр витісняється — рахуємо як дроп,
        // щоб деградацію було видно, а не доводилось про неї здогадуватись.
        if (pending.valid) st.dropped++;
        pending.frame = f;
        pending.fb_id = fb;
        pending.valid = true;
        return true;
    }

    // --- внутрішнє ---

    uint32_t fb_for(const Frame& f) {
        auto it = fb_cache.find(f.fd[0]);
        if (it != fb_cache.end()) {
            const FbEntry& e = it->second;
            if (e.width == f.width && e.height == f.height &&
                e.fourcc == f.fourcc && e.modifier == f.modifier) {
                return e.fb_id;
            }
            // Той самий fd, але інші параметри — старий fb більше не
            // описує цей буфер.
            drmModeRmFB(fd, e.fb_id);
            fb_cache.erase(it);
        }

        uint32_t handles[4] = {};
        uint32_t strides[4] = {};
        uint32_t offsets[4] = {};
        uint64_t modifiers[4] = {};

        for (int i = 0; i < f.n_planes; ++i) {
            if (drmPrimeFDToHandle(fd, f.fd[i] >= 0 ? f.fd[i] : f.fd[0], &handles[i]) != 0) {
                std::fprintf(stderr, "[kms] drmPrimeFDToHandle(площина %d): %s\n",
                             i, std::strerror(errno));
                return 0;
            }
            strides[i] = f.stride[i];
            offsets[i] = f.offset[i];
            modifiers[i] = f.modifier;
        }

        uint32_t fb = 0;
        int ret = drmModeAddFB2WithModifiers(fd, f.width, f.height, f.fourcc,
                                              handles, strides, offsets, modifiers,
                                              &fb,
                                              f.modifier ? DRM_MODE_FB_MODIFIERS : 0);
        if (ret != 0) {
            // Не всі драйвери приймають модифікатори; для LINEAR це
            // еквівалентно і без них.
            ret = drmModeAddFB2(fd, f.width, f.height, f.fourcc,
                                handles, strides, offsets, &fb, 0);
        }
        if (ret != 0) {
            std::fprintf(stderr, "[kms] drmModeAddFB2 %dx%d fourcc=0x%08x: %s\n",
                         f.width, f.height, f.fourcc, std::strerror(errno));
            return 0;
        }

        fb_cache[f.fd[0]] = FbEntry{fb, f.width, f.height, f.fourcc, f.modifier};
        return fb;
    }

    bool commit(const Slot& s, bool allow_modeset) {
        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (!req) return false;

        const Rect vis = s.frame.visible();

        if (allow_modeset) {
            drmModeAtomicAddProperty(req, crtc_id, crtc_props["MODE_ID"], mode_blob);
            drmModeAtomicAddProperty(req, crtc_id, crtc_props["ACTIVE"], 1);
            drmModeAtomicAddProperty(req, connector_id, conn_props["CRTC_ID"], crtc_id);
        }

        drmModeAtomicAddProperty(req, plane_id, plane_props["FB_ID"], s.fb_id);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_ID"], crtc_id);

        // CRTC_* — звичайні пікселі екрана.
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_X"], 0);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_Y"], 0);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_W"], info_.width);
        drmModeAtomicAddProperty(req, plane_id, plane_props["CRTC_H"], info_.height);

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
            std::fprintf(stderr, "[kms] atomic commit%s: %s\n",
                         allow_modeset ? " (modeset)" : "", std::strerror(errno));
            return false;
        }
        return true;
    }

    // Викликається з потоку подій після ПІДТВЕРДЖЕННЯ показу.
    void on_flip(int64_t when_ns) {
        PresentCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx);
            // Кадр, що був на екрані, більше не читається — саме тут
            // падає його keepalive і буфер повертається рендереру.
            current = in_flight;
            in_flight.valid = false;
            in_flight.frame = Frame{};

            st.presented++;
            st.last_present_ns = when_ns;
            cb = on_present;
        }
        if (cb) cb(when_ns);
    }

    void event_loop() {
        drmEventContext ctx{};
        ctx.version = 2;
        ctx.page_flip_handler = [](int, unsigned, unsigned, unsigned, void* data) {
            static_cast<Impl*>(data)->on_flip(now_ns());
        };

        while (running.load(std::memory_order_relaxed)) {
            struct pollfd pfd{fd, POLLIN, 0};
            int r = poll(&pfd, 1, 200);
            if (r > 0 && (pfd.revents & POLLIN)) {
                drmHandleEvent(fd, &ctx);
            }
        }
    }
};

// ---------------------------------------------------------------------

KmsDisplayManager::KmsDisplayManager() : KmsDisplayManager(Config{}) {}

KmsDisplayManager::KmsDisplayManager(Config cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

KmsDisplayManager::~KmsDisplayManager() { close(); }

bool KmsDisplayManager::open() {
    Impl& d = *impl_;
    if (d.fd >= 0) return true;

    d.fd = ::open(d.cfg.card.c_str(), O_RDWR | O_CLOEXEC);
    if (d.fd < 0) {
        std::fprintf(stderr, "[kms] open(%s): %s\n",
                     d.cfg.card.c_str(), std::strerror(errno));
        return false;
    }

    if (d.cfg.become_master) {
        if (drmSetMaster(d.fd) != 0) {
            std::fprintf(stderr,
                "[kms] drmSetMaster: %s — DRM master уже зайнятий "
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
        std::fprintf(stderr, "[kms] драйвер не підтримує atomic/universal planes\n");
        close();
        return false;
    }

    drmModeRes* res = drmModeGetResources(d.fd);
    if (!res) {
        std::fprintf(stderr, "[kms] drmModeGetResources: %s\n", std::strerror(errno));
        close();
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
        std::fprintf(stderr, "[kms] жодного підключеного коннектора\n");
        drmModeFreeResources(res);
        close();
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
                "[kms] режим %dx%d не знайдено, беру PREFERRED\n",
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
        std::fprintf(stderr, "[kms] не знайшовся CRTC для коннектора\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close();
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
        std::fprintf(stderr, "[kms] не знайшовся primary-плейн на CRTC %u\n", d.crtc_id);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close();
        return false;
    }

    d.plane_props = read_props(d.fd, d.plane_id, DRM_MODE_OBJECT_PLANE);
    d.crtc_props  = read_props(d.fd, d.crtc_id,  DRM_MODE_OBJECT_CRTC);
    d.conn_props  = read_props(d.fd, d.connector_id, DRM_MODE_OBJECT_CONNECTOR);

    if (drmModeCreatePropertyBlob(d.fd, &d.mode, sizeof(d.mode), &d.mode_blob) != 0) {
        std::fprintf(stderr, "[kms] drmModeCreatePropertyBlob: %s\n", std::strerror(errno));
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close();
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
                    "[kms] не вдалося запінити колір (%s) — лишаю як є\n",
                    std::strerror(errno));
            }
        }
        drmModeAtomicFree(req);
    }

    // --- метадані шару ---
    d.info_.width = d.mode.hdisplay;
    d.info_.height = d.mode.vdisplay;
    // vrefresh у drmModeModeInfo — округлені герци; рахуємо точніше з
    // піксельного клока й повних розмірів, бо саме дробова частина
    // визначає дрейф відносно джерела.
    if (d.mode.htotal && d.mode.vtotal) {
        d.info_.refresh_mhz =
            (int)((int64_t)d.mode.clock * 1000000LL / (d.mode.htotal * d.mode.vtotal));
    } else {
        d.info_.refresh_mhz = d.mode.vrefresh * 1000;
    }
    d.info_.supported_formats = formats;
    d.info_.fourcc = DRM_FORMAT_XRGB8888;
    d.info_.modifier = DRM_FORMAT_MOD_LINEAR;
    d.info_.colorspace = ColorSpace::BT709;
    d.info_.color_range = ColorRange::Full;
    d.info_.color_format = d.cfg.color_format;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s %dx%d@%.3f",
                  conn->connector_type == DRM_MODE_CONNECTOR_HDMIA ? "HDMI-A" : "conn",
                  d.info_.width, d.info_.height, d.info_.refresh_hz());
    d.desc = buf;

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    d.running.store(true, std::memory_order_relaxed);
    d.event_thread = std::thread([&d] { d.event_loop(); });

    std::fprintf(stderr,
        "[kms] %s | crtc=%u plane=%u | %d форматів | колір: %s %d біт | бюджет кадру %.2f мс\n",
        d.desc.c_str(), d.crtc_id, d.plane_id, (int)formats.size(),
        color_format_name(d.cfg.color_format), d.cfg.color_depth_bits,
        d.info_.frame_time_ns() / 1e6);

    return true;
}

void KmsDisplayManager::close() {
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
        for (auto& kv : d.fb_cache) drmModeRmFB(d.fd, kv.second.fb_id);
        d.fb_cache.clear();

        if (d.mode_blob) {
            drmModeDestroyPropertyBlob(d.fd, d.mode_blob);
            d.mode_blob = 0;
        }
        if (d.master_taken) {
            drmDropMaster(d.fd);
            d.master_taken = false;
        }
        ::close(d.fd);
        d.fd = -1;
    }

    d.pending = {};
    d.in_flight = {};
    d.current = {};
    d.modeset_done = false;
    d.info_ = {};
    d.desc = "<closed>";
}

bool KmsDisplayManager::is_open() const { return impl_->fd >= 0; }

Layer& KmsDisplayManager::layer() { return *impl_; }

bool KmsDisplayManager::present() {
    Impl& d = *impl_;
    if (d.fd < 0) return false;

    Impl::Slot to_show;
    {
        std::lock_guard<std::mutex> lock(d.mtx);
        // На один CRTC може бути лише ОДИН flip у польоті. Другий commit
        // до підтвердження першого поверне EBUSY, тож просто лишаємо кадр
        // у pending — його покажемо після наступного підтвердження. Це не
        // крайовий випадок, а норма: джерело з розгорткою не синхронні.
        if (d.in_flight.valid) return false;
        if (!d.pending.valid) return false;

        to_show = d.pending;
        d.pending.valid = false;
        d.pending.frame = Frame{};
        d.in_flight = to_show;
    }

    bool need_modeset = !d.modeset_done;
    if (!d.commit(to_show, need_modeset)) {
        std::lock_guard<std::mutex> lock(d.mtx);
        d.in_flight.valid = false;
        d.in_flight.frame = Frame{};
        return false;
    }
    d.modeset_done = true;
    return true;
}

void KmsDisplayManager::set_present_callback(PresentCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->on_present = std::move(cb);
}

PresentStats KmsDisplayManager::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->st;
}

const std::string& KmsDisplayManager::description() const { return impl_->desc; }

} // namespace vrx::display
