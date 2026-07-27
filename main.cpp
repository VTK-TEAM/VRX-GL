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
#include "source/h265_source.hpp"
#include "source/test_source.hpp"

#include <gst/gst.h>

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

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    stop_desktop_if_running();

    // Підганяємо розгортку під частоту камери — без цього фаза приходу
    // кадру пливе, і раз на ~4.7 с кадр неминуче дублюється або гине.
    vrx::display::KmsDisplayManager::Config disp_cfg;
    disp_cfg.genlock_hz = 60.0;
    vrx::display::KmsDisplayManager display(disp_cfg);
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
    vrx::source::H265Source::Config h265_cfg;
    h265_cfg.udp_port = 5600;
    auto main_src = std::make_shared<vrx::source::H265Source>("h265", h265_cfg);

    // Другим поки лишається тестове джерело: другого декодера ще немає,
    // а перевіряти розкладку на чомусь треба.
    auto pip_src = std::make_shared<vrx::source::TestSource>("pip", 640, 480);

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

        auto ms = main_src->stats();
        double in_mn = 0, in_avg = 0, in_mx = 0;
        main_src->input_intervals(&in_mn, &in_avg, &in_mx);
        double dc_mn = 0, dc_avg = 0, dc_mx = 0;
        main_src->decode_latency(&dc_mn, &dc_avg, &dc_mx);

        std::printf("  %5.1f с | показано %llu (%.1f/с) | GPU %.2f мс"
                    " | h265 %dx%d: нових %llu, повтор %llu, дроп %llu"
                    " | ЗАТРИМКА %.1f мс | ФАЗА %.1f мс (дрейф %+.2f мс/с) | опит %.1f | екран %.3f Гц\n"
                    "          інтервали: вхід %.1f/%.1f/%.1f -> вихід %.1f/%.1f/%.1f"
                    " | ДЕКОД %.1f/%.1f/%.1f мс\n",
                    sec, (unsigned long long)ds.presented, ds.presented / sec,
                    rs.avg_draw_ms,
                    main_src->frame_width(), main_src->frame_height(),
                    (unsigned long long)ms.taken, (unsigned long long)ms.reused,
                    (unsigned long long)ms.dropped,
                    rs.latency_avg_ms, rs.phase_ms, rs.phase_drift_ms_per_s,
                    rs.poll_offset_ms, ds.measured_hz,
                    in_mn, in_avg, in_mx,
                    ms.interval_min_ms, ms.interval_avg_ms, ms.interval_max_ms,
                    dc_mn, dc_avg, dc_mx);
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
