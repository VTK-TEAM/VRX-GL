// VRX_GL — приймальна станція цифрового FPV-лінка.
//
// Переписується з нуля. Дві архітектурні відмінності від VRX:
//
//   1. Вивід через ОДИН DRM-плейн. Шари (основне відео, PiP, OSD)
//      зводить GPU у спільний буфер. Причина — портованість: три
//      незалежні плейни з лінійним NV12 є лише на RK3588, а на RK3566
//      таке вікно взагалі одне. Заміряно на G610: повна композиція в
//      1080p займає ~1.1 мс при бюджеті кадру 16.67 мс, і впирається в
//      смугу пам'яті, а не в шейдери.
//
//   2. Запис — ОКРЕМИЙ пайплайн зі своїм udpsrc на тому ж порту.
//      Борт шле бродкастом, а ядро віддає копію кожної датаграми
//      кожному сокету на порту (перевірено: юнікаст натомість
//      балансується між сокетами й для цього не годиться). Тому запис
//      фізично не має шляху вплинути на тракт показу.
//
// Цілі: RK3588 (Orange Pi 5) і RK3566.

#include "display/kms_display_manager.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

bool lightdm_active() {
    return std::system("systemctl is-active --quiet lightdm") == 0;
}

// DRM master ексклюзивний: поки його тримає графічна сесія, ми не
// отримаємо ні зміни режиму, ні плейна. Тому робочий стіл треба зупинити
// ДО відкриття дисплея, а не з'ясовувати це вже по помилці.
void stop_desktop_if_running() {
    if (!lightdm_active()) return;

    std::fprintf(stderr, "[main] lightdm активний, зупиняю (тримає DRM master)\n");
    if (std::system("systemctl stop lightdm") != 0) {
        std::system("sudo -n systemctl stop lightdm >/dev/null 2>&1");
    }
    // systemctl повертає керування до того, як сесія реально відпустить
    // пристрій, тож даємо їй домерти.
    for (int i = 0; i < 20 && lightdm_active(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::fprintf(stderr, "[main] lightdm %s\n",
                 lightdm_active() ? "зупинити НЕ вдалося" : "зупинено");
}

const char* color_format_str(vrx::display::ColorFormat f) {
    using CF = vrx::display::ColorFormat;
    switch (f) {
        case CF::RGB:      return "RGB";
        case CF::YCbCr444: return "YCbCr444";
        case CF::YCbCr422: return "YCbCr422";
        case CF::YCbCr420: return "YCbCr420";
        default:           return "?";
    }
}

// ---------------------------------------------------------------------
// ТИМЧАСОВО: найпростіший виробник кадрів.
//
// Потрібен лише щоб довести, що ланцюжок працює: буфер -> dmabuf ->
// submit -> atomic commit -> підтвердження flip'а -> звільнення буфера.
// Коли з'явиться рендерер, буфери виділятиме він через GBM, а це піде.
//
// Взято dumb buffer, а не GBM: він придатний і для сканування, і для
// mmap із CPU, і не тягне за собою залежність від реалізації GBM.
// ---------------------------------------------------------------------
struct DumbBuffer {
    int drm_fd = -1;
    uint32_t handle = 0;
    uint32_t stride = 0;
    uint64_t size = 0;
    int dmabuf_fd = -1;
    uint8_t* pixels = nullptr;
    int width = 0, height = 0;

    // Маркер зайнятості. Копія лежить у Frame::keepalive, тож поки
    // дисплей тримає кадр, лічильник більший за одиницю. Це і є перевірка
    // "чи можна вже малювати в цей буфер" — рівно той контракт, який
    // описано в АПІ.
    std::shared_ptr<int> busy = std::make_shared<int>(0);

    bool free_now() const { return busy.use_count() == 1; }

    bool create(int fd, int w, int h) {
        drm_fd = fd; width = w; height = h;
        if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &handle, &stride, &size) != 0) {
            std::fprintf(stderr, "[buf] drmModeCreateDumbBuffer: %s\n", std::strerror(errno));
            return false;
        }
        uint64_t offset = 0;
        if (drmModeMapDumbBuffer(fd, handle, &offset) != 0) {
            std::fprintf(stderr, "[buf] drmModeMapDumbBuffer: %s\n", std::strerror(errno));
            return false;
        }
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)offset);
        if (p == MAP_FAILED) {
            std::fprintf(stderr, "[buf] mmap: %s\n", std::strerror(errno));
            return false;
        }
        pixels = static_cast<uint8_t*>(p);
        if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &dmabuf_fd) != 0) {
            std::fprintf(stderr, "[buf] drmPrimeHandleToFD: %s\n", std::strerror(errno));
            return false;
        }
        return true;
    }

    void fill(uint32_t xrgb) {
        for (int y = 0; y < height; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(pixels + (size_t)y * stride);
            for (int x = 0; x < width; ++x) row[x] = xrgb;
        }
    }

    void fill_rect(int x0, int y0, int w, int h, uint32_t xrgb) {
        if (x0 < 0) { w += x0; x0 = 0; }
        if (x0 + w > width) w = width - x0;
        if (w <= 0) return;
        for (int y = y0; y < y0 + h && y < height; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(pixels + (size_t)y * stride);
            for (int x = x0; x < x0 + w; ++x) row[x] = xrgb;
        }
    }

    vrx::display::Frame frame() const {
        vrx::display::Frame f;
        f.fourcc = DRM_FORMAT_XRGB8888;
        f.modifier = DRM_FORMAT_MOD_LINEAR;
        f.width = width;
        f.height = height;
        f.n_planes = 1;
        f.fd[0] = dmabuf_fd;
        f.stride[0] = stride;
        f.offset[0] = 0;
        f.keepalive = busy;     // дисплей тримає цю копію, доки показує
        return f;
    }

    void destroy() {
        if (pixels) { munmap(pixels, size); pixels = nullptr; }
        if (dmabuf_fd >= 0) { ::close(dmabuf_fd); dmabuf_fd = -1; }
        if (handle) { drmModeDestroyDumbBuffer(drm_fd, handle); handle = 0; }
    }
};

} // namespace

