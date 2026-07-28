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

#include "control/phase_controller.hpp"
#include "record/recorder.hpp"
#include "record/storage.hpp"
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
#include <string>
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

    // Петлю можна вимкнути. Потрібно не для польоту, а для замірів: поки
    // вона працює, вона перезаписує підстроювання камери двічі на
    // секунду, і будь-яке значення, виставлене ззовні, живе пів секунди.
    // Без цього прапорця не можна ні зміряти власну частоту камери, ні
    // перевірити, що підстроювання взагалі діє.
    bool phase_loop = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-phase") phase_loop = false;
    }

    stop_desktop_if_running();

    // Опускаємо розгортку до 59 Гц — рядками гасіння, не клоком.
    //
    // Це не підстроювання під камеру, а вибір РОБОЧОЇ ТОЧКИ. Матриця
    // вміє рухатись лише вниз від своєї апаратної стелі 60 Гц, тож
    // рівновага петлі мусить лежати нижче неї. На 59 запас виходить ~1
    // Гц угору й 2 вниз незалежно від того, який монітор підключили;
    // на стоковій панелі 59.789 його лишалось би 210 мГц, і будь-який
    // рівно 60-герцовий монітор зробив би захоплення неможливим.
    //
    // Задається один раз при відкритті, до появи картинки — блимання
    // немає, бо це і є первинний modeset.
    // VRX_REFRESH перекриває ціль на час діагностики: 0 = лишити рідну
    // частоту панелі. Що нижча ціль, то більший зсув доводиться тримати
    // камері, а разом із ним і ризик, що вона почне дублювати кадри.
    vrx::display::KmsDisplayManager::Config disp_cfg;
    disp_cfg.target_refresh_hz = 59.0;
    if (const char* e = std::getenv("VRX_REFRESH")) disp_cfg.target_refresh_hz = std::atof(e);
    vrx::display::KmsDisplayManager display(disp_cfg);
    if (!display.open()) {
        std::fprintf(stderr, "[main] дисплей не відкрився\n");
        return 1;
    }

    // Монітора може ще не бути — це не помилка. Дисплей слухає uevent-и
    // й підніме вивід сам, щойно його під'єднають; рендерер побачить це
    // за номером конфігурації й перестворить буфери під нову геометрію.
    const vrx::display::LayerInfo& i = display.layer().info();
    std::printf("\n=== шар виводу ===\n");
    if (i.generation == 0) {
        std::printf("  монітора немає — чекаю підключення\n\n");
    } else {
        std::printf("  %s\n", display.description().c_str());
        std::printf("  розмір     %dx%d\n", i.width, i.height);
        std::printf("  частота    %.3f Гц (бюджет кадру %.3f мс)\n",
                    i.refresh_hz(), i.frame_time_ns() / 1e6);
        std::printf("  формат     0x%08x\n", i.fourcc);
        std::printf("  колір      %s\n\n", color_format_str(i.color_format));
    }

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

    // Фазове автопідстроювання. Веде ЧАСТОТУ камери так, щоб прихід
    // кадру стояв трохи раніше за опит рендерера — настільки раніше,
    // наскільки дістає виміряний джитер, і не більше. Обидві величини
    // рахуються на ходу, тому тут немає жодної цифри. Єдиний актуатор у
    // тракті: режим екрана не чіпаємо взагалі.
    vrx::control::PhaseController::Config ph_cfg;
    ph_cfg.camera.host = "192.168.1.10";
    vrx::control::PhaseController phase(display, renderer, main_src, ph_cfg);
    if (phase_loop) {
        phase.start();
    } else {
        std::printf("Петля фази ВИМКНЕНА (--no-phase): камеру не чіпаємо.\n");
    }

    // Запис. Носій веде окремий клас у власних потоках, рекордер —
    // ще один потік зі своїм пайплайном. Спільного з показом немає
    // нічого, тож зрив носія до картинки не дістає.
    vrx::record::Storage storage{vrx::record::Storage::Config{}};
    storage.start();

    vrx::record::Recorder::Config rec_cfg;
    rec_cfg.name = "main";
    rec_cfg.udp_port = h265_cfg.udp_port;
    rec_cfg.payload_type = h265_cfg.payload_type;
    vrx::record::Recorder recorder(rec_cfg, storage);
    recorder.start();

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
                    " | ЗАТРИМКА %.1f мс | ФАЗА %.1f мс (дрейф %+.2f мс/с) | опит %.1f\n"
                    "          ЧАСТОТИ: камера %.4f | екран %.4f | різниця %+.0f мГц"
                    " -> дрейф має бути %+.2f мс/с\n"
                    "          інтервали: вхід %.1f/%.1f/%.1f -> вихід %.1f/%.1f/%.1f"
                    " | ДЕКОД %.1f/%.1f/%.1f мс\n",
                    sec, (unsigned long long)ds.presented, ds.presented / sec,
                    rs.avg_draw_ms,
                    main_src->frame_width(), main_src->frame_height(),
                    (unsigned long long)ms.taken, (unsigned long long)ms.reused,
                    (unsigned long long)ms.dropped,
                    rs.latency_avg_ms, rs.phase_ms, rs.phase_drift_ms_per_s,
                    rs.poll_offset_ms,
                    ms.produced_hz, ds.measured_hz,
                    (ms.produced_hz - ds.measured_hz) * 1000.0,
                    ds.measured_hz > 0
                        ? -(ms.produced_hz - ds.measured_hz) * (1000.0 / ds.measured_hz)
                        : 0.0,
                    in_mn, in_avg, in_mx,
                    ms.interval_min_ms, ms.interval_avg_ms, ms.interval_max_ms,
                    dc_mn, dc_avg, dc_mx);

        auto ps = phase.stats();
        if (ps.engaged) {
            std::printf("          ФАПЧ: фаза %.2f -> ціль %.2f (похибка %+.2f мс)"
                        " | опит %.2f − запас %.2f (розкид %.2f) | затримка %.1f мс"
                        " | камера %+d мГц | екран %.4f Гц | %s%s\n",
                        ps.phase_ms, ps.target_ms, ps.error_ms,
                        ps.poll_ms, ps.guard_ms, ps.jitter_ms, ps.latency_ms,
                        ps.trim_mhz, ps.display_hz,
                        ps.locked ? "ЗАХОПЛЕНО" : "ведення",
                        ps.failed ? " | ЗАПИСИ ПАДАЮТЬ" : "");
        } else {
            std::printf("          ФАПЧ: не веде (%s)\n", ps.holding);
        }

        // Крок ЗЙОМКИ між показаними кадрами — те, що визначає плавність
        // руху. Лічильники повторів мовчать, поки ми беремо по кадру на
        // розгортку, навіть якщо зняті вони були врозбіг.
        const uint64_t st_all = rs.step_ok + rs.step_repeat + rs.step_short + rs.step_gap;
        if (st_all > 0) {
            std::printf("          КРОК ЗЙОМКИ: норма %.2f%% | повтор %llu | коротко %llu"
                        " | пропуск %llu | розмах %.1f..%.1f мс\n",
                        100.0 * rs.step_ok / st_all,
                        (unsigned long long)rs.step_repeat,
                        (unsigned long long)rs.step_short,
                        (unsigned long long)rs.step_gap,
                        rs.step_min_ms, rs.step_max_ms);
        }
        {
            auto rc = recorder.stats();
            auto dv = storage.state();
            if (rc.active) {
                std::printf("          ЗАПИС: %.1f МБ | файлів %u (ротацій %u, рестартів %u)"
                            " | носій вільно %.1f ГБ%s\n",
                            rc.bytes / 1e6, rc.files, rc.rotations, rc.restarts,
                            dv.free_bytes / 1e9,
                            storage.sync_in_progress() ? " | СКИДАЮ КЕШІ" : "");
            } else {
                std::printf("          ЗАПИС: не йде (%s)\n",
                            dv.usable() ? "чекаю сигнал" : "носія немає");
            }
        }
        std::fflush(stdout);
    }

    recorder.stop();
    storage.stop();
    phase.stop();
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
