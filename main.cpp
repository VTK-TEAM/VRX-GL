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
//
// main НІЧИМ не керує покадрово: кожна підсистема має власний потік і
// власний темп. Тут лише ініціалізація, запуск і зупинка.

#include "display/kms_display_manager.hpp"
#include "render/gl_renderer.hpp"
#include "source/test_source.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

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

} // namespace

int main() {
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
    std::printf("  формат     0x%08x\n", i.fourcc);
    std::printf("  колір      %s\n\n", color_format_str(i.color_format));

    vrx::render::GlRenderer renderer;
    if (!renderer.init(display)) {
        std::fprintf(stderr, "[main] рендерер не піднявся\n");
        display.close();
        return 1;
    }
    if (!renderer.start()) {
        display.close();
        return 1;
    }

    // ТИМЧАСОВО: два тестові джерела замість декодерів. Перевіряють
    // реєстрацію, вписування за пропорцією, якір і порядок за z. Коли
    // з'явиться декод, тут просто зміняться класи джерел.
    auto main_src = std::make_shared<vrx::source::TestSource>("main", 1920, 1080);
    auto pip_src  = std::make_shared<vrx::source::TestSource>("pip", 640, 480);

    {
        vrx::layout::Placement p;          // на весь екран
        p.z = 0;
        main_src->set_placement(p);
    }
    {
        vrx::layout::Placement p;          // у правий верхній кут
        p.x = 0.02f; p.y = 0.02f; p.w = 0.30f; p.h = 0.30f;
        p.anchor = vrx::layout::Anchor::TopRight;
        p.x = 1.0f - 0.30f - 0.02f;
        p.z = 1;                            // поверх основного
        pip_src->set_placement(p);
    }

    main_src->start();
    pip_src->start();
    renderer.add_source(main_src);
    renderer.add_source(pip_src);
    std::printf("Джерел зареєстровано: %d\n", renderer.source_count());

    std::printf("Працюю. Ctrl+C для виходу.\n");

    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(2)) continue;
        last = now;

        auto ds = display.stats();
        auto rs = renderer.stats();
        double sec = std::chrono::duration<double>(now - t0).count();
        std::printf("  %5.1f с | показано %llu (%.1f/с) | дропнуто %llu"
                    " | GPU %.2f мс | простоїв %llu\n",
                    sec, (unsigned long long)ds.presented, ds.presented / sec,
                    (unsigned long long)ds.dropped, rs.avg_draw_ms,
                    (unsigned long long)rs.stalls);
        std::fflush(stdout);
    }

    renderer.stop();
    main_src->stop();
    pip_src->stop();

    auto ds = display.stats();
    auto rs = renderer.stats();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nПідсумок: %llu показів за %.1f с = %.2f к/с (екран %.3f Гц)\n"
                "          намальовано %llu, прохід GPU %.2f мс\n",
                (unsigned long long)ds.presented, sec, ds.presented / sec, i.refresh_hz(),
                (unsigned long long)rs.frames, rs.avg_draw_ms);

    display.close();
    return 0;
}