int main(int argc, char** argv) {
    // --colortest: три вертикальні смуги чистих R, G, B зліва направо.
    // Потрібне, щоб переконатися, що порядок каналів у XRGB8888 саме
    // такий, як ми думаємо. Помилка тут не помітна на службовій графіці,
    // але зіпсує колір усього відео, і шукати її потім довго.
    bool colortest = false;
    for (int a = 1; a < argc; ++a) {
        if (std::strcmp(argv[a], "--colortest") == 0) colortest = true;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    stop_desktop_if_running();

    vrx::display::KmsDisplayManager display;
    if (!display.open()) {
        std::fprintf(stderr, "[main] дисплей не відкрився\n");
        return 1;
    }

    const vrx::display::LayerInfo& i = display.layer().info();
    std::printf("\n=== шар виводу ===\n");
    std::printf("  %s\n", display.description().c_str());
    std::printf("  розмір     %dx%d\n", i.width, i.height);
    std::printf("  частота    %.3f Гц (бюджет кадру %.3f мс)\n",
                i.refresh_hz(), i.frame_time_ns() / 1e6);
    std::printf("  формат     0x%08x, модифікатор 0x%llx\n",
                i.fourcc, (unsigned long long)i.modifier);
    std::printf("  колір      %s\n", color_format_str(i.color_format));

    // Свій дескриптор під виділення буферів: у фінальній схемі їх так
    // само виділятиме рендерер, а дисплей лише імпортує dmabuf.
    int alloc_fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (alloc_fd < 0) {
        std::fprintf(stderr, "[main] open(card0) під буфери: %s\n", std::strerror(errno));
        display.close();
        return 1;
    }

    // Два буфери — мінімум: у той, що сканується, малювати не можна.
    std::vector<DumbBuffer> bufs(2);
    for (int k = 0; k < 2; ++k) {
        if (!bufs[k].create(alloc_fd, i.width, i.height)) {
            display.close();
            return 1;
        }
        if (colortest) {
            const int t = i.width / 3;
            bufs[k].fill(0x00000000);
            bufs[k].fill_rect(0,       0, t,             i.height, 0x00FF0000);  // мав би бути ЧЕРВОНИЙ
            bufs[k].fill_rect(t,       0, t,             i.height, 0x0000FF00);  // мав би бути ЗЕЛЕНИЙ
            bufs[k].fill_rect(2 * t,   0, i.width - 2*t, i.height, 0x000000FF);  // мав би бути СИНІЙ
        } else {
            bufs[k].fill(0x00101820);   // темно-синє тло, один раз
        }
    }
    std::printf("  буфери     2 x %dx%d, крок %u, %llu КБ кожен\n\n",
                i.width, i.height, bufs[0].stride,
                (unsigned long long)(bufs[0].size / 1024));

    std::atomic<uint64_t> flips{0};
    display.set_present_callback([&flips](int64_t) { flips.fetch_add(1); });

    // Рухома смуга. Тло залите один раз, щокадру перемальовується лише
    // вузька смужка — інакше заливка 4 МБ у некешовану пам'ять з'їдала б
    // більше часу, ніж увесь бюджет кадру.
    const int bar_w = 60;
    int bar_x = 0, dir = 6;
    int last_x[2] = {-1, -1};

    auto t0 = std::chrono::steady_clock::now();
    auto last_report = t0;
    uint64_t submitted = 0;

    std::printf("Показую рухому смугу. Ctrl+C для виходу.\n");

    while (!g_stop.load()) {
        // Шукаємо буфер, який дисплей уже відпустив.
        int idx = -1;
        for (int k = 0; k < 2; ++k) {
            if (bufs[k].free_now()) { idx = k; break; }
        }
        if (idx < 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        DumbBuffer& b = bufs[idx];
        if (!colortest) {
            if (last_x[idx] >= 0) {
                b.fill_rect(last_x[idx], 0, bar_w, i.height, 0x00101820);  // стерти старе
            }
            b.fill_rect(bar_x, 0, bar_w, i.height, 0x00E0A000);            // намалювати нове
            last_x[idx] = bar_x;
        }

        if (display.layer().submit(b.frame())) {
            submitted++;
            display.present();   // false, якщо flip ще в польоті — не біда
        }

        bar_x += dir;
        if (bar_x <= 0 || bar_x + bar_w >= i.width) dir = -dir;

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2)) {
            auto st = display.stats();
            double sec = std::chrono::duration<double>(now - t0).count();
            std::printf("  %5.1f с | показано %llu (%.1f/с) | дропнуто %llu | подано %llu\n",
                        sec, (unsigned long long)st.presented,
                        st.presented / sec,
                        (unsigned long long)st.dropped,
                        (unsigned long long)submitted);
            std::fflush(stdout);
            last_report = now;
        }
    }

    auto st = display.stats();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nПідсумок: %llu показів за %.1f с = %.2f к/с (екран %.3f Гц)\n",
                (unsigned long long)st.presented, sec, st.presented / sec, i.refresh_hz());

    for (auto& b : bufs) b.destroy();
    ::close(alloc_fd);
    display.close();
    return 0;
}
