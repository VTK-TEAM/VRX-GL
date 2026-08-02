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

#include "control/layout_control.hpp"
#include "control/phase_controller.hpp"
#include "license/license_gate.hpp"
#include "record/recorder.hpp"
#include "record/storage.hpp"
#include "diag/link_monitor.hpp"
#include "diag/path_meter.hpp"
#include "display/display.hpp"
#include "osd/osd.hpp"
#include "osd/local_channels.hpp"
#include "osd/telemetry/vt_telemetry_index.h"
#include "osd/subtitle_writer.hpp"
#include "render/gl_renderer.hpp"
#include "source/h265_source.hpp"
#include "source/mjpeg_source.hpp"
#include "ui/pointer.hpp"
#include "ui/screen_ui.hpp"

#include <gst/gst.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

// Вийти, щоб наглядач запустив редактор. Окремий код виходу — це
// весь протокол між станцією і тим, хто нею керує: без сокетів,
// файлів-прапорців і сигналів.
bool g_editor = false;
constexpr int kExitRunEditor = 10;
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
    bool colortest = false;
    bool sdinfo = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-phase") phase_loop = false;
        if (a == "--colortest") colortest = true;
        if (a == "-sdinfo") sdinfo = true;
    }

    // Діагностика носія. Друкує поля картки й hex повідомлення для підпису,
    // не чіпаючи ні робочий стіл, ні екран — саме цей вивід і забирає
    // активаційний скрипт по SSH. Навмисно невиразна назва: сторонньому це
    // виглядає як звичайний дамп заліза, а не половина схеми захисту.
    if (sdinfo) {
        return vrx::license::print_sdinfo();
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
    vrx::display::Display::Config disp_cfg;
    disp_cfg.target_refresh_hz = 59.0;
    if (const char* e = std::getenv("VRX_REFRESH")) disp_cfg.target_refresh_hz = std::atof(e);
    // VRX_MODE=1920x1080 — узяти конкретний режим замість PREFERRED.
    // Потрібно не для польоту, а для перевірок, які інакше вимагають
    // іншого монітора: масштаб OSD, вартість зведення на більшій площі,
    // поведінка при зміні геометрії. Немає такого режиму — дисплей сам
    // скаже й візьме PREFERRED.
    if (const char* e = std::getenv("VRX_MODE")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            disp_cfg.want_width = w;
            disp_cfg.want_height = h;
        }
    }
    // VRX_MAX_MODE=1280x720 — посунути СТЕЛЮ роздільності. Типово
    // 1920x1080: більше коштує смуги пам'яті, а вона тут головна стаття.
    // Потрібно і для перевірки самої стелі (4K-монітора під рукою нема), і
    // на випадок слабшої плати, де навіть Full HD виявиться забагато.
    if (const char* e = std::getenv("VRX_MAX_MODE")) {
        int w = 0, h = 0;
        if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            disp_cfg.max_width = w;
            disp_cfg.max_height = h;
        }
    }
    vrx::display::Display display(disp_cfg);
    if (!display.open()) {
        std::fprintf(stderr, "[main] дисплей не відкрився\n");
        return 1;
    }

    // Монітора може ще не бути — це не помилка. Дисплей слухає uevent-и
    // й підніме вивід сам, щойно його під'єднають; рендерер побачить це
    // за номером конфігурації й перестворить буфери під нову геометрію.
    const vrx::display::OutputState i = display.state();
    std::printf("\n=== шар виводу ===\n");
    if (i.generation == 0) {
        std::printf("  монітора немає — чекаю підключення\n\n");
    } else {
        std::printf("  %s\n", i.name.c_str());
        std::printf("  розмір     %dx%d\n", i.width, i.height);
        std::printf("  частота    %.3f Гц (бюджет кадру %.3f мс)\n",
                    i.refresh_hz(), i.frame_time_ns() / 1e6);
        std::printf("  формат     0x%08x\n", i.fourcc);
        std::printf("  колір      %s\n\n", color_format_str(i.color_format));
    }

    vrx::render::GlRenderer renderer;
    vrx::render::GlRenderer::Config rend_cfg;
    rend_cfg.colortest = colortest;
    if (colortest) std::printf("РЕЖИМ ПЕРЕВІРКИ КОЛЬОРУ: смуги R/G/B зліва направо\n");
    if (!renderer.init(display, rend_cfg)) {
        std::fprintf(stderr, "[main] рендерер не піднявся\n");
        display.close();
        return 1;
    }
    if (!renderer.start()) {
        display.close();
        return 1;
    }

    // ВОРОТА ЛІЦЕНЗІЇ. Станція прив'язана до серійника мікроСД: підпис
    // Ed25519 над особистістю картки лежить у файлі-приманці, перевіряє
    // його зашитий публічний ключ. Немає дійсного підпису для ЦІЄЇ картки —
    // станція працює ЯК ЗВИЧАЙНО (запис, фаза, редактор), але з одним
    // саботажем у головному циклі: раз на 5 с вхідний порт основного відео
    // перемикається на ВИПАДКОВИЙ "лівий", кадри перестають надходити,
    // картинка застигає й через signal_timeout зникає; ще 5 с — порт назад.
    // Синхронно з "лівим" портом показуємо "SD CARD ERROR". Назовні це
    // несправний носій, а не захист: сам факт перевірки ніде не
    // проговорюється (див. license/).
    const vrx::license::GateResult gate =
        vrx::license::check_license(vrx::license::kDefaultStorePath);
    const bool licensed = (gate == vrx::license::GateResult::Ok);
    if (!licensed) {
        std::fprintf(stderr, "[main] носій: %s\n", vrx::license::gate_result_note(gate));
    }

    // ТИМЧАСОВО: два тестові джерела замість декодерів. Перевіряють
    // реєстрацію, вписування за пропорцією, якір і порядок за z. Коли
    // з'явиться декод, тут просто зміняться класи джерел.
    vrx::source::VideoSource::Config h265_cfg;
    h265_cfg.udp_port = 5600;
    auto main_src = std::make_shared<vrx::source::H265Source>("h265", h265_cfg);

    // ДРУГИЙ КАНАЛ. Окремий порт, окремий декодер, окремий запис.
    //
    // На синхронізацію НЕ впливає: розгортка одна, а кожна камера має
    // власний кварц, тож підстроюватись можна рівно під одну. Фаза
    // міряється по першому джерелу, другий живе як виходить.
    //
    // Ціна, про яку варто пам'ятати: на RK3588 апаратний декодер один на
    // всіх, і другий mppvideodec ділить із першим той самий mpp_service.
    vrx::source::VideoSource::Config pip_cfg;
    pip_cfg.udp_port = 5001;
    auto pip_src = std::make_shared<vrx::source::MjpegSource>("pip", pip_cfg);

    // ТИПОВА РОЗКЛАДКА. Діє, поки керування з телеметрії мовчить, тобто
    // одразу після ввімкнення й на станції, якій ніхто нічого не шле.
    //
    // Координата — це точка, до якої кріпиться ЯКІР САМОЇ КАРТИНКИ, а не
    // кут прямокутника: центр екрана для основного каналу, правий верхній
    // кут із відступом — для PiP.
    vrx::layout::Placement main_default;
    main_default.x = 0.5f; main_default.y = 0.5f;      // центр екрана
    main_default.w = 1.0f; main_default.h = 1.0f;      // уся площа
    main_default.anchor = vrx::layout::Anchor::Center;
    main_default.z = 0;
    main_src->set_placement(main_default);

    vrx::layout::Placement pip_default;
    pip_default.x = 0.98f; pip_default.y = 0.02f;      // правий верх, відступ 2%
    pip_default.w = 0.30f; pip_default.h = 0.30f;
    pip_default.anchor = vrx::layout::Anchor::TopRight;
    pip_default.z = 1;                                  // поверх основного
    pip_src->set_placement(pip_default);

    // ТРЕТІЙ ПОТІК — ЛОКАЛЬНИЙ ЗАХВАТ (MS2106) по V4L2.
    //
    // Плати ще немає, тож джерело ОПТ-ІН: створюється лише коли задано
    // VRX_CAPTURE_DEV (напр. /dev/video0). Без неї нічого не піднімаємо —
    // інакше v4l2src чіплявся б до першого-ліпшого /dev/video* на платі
    // (ISP, HDMI-in), не того. Коли захват припаяно — одна змінна
    // оточення вмикає весь ланцюг: показ, розкладку з телеметрії, канал
    // fps, і сценарій "останній вцілілий на весь екран".
    // ТРЕТІЙ ПОТІК — ЛОКАЛЬНИЙ ЗАХВАТ (аналог через USB).
    //
    // Пристроєм V4L2 володіє ОКРЕМА програма — релей (relay/uvc_relay.c),
    // яку піднімає наглядач. Релей жене MJPEG у LOOPBACK-мультикаст, а не
    // бродкастом: бродкаст ішов би по end1 і глушив прийом основного H.265
    // (заміряно: втрата ×30). Мультикаст із ttl=0 + iface=lo лишається на
    // платі (0 пакетів на end1, перевірено tcpdump'ом), а копію все одно
    // отримують ВСІ локальні сокети — показ, запис і його проба.
    //
    // Для станції третій потік — це майже копія другого: той самий
    // MjpegSource, лише інший порт + адреса групи. Жодного V4L2 тут.
    //
    // Вмикається за VRX_CAPTURE_DEV — тією самою змінною, за якою наглядач
    // піднімає релей. Немає її — третього каналу немає.
    constexpr int kCapPort = 5002;
    constexpr const char* kCapGroup = "239.255.77.1";

    std::shared_ptr<vrx::source::MjpegSource> cap_src;
    vrx::layout::Placement cap_default;
    cap_default.x = 0.98f; cap_default.y = 0.98f;      // правий НИЗ, відступ 2%
    cap_default.w = 0.30f; cap_default.h = 0.30f;
    cap_default.anchor = vrx::layout::Anchor::BottomRight;
    cap_default.z = 2;                                  // поверх обох мережевих
    if (const char* dev = std::getenv("VRX_CAPTURE_DEV")) {
        if (dev[0]) {
            vrx::source::VideoSource::Config cap_cfg;
            cap_cfg.udp_port = kCapPort;
            cap_cfg.multicast_addr = kCapGroup;
            cap_src = std::make_shared<vrx::source::MjpegSource>("capture", cap_cfg);
            cap_src->set_placement(cap_default);
            std::printf("Локальний захват: релей -> %s:%d (loopback)\n", kCapGroup, kCapPort);
        }
    }

    main_src->start();
    pip_src->start();
    if (cap_src) cap_src->start();
    renderer.add_source(main_src);
    renderer.add_source(pip_src);
    if (cap_src) renderer.add_source(cap_src);
    std::printf("Джерел зареєстровано: %d\n", renderer.source_count());

    // OSD. Два власні потоки всередині: приймач телеметрії й збирач
    // списку квадів. Тут — лише init, start і реєстрація; покадрово
    // ним ніхто не керує, як і джерелами.
    auto osd = std::make_shared<vrx::osd::Osd>(vrx::osd::Osd::Config{});
    if (osd->init() && osd->start()) {
        renderer.add_overlay(osd);
        std::printf("OSD піднято.\n");
    } else {
        // Немає атласу чи конфігу — працюємо без OSD, а не падаємо:
        // відео важливіше за телеметрію.
        std::fprintf(stderr, "[main] OSD не піднявся, працюю без нього\n");
        osd.reset();
    }

    // ЕКРАННЕ КЕРУВАННЯ: курсор і кнопка переходу в редактор.
    //
    // Миша читається напряму з /dev/input — X на станції немає, брати її
    // більше нізвідки. Кнопка живе окремим оверлеєм, а не в OSD: вміст
    // OSD задає користувач тим самим редактором, у який вона веде, і
    // випадково пересунути чи видалити її не має бути можливості.
    vrx::ui::Pointer pointer;
    pointer.start();
    auto screen_ui = std::make_shared<vrx::ui::ScreenUi>(vrx::ui::ScreenUi::Config{});
    screen_ui->attach(&pointer);
    renderer.add_overlay(screen_ui);

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

    // Запис. (Прапорець лишаю на випадок тесту ізоляції флешки — false
    // глушить усі рекордери й субтитри.)
    constexpr bool kRecordEnabled = true;

    vrx::record::Recorder::Config rec_cfg;
    rec_cfg.name = "main";
    rec_cfg.udp_port = h265_cfg.udp_port;
    rec_cfg.payload_type = h265_cfg.payload_type;
    vrx::record::Recorder recorder(rec_cfg, storage);
    if (kRecordEnabled) recorder.start();

    // Субтитри поруч із записаним відео: та сама телеметрія й той самий
    // osd_config.json, тож у плеєрі показання стоять там же, де стояли
    // на екрані. Власний потік — усе, що чіпає знімний носій, живе
    // окремо від показу.
    //
    // Синхронізується з рекордером ОСНОВНОГО каналу: новий файл запису
    // тягне новий .ass, зупинка запису їх закриває.
    std::unique_ptr<vrx::osd::SubtitleWriter> subs;
    std::unique_ptr<vrx::osd::LocalChannels> local_ch;
    std::unique_ptr<vrx::control::LayoutControl> layout_ctl;
    if (osd) {
        vrx::osd::SubtitleWriter::Config sub_cfg;
        // PlayRes має відповідати ЗАПИСАНОМУ відео, а не екрана: плеєр
        // масштабує субтитри під кадр. Розмір беремо в джерела, якщо
        // воно вже знає; до першого кадру лишається типовий 1080p.
        if (main_src->frame_width() > 0 && main_src->frame_height() > 0) {
            sub_cfg.video_w = main_src->frame_width();
            sub_cfg.video_h = main_src->frame_height();
        }
        // Локальні канали: те, що станція знає про себе — стан запису,
        // втрати в лінії, реальна частота обох потоків, скільки з них
        // дійшло до екрана і з якою частотою показує сам екран. Власний
        // потік: він опитує підсистеми, а не вони його штовхають.
        local_ch = std::make_unique<vrx::osd::LocalChannels>(
            vrx::osd::LocalChannels::Config{});
        local_ch->start(osd->storage(), display, renderer, phase,
                        recorder, storage, main_src, pip_src, cap_src);

        // Розкладка з телеметрії. Окремого каналу керування свідомо
        // немає: телеметрія вже прокладена, вже з CRC і вже перевірена
        // роботою, а другий канал був би другим місцем, яке ламається.
        // Типова розкладка вище лишається чинною, поки керування мовчить.
        layout_ctl = std::make_unique<vrx::control::LayoutControl>(
            vrx::control::LayoutControl::Config{});
        {
            vrx::control::LayoutControl::Bound b;
            b.source = main_src; b.name = "основний";
            b.ch_w = VT_TLM_LAYOUT_MAIN_W; b.ch_h = VT_TLM_LAYOUT_MAIN_H;
            b.ch_x = VT_TLM_LAYOUT_MAIN_X; b.ch_y = VT_TLM_LAYOUT_MAIN_Y;
            b.ch_anchor = VT_TLM_LAYOUT_MAIN_ANCHOR;
            b.fallback = main_default;
            layout_ctl->bind(std::move(b));
        }
        {
            vrx::control::LayoutControl::Bound b;
            b.source = pip_src; b.name = "PiP";
            b.ch_w = VT_TLM_LAYOUT_PIP_W; b.ch_h = VT_TLM_LAYOUT_PIP_H;
            b.ch_x = VT_TLM_LAYOUT_PIP_X; b.ch_y = VT_TLM_LAYOUT_PIP_Y;
            b.ch_anchor = VT_TLM_LAYOUT_PIP_ANCHOR;
            b.fallback = pip_default;
            layout_ctl->bind(std::move(b));
        }
        if (cap_src) {
            vrx::control::LayoutControl::Bound b;
            b.source = cap_src; b.name = "захват";
            b.ch_w = VT_TLM_LAYOUT_CAP_W; b.ch_h = VT_TLM_LAYOUT_CAP_H;
            b.ch_x = VT_TLM_LAYOUT_CAP_X; b.ch_y = VT_TLM_LAYOUT_CAP_Y;
            b.ch_anchor = VT_TLM_LAYOUT_CAP_ANCHOR;
            b.fallback = cap_default;
            layout_ctl->bind(std::move(b));
        }
        layout_ctl->start(osd->storage());

        subs = std::make_unique<vrx::osd::SubtitleWriter>(sub_cfg);
        if (kRecordEnabled && subs->init()) {
            subs->start(recorder, storage, osd->storage());
        } else if (!kRecordEnabled) {
            subs.reset();   // запис вимкнено — субтитри теж не пишемо
        } else {
            std::fprintf(stderr, "[main] субтитри не піднялись, запис іде без них\n");
            subs.reset();
        }
    }

    // Кожен канал пишеться СВОЇМ рекордером у свій файл: окремий потік,
    // окремий пайплайн, окремий udpsrc. Один канал може зникнути, а
    // другий продовжить писати, не помітивши.
    vrx::record::Recorder::Config rec2_cfg;
    rec2_cfg.name = "pip";
    rec2_cfg.codec = vrx::record::Recorder::Codec::MJPEG;
    rec2_cfg.udp_port = pip_cfg.udp_port;
    rec2_cfg.payload_type = pip_cfg.payload_type;
    vrx::record::Recorder recorder2(rec2_cfg, storage);
    if (kRecordEnabled) recorder2.start();

    // ТРЕТІЙ РЕКОРДЕР — захват. Копія другого, інший порт: читає той самий
    // бродкаст релею, що й показ (бродкаст ядро копіює ВСІМ сокетам на
    // порту — і показу, і сюди, і пробі). Незалежний: свій udpsrc, свій файл.
    std::unique_ptr<vrx::record::Recorder> recorder3;
    if (cap_src) {
        vrx::record::Recorder::Config rec3_cfg;
        rec3_cfg.name = "capture";
        rec3_cfg.codec = vrx::record::Recorder::Codec::MJPEG;
        rec3_cfg.udp_port = kCapPort;
        rec3_cfg.multicast_addr = kCapGroup;
        recorder3 = std::make_unique<vrx::record::Recorder>(rec3_cfg, storage);
        if (kRecordEnabled) recorder3->start();
    }

    // Спостерігач лінка. Розрізняє два випадки, які по кадрах виглядають
    // однаково: пакети загубились у дорозі чи камера нічого не слала.
    // Відповідь дають номери послідовності RTP, тож лише для першого
    // каналу — другий іде сирим JPEG, номерів там немає.
    vrx::diag::LinkMonitor::Config lm_cfg;
    lm_cfg.udp_port = h265_cfg.udp_port;
    lm_cfg.log_path = "/tmp/vrx_link.log";
    vrx::diag::LinkMonitor link(lm_cfg);
    link.start();

    std::printf("Працюю. Ctrl+C для виходу.\n");

    // САБОТАЖ НЕЛІЦЕНЗОВАНОЇ СТАНЦІЇ. Раз на 5 с перемикаємо вхід основного
    // відео на випадковий вільний порт — кадри застигають і через
    // signal_timeout зникають, а "SD CARD ERROR" списує це на носій; ще 5 с
    // — порт назад, напис прибираємо. Порт щоразу новий і випадковий, щоб не
    // було сталого "магічного" числа, за яким саботаж легко впізнати.
    //
    // TODO: третій потік (захват) саботаж поки НЕ чіпає. Тепер це звичайний
    // MjpegSource на порту релею — досить додати cap_src->set_udp_port у блок
    // нижче, як для main/pip. Лишив на потім.
    std::mt19937 rng(
        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    bool sab_bad = false;
    auto sab_flip = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!licensed && std::chrono::steady_clock::now() >= sab_flip) {
            sab_bad = !sab_bad;
            if (sab_bad) {
                // Обидва потоки на "ліві" порти, кожен свій випадковий.
                // РЕКОРДЕРИ теж: у них власні udpsrc на тих самих 5600/5001,
                // і без цього запис ішов би далі, поки показ застиг. Один
                // порт на показ і запис кожного каналу — щоб застигали
                // синхронно.
                const int bad_main = 20000 + (int)(rng() % 20000);  // 20000..39999
                const int bad_pip  = 40000 + (int)(rng() % 20000);  // 40000..59999
                main_src->set_udp_port(bad_main);
                pip_src->set_udp_port(bad_pip);
                recorder.set_udp_port(bad_main);
                recorder2.set_udp_port(bad_pip);
                if (osd) osd->set_notice("SD CARD ERROR  E-19");
            } else {
                main_src->set_udp_port(h265_cfg.udp_port);      // назад на робочі
                pip_src->set_udp_port(pip_cfg.udp_port);
                recorder.set_udp_port(h265_cfg.udp_port);
                recorder2.set_udp_port(pip_cfg.udp_port);
                if (osd) osd->set_notice("");
            }
            sab_flip = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }

        // ПЕРЕХІД У РЕДАКТОР. Станція виходить сама, з окремим кодом —
        // а хто саме запустить редактор і поверне станцію назад, вирішує
        // наглядач (scripts/run.sh). Тут ми не запускаємо нічого: поки
        // цей процес живий, він тримає DRM master, і редактор однаково не
        // отримав би екрана.
        if (screen_ui->take_editor_request()) {
            std::printf("Перехід у редактор розкладки.\n");
            g_editor = true;
            g_stop.store(true);
            break;
        }

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
                    " | h265 %dx%d: нових %llu, повтор %llu, дроп %llu, НЕ ПРИЙШЛО %llu"
                    " | ЗАТРИМКА %.1f мс | ФАЗА %.1f мс (дрейф %+.2f мс/с) | опит %.1f\n"
                    "          ЧАСТОТИ: камера %.4f | екран %.4f | різниця %+.0f мГц"
                    " -> дрейф має бути %+.2f мс/с\n"
                    "          інтервали: вхід %.1f/%.1f/%.1f -> вихід %.1f/%.1f/%.1f"
                    " | ДЕКОД %.1f/%.1f/%.1f мс\n",
                    sec, (unsigned long long)ds.presented, ds.presented / sec,
                    rs.avg_draw_ms,
                    main_src->frame_width(), main_src->frame_height(),
                    (unsigned long long)ms.taken, (unsigned long long)ms.reused,
                    (unsigned long long)ms.dropped, (unsigned long long)ms.missing,
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

        std::printf("          ВИВІД: конфіг #%u %dx%d@%.3f | буферів живих %d,"
                    " відкладених %d | показано %llu, витіснено %llu\n",
                    ds.generation, display.state().width, display.state().height,
                    display.state().refresh_hz(), ds.live_bufs, ds.retired_bufs,
                    (unsigned long long)ds.presented, (unsigned long long)ds.dropped);

        auto ps = phase.stats();
        if (ps.engaged) {
            std::printf("          ФАПЧ: фаза %.2f -> ціль %.2f (похибка %+.2f мс)"
                        " | опит %.2f − запас %.2f (розкид %.2f) | затримка %.1f мс"
                        " | камера %+d мГц | екран %.4f Гц | %s%s\n",
                        ps.phase_ms, ps.target_ms, ps.error_ms,
                        ps.poll_ms, ps.guard_ms, ps.jitter_ms, ps.latency_ms,
                        ps.trim_mhz, ps.display_hz,
                        ps.locked ? "ЗАХОПЛЕНО" : "ведення",
                        ps.last_write_failed ? " | КАМЕРА НЕ ВІДПОВІДАЄ" : "");
            // Три числа, з яких видно, ЧОМУ прапорець стоїть або не
            // стоїть: зміщення, його розкид і поріг, порахований із шуму
            // самого виміру фази. Без них "не захоплено" не читається.
            std::printf("               зміщення %+.2f, розкид %.2f мс проти шуму виміру"
                        " %.2f | поріг %.2f | у захопленні %llu з %llu тактів (%.0f%%)\n",
                        ps.error_smooth_ms, ps.error_spread_ms, ps.meas_noise_ms,
                        ps.lock_thr_ms, (unsigned long long)ps.lock_ticks,
                        (unsigned long long)ps.ticks,
                        ps.ticks ? 100.0 * ps.lock_ticks / ps.ticks : 0.0);
        } else {
            std::printf("          ФАПЧ: не веде (%s)\n", ps.holding);
        }

        if (osd) {
            // Що РЕАЛЬНО лежить у службових каналах. Стан запису на екрані
            // малює канал 200, і якщо він розходиться з рядком ЗАПИС вище,
            // винен не рекордер, а те, що між ними.
            auto ch = [&](uint8_t id) {
                float v = 0.f; uint32_t age = 0;
                return osd->storage().get_value(id, &v, &age)
                     ? std::string(std::to_string(v).substr(0, 6) + "/" +
                                   std::to_string(age) + "мс")
                     : std::string("немає");
            };
            std::printf("          КАНАЛИ: 200 запис %s | 206 ФАПЧ %s | 207 затримка %s"
                        " | 208 дроп %s | 209 пізно %s\n",
                        ch(200).c_str(), ch(206).c_str(), ch(207).c_str(),
                        ch(208).c_str(), ch(209).c_str());
            std::printf("               210 не прийшло кадрів: %s\n", ch(210).c_str());

            auto os = osd->stats();
            std::printf("          OSD: квадів %llu | збірок %llu по %.2f мс"
                        " | телеметрія: пакетів %llu, CRC-збоїв %llu\n",
                        (unsigned long long)os.quads, (unsigned long long)os.builds,
                        os.build_ms, (unsigned long long)os.packets,
                        (unsigned long long)os.crc_fails);
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
            auto ps2 = pip_src->stats();
            std::printf("          КАНАЛ 2 (порт %d): %dx%d | нових %llu, повтор %llu, дроп %llu\n",
                        pip_cfg.udp_port, pip_src->frame_width(), pip_src->frame_height(),
                        (unsigned long long)ps2.taken, (unsigned long long)ps2.reused,
                        (unsigned long long)ps2.dropped);
        }
        {
            auto rc = recorder.stats();
            auto dv = storage.state();
            if (rc.active) {
                std::printf("          ЗАПИС: %.1f МБ | файлів %u (ротацій %u, рестартів %u)"
                            " | носій вільно %.1f ГіБ (знімку %lld мс)"
                            " | syncfs %lld/%lld мс%s\n",
                            rc.bytes / 1e6, rc.files, rc.rotations, rc.restarts,
                            dv.free_bytes / 1073741824.0, (long long)dv.age_ms,   // 1024³
                            (long long)dv.last_sync_ms, (long long)dv.max_sync_ms,
                            storage.sync_in_progress() ? " | СКИДАЮ" : "");
            } else {
                std::printf("          ЗАПИС: не йде (%s)\n",
                            dv.usable() ? "чекаю сигнал" : "носія немає");
            }
            auto rc2 = recorder2.stats();
            if (rc2.active) {
                std::printf("          ЗАПИС 2: %.1f МБ | файлів %u\n", rc2.bytes / 1e6, rc2.files);
            }
            if (recorder3) {
                auto rc3 = recorder3->stats();
                if (rc3.active) {
                    std::printf("          ЗАПИС 3 (захват): %.1f МБ | файлів %u\n",
                                rc3.bytes / 1e6, rc3.files);
                }
            }
            // Мовчить, поки все гаразд. Ці три лічильники рахують збої,
            // які раніше не було видно взагалі: запис ставав, а назовні
            // лишалось "пишу".
            if (rc.errors || rc.stalls || rc.drive_stale ||
                rc2.errors || rc2.stalls || rc2.drive_stale) {
                std::printf("          ЗБОЇ ЗАПИСУ: шина %u/%u | без кадрів %u/%u"
                            " | знімок носія протух %u/%u\n",
                            rc.errors, rc2.errors, rc.stalls, rc2.stalls,
                            rc.drive_stale, rc2.drive_stale);
            }
        }
        {
            auto ls = link.stats();
            if (ls.packets > 0) {
                std::printf("          ЛІНК: %llu пакетів, втрачено %llu (%.3f%%)"
                            " | пауз %u: лінк %u / борт %u | найдовша %lld мс\n",
                            (unsigned long long)ls.packets, (unsigned long long)ls.lost,
                            ls.loss_percent, ls.gaps, ls.gaps_with_loss, ls.gaps_clean,
                            (long long)ls.worst_gap_ms);
            }
        }
        std::fflush(stdout);
    }

    link.stop();
    if (subs) subs->stop();
    if (local_ch) local_ch->stop();
    if (osd) osd->stop();
    recorder.stop();
    recorder2.stop();
    if (recorder3) recorder3->stop();
    storage.stop();
    phase.stop();
    renderer.stop();
    main_src->stop();
    pip_src->stop();
    if (cap_src) cap_src->stop();

    auto ds = display.stats();
    auto rs = renderer.stats();
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nПідсумок: %llu показів за %.1f с = %.2f к/с (екран %.3f Гц)\n"
                "          намальовано %llu, прохід GPU %.2f мс\n",
                (unsigned long long)ds.presented, sec, ds.presented / sec, i.refresh_hz(),
                (unsigned long long)rs.frames, rs.avg_draw_ms);

    // Наскрізний вимір тракту, якщо його зібрано (VRX_MEASURE). У
    // звичайній збірці від цього рядка не лишається нічого.
    VRX_PM_REPORT();

    display.close();
    if (g_editor) {
        std::printf("Вихід для запуску редактора (код %d).\n", kExitRunEditor);
        return kExitRunEditor;
    }
    return 0;
}
