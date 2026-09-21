#pragma once

// Заголовок користується uint8_t, тож включає його сам: досі це
// працювало лише тому, що його завжди включали після когось, хто
// вже підтягнув <cstdint>. Перший же самостійний споживач (стенд
// перевірки імен каналів) на цьому й спіткнувся.
#include <cstdint>

// Дзеркало fw_telemetry.h із прошивки борта VTK-TEAM FIBER_WIND.
// ТРИМАТИ РУЧНИМИ В СИНХРОНІ з прошивкою — id тут мають збігатись 1:1 з
// тим, що борт кладе на дріт, інакше станція прочитає чуже поле як своє.
//
// РОЗКЛАДКА v2 (2026-09-17), без сумісності з v1. Номери розкладені
// БЛОКАМИ ПО 64 за змістом, усередині блоку суцільно, у кінці кожного —
// запас. Сенс саме в запасі: нове поле лягає у свій блок і НІКОЛИ не
// зсуває сусідів, тож нумерацію більше не доводиться заморожувати.
//
//    0…63   політ            192…319  канали пультів, по 32 на джерело
//   64…95   GPS              320…351  оптика борта
//   96…127  живлення борта   384…447  станція (борт лише резервує місце)
//  128…159  ESC і мотори     448…511  версії протоколів і збірок
//  160…191  керування, лінки 512+     простір станції, борт його не чіпає
//
// Версію самої розкладки борт шле каналом VT_TLM_PROTO_TELEMETRY_VER.
// Він може сказати, що id перемістились, але не що змінився формат запису:
// якби змінився, не прочиталось би нічого, включно з самим номером.
typedef enum {
    // ── 0…63 ПОЛІТ ───────────────────────────────────────────────────────
    VT_TLM_ROLL                    = 0,   // Крен, °
    VT_TLM_PITCH                   = 1,   // Тангаж, °
    VT_TLM_YAW                     = 2,   // Рискання, °
    VT_TLM_THROTTLE                = 3,   // Газ, мкс (rcCommand)
    VT_TLM_EST_ALTITUDE            = 4,   // Оцінена висота (баро+GPS), м
    VT_TLM_CLIMB_RATE              = 5,   // Вертикальна швидкість, км/год
    VT_TLM_G_FORCE                 = 6,   // Навантаження, g (1.0 = висіння)
    VT_TLM_ARM_STATUS              = 7,   // 0/1
    VT_TLM_DISARM_REASON           = 8,   // flightLogDisarmReason_e, NA до першого роззброєння
    VT_TLM_ARMING_DISABLE_REASON   = 9,   // молодший біт armingDisableFlags_e + 1, 0 = дозволено
    VT_TLM_FLIGHT_MODE             = 10,  // 0 acro, 1 angle, 2 horizon
    VT_TLM_CURRENT_PROFILE         = 11,  // індекс PID-профілю
    VT_TLM_FLIGHT_TIME_S           = 12,  // с у армі, накопичено
    VT_TLM_UPTIME_S                = 13,  // с від старту
    VT_TLM_TRAVELLED_DIST_M        = 14,  // м (GPS)
    VT_TLM_GYRO_TEMP               = 15,  // °C
    VT_TLM_BARO_TEMP               = 16,  // °C
    VT_TLM_CORE_TEMP               = 17,  // °C, кристал MCU
    VT_TLM_CPU_LOAD                = 18,  // %
    VT_TLM_GYRO_CALIBRATED         = 19,  // 0/1
    VT_TLM_ACC_CALIBRATED          = 20,  // 0/1

    // ── 64…95 GPS ────────────────────────────────────────────────────────
    VT_TLM_GPS_FIX_TYPE            = 64,  // 0 / 2 / 3
    VT_TLM_GPS_NUM_SAT             = 65,
    VT_TLM_GPS_LAT                 = 66,  // °
    VT_TLM_GPS_LON                 = 67,  // °
    VT_TLM_GPS_ALT                 = 68,  // м
    VT_TLM_GPS_SPEED               = 69,  // км/год
    VT_TLM_GPS_COURSE              = 70,  // °
    VT_TLM_GPS_DIST_TO_HOME        = 71,  // м
    VT_TLM_GPS_HDOP                = 72,

    // ── 96…127 ЖИВЛЕННЯ БОРТА ────────────────────────────────────────────
    VT_TLM_VBAT                    = 96,  // В
    VT_TLM_AMPERAGE                = 97,  // А
    VT_TLM_MAH_DRAWN               = 98,  // мА·год
    VT_TLM_ESC_POWER_ON            = 99,  // 0/1, стан рейки ESC
    VT_TLM_CAMERA_POWER_ON         = 100, // 0/1, стан рейки камери

    // ── 128…159 ESC І МОТОРИ, по 4 (id + номер мотора 0…3) ───────────────
    VT_TLM_ESC_TEMP_0              = 128, // °C, розширена DShot-телеметрія
    VT_TLM_ESC_VOLTAGE_0           = 132, // В
    VT_TLM_ESC_CURRENT_0           = 136, // А
    VT_TLM_ESC_RPM_0               = 140, // об/хв
    VT_TLM_MOTOR_OUT_0             = 144, // 0…1, вихід міксера

    // ── 160…191 КЕРУВАННЯ І ЛІНКИ ────────────────────────────────────────
    VT_TLM_ACTIVE_RC_SOURCE        = 160, // 0 Ethernet, 1 резерв ELRS
    VT_TLM_CONTROL_DELAY_MS        = 161, // наскрізна затримка керування
    VT_TLM_RC1_AGE_MS              = 162,
    VT_TLM_RC2_AGE_MS              = 163,
    VT_TLM_RC3_AGE_MS              = 164,
    VT_TLM_ERLS_AGE_MS             = 165,
    VT_TLM_HANDOVER_COUNT          = 166, // скільки разів мінялось джерело
    VT_TLM_FAILSAFE_ACTIVE         = 167, // 0/1
    VT_TLM_RC_FRAMES_ETH           = 168,
    VT_TLM_RC_FRAMES_ERLS          = 169,
    VT_TLM_ERLS_RSSI1              = 176, // дБм (від'ємне)
    VT_TLM_ERLS_RSSI2              = 177, // дБм
    VT_TLM_ERLS_LQ                 = 178, // %
    VT_TLM_ERLS_SNR                = 179, // дБ
    VT_TLM_ERLS_TX_PWR             = 180, // мВт (індекс CRSF)
    VT_TLM_ERLS_DL_LQ              = 181, // %
    VT_TLM_ERLS_DL_SNR             = 182, // дБ

    // ── 192…319 КАНАЛИ, по 32 слоти на джерело ───────────────────────────
    // Слот 16 у кожному наборі — вік даних пульта, слот 17 — прапори.
    VT_TLM_RC1_CH1                 = 192, // Ethernet, пульт 1 … CH32 = 223
    VT_TLM_RC2_CH1                 = 224, // поки не збирається
    VT_TLM_RC3_CH1                 = 256, // поки не збирається
    VT_TLM_ERLS_CH1                = 288, // резерв, CRSF

    // ── 320…351 ОПТИКА БОРТА ─────────────────────────────────────────────
    VT_TLM_SFP_TEMP                = 320, // °C
    VT_TLM_SFP_VCC                 = 321, // В
    VT_TLM_SFP_TX_DBM              = 322, // дБм, ВІДНОСНІ (без калібрування)
    VT_TLM_SFP_RX_DBM              = 323, // дБм, відносні

    // ── 352…383 ПРИСТРІЙ ІНІЦІАЦІЇ ─────────────────────────────
    //
    // Борт дає на пристрій сервоімпульс (команду) і читає його статус —
    // 16-символьний рядок по UART. Рядок на землю не йде: борт зводить його
    // до коду.
    //
    // КОМАНДА Й СТАТУС — РІЗНІ ПЕРЕЛІКИ З РІЗНИМИ ЗНАЧЕННЯМИ, і це навмисно.
    // Команда має три позиції, статус — те, що пристрій показує, і
    // взаємно-однозначної відповідності між ними немає: на ARM і на ACTIVATE
    // пристрій однаково відповідає ARM.
    VT_TLM_INIT_DEV_COMMAND        = 352, // що борт ВИДАЄ на пристрій
    VT_TLM_INIT_DEV_STATUS         = 353, // що пристрій КАЖЕ про себе
    VT_TLM_INIT_DEV_TIMER_S        = 354, // таймер із рядка статусу, секунди

    // ── 384…447 СТАНЦІЯ (борт лише резервує, значення кладе станція) ─────
    VT_TLM_STATION_SUPPLY_V        = 384, // В
    VT_TLM_STATION_BUILD           = 385, // лічильник збірки станції
    VT_TLM_STATION_SFP_PRESENT     = 386, // 0/1
    VT_TLM_STATION_SFP_TEMP        = 387, // °C
    VT_TLM_STATION_SFP_VCC         = 388, // В
    VT_TLM_STATION_SFP_TX_DBM      = 389, // дБм
    VT_TLM_STATION_SFP_RX_DBM      = 390, // дБм

    // ── 448…511 ВЕРСІЇ ───────────────────────────────────────────────────
    VT_TLM_BUILD_POINT             = 448, // наскрізний лічильник збірок борта
    VT_TLM_BF_VERSION              = 449, // рік × 100 + місяць
    VT_TLM_PROTO_TRANSPORT_VER     = 450,
    VT_TLM_PROTO_CONTROL_VER       = 451,
    VT_TLM_PROTO_TELEMETRY_VER     = 452, // версія ЦІЄЇ розкладки
    VT_TLM_PROTO_MSP_VER           = 453,
    VT_TLM_PROTO_HEARTBEAT_VER     = 454,
} VT_telemetry_index_e;

// Той самий запас, що і TELEMETRY_CAPACITY на STM (Core/VT_TLM/telemetry_storage_cls.h).
constexpr unsigned VT_TELEMETRY_CAPACITY = 512u;

// Той самий сентинел, що і TELEMETRY_SOURCE_NOT_AVAILABLE на STM — прошивка
// сама підставляє це замість значення, якщо канал не оновлювався довше
// TELEMETRY_STALE_TIMEOUT_MS (2000мс) ще ДО відправки. VRX-сторона окремо
// рахує власну "давність" по факту прийому (див. vt_telemetry_storage.h) —
// це два незалежні захисти, обидва варто враховувати при читанні значення.
constexpr float VT_TELEMETRY_SOURCE_NOT_AVAILABLE = -9999.0f;

// ─── tosTelemetryFlightMode_e (VT_TLM_FLIGHT_MODE, id 24) ───────────────
// Повний список — можна безпечно використовувати в ENUM_SWITCH-елементах
// osd_config.json (CASES з OP:"EQ" на ці значення).
constexpr int VT_TLM_FLIGHT_MODE_ACRO       = 0;
constexpr int VT_TLM_FLIGHT_MODE_ANGLE      = 1;
constexpr int VT_TLM_FLIGHT_MODE_HORIZON    = 2;
constexpr int VT_TLM_FLIGHT_MODE_HEADFREE   = 3;
constexpr int VT_TLM_FLIGHT_MODE_PASSTHRU   = 4;
constexpr int VT_TLM_FLIGHT_MODE_GPS_RESCUE = 5;
constexpr int VT_TLM_FLIGHT_MODE_FAILSAFE   = 6;

// ─── tosTelemetryGpsFixType_e (VT_TLM_GPS_FIX_TYPE, id 6) ───────────────
// INIT_DEV_COMMAND — що борт ВИДАЄ на пристрій ініціації. У дужках —
// ширина сервоімпульсу, яку борт при цьому формує.
constexpr int VT_TLM_INIT_DEV_COMMAND_DISARM   = 0;  // 1000 мкс
constexpr int VT_TLM_INIT_DEV_COMMAND_ARM      = 1;  // 1500 мкс
constexpr int VT_TLM_INIT_DEV_COMMAND_ACTIVATE = 2;  // 2000 мкс

// INIT_DEV_STATUS — що пристрій КАЖЕ про себе.
//
// Словник неповний ЗА ЗАДУМОМ: пристрій не документований, напевно відомі
// лише DIS і ARM. Нові значення додаються в кінець, наявні не
// перенумеровуються — інакше старий запис прочитається як інший стан.
constexpr int VT_TLM_INIT_DEV_STATUS_NO_SIGNAL = 0;  // тиша понад 2 с або від старту
constexpr int VT_TLM_INIT_DEV_STATUS_DISARM    = 1;
constexpr int VT_TLM_INIT_DEV_STATUS_ARM       = 2;
constexpr int VT_TLM_INIT_DEV_STATUS_UNKNOWN   = 3;  // рядок є, слово невідоме

constexpr int VT_TLM_GPS_FIX_TYPE_NONE = 0;
constexpr int VT_TLM_GPS_FIX_TYPE_2D   = 2;
constexpr int VT_TLM_GPS_FIX_TYPE_3D   = 3;

// ─── tosTelemetryArmingDisableReason_e (VT_TLM_ARMING_DISABLE_REASON, id 23) ─
//
// ПОВНИЙ перелік, 1:1 з прошивкою. Раніше тут були лише крайні значення й
// чесна помітка "4..29 не задокументовано" — тому ENUM_SWITCH на причини
// блокування арму зробити було не можна: пілот бачив би число замість
// причини.
//
// Прошивка бере armingDisableFlags_e (30-бітова маска, причин може бути
// кілька одночасно) і згортає її до ОДНІЄЇ — найнижчого встановленого
// біта, бо прапорці там перелічені за критичністю. Значення тут — це
// позиція біта плюс один; нуль означає, що арму ніщо не заважає.
constexpr int VT_TLM_ARMING_DISABLE_REASON_OK                 =  0;
constexpr int VT_TLM_ARMING_DISABLE_REASON_NO_GYRO            =  1;
constexpr int VT_TLM_ARMING_DISABLE_REASON_FAILSAFE           =  2;
constexpr int VT_TLM_ARMING_DISABLE_REASON_RX_FAILSAFE        =  3;
constexpr int VT_TLM_ARMING_DISABLE_REASON_NOT_DISARMED       =  4;
constexpr int VT_TLM_ARMING_DISABLE_REASON_BOXFAILSAFE        =  5;
constexpr int VT_TLM_ARMING_DISABLE_REASON_RUNAWAY_TAKEOFF    =  6;
constexpr int VT_TLM_ARMING_DISABLE_REASON_CRASH_DETECTED     =  7;
constexpr int VT_TLM_ARMING_DISABLE_REASON_THROTTLE           =  8;
constexpr int VT_TLM_ARMING_DISABLE_REASON_ANGLE              =  9;
constexpr int VT_TLM_ARMING_DISABLE_REASON_BOOT_GRACE_TIME    = 10;
constexpr int VT_TLM_ARMING_DISABLE_REASON_NOPREARM           = 11;
constexpr int VT_TLM_ARMING_DISABLE_REASON_LOAD               = 12;
constexpr int VT_TLM_ARMING_DISABLE_REASON_CALIBRATING        = 13;
constexpr int VT_TLM_ARMING_DISABLE_REASON_CLI                = 14;
constexpr int VT_TLM_ARMING_DISABLE_REASON_CMS_MENU           = 15;
constexpr int VT_TLM_ARMING_DISABLE_REASON_BST                = 16;
constexpr int VT_TLM_ARMING_DISABLE_REASON_MSP                = 17;
constexpr int VT_TLM_ARMING_DISABLE_REASON_PARALYZE           = 18;
constexpr int VT_TLM_ARMING_DISABLE_REASON_GPS                = 19;
constexpr int VT_TLM_ARMING_DISABLE_REASON_RESC               = 20;
constexpr int VT_TLM_ARMING_DISABLE_REASON_DSHOT_TELEM        = 21;
constexpr int VT_TLM_ARMING_DISABLE_REASON_REBOOT_REQUIRED    = 22;
constexpr int VT_TLM_ARMING_DISABLE_REASON_DSHOT_BITBANG      = 23;
constexpr int VT_TLM_ARMING_DISABLE_REASON_ACC_CALIBRATION    = 24;
constexpr int VT_TLM_ARMING_DISABLE_REASON_MOTOR_PROTOCOL     = 25;
constexpr int VT_TLM_ARMING_DISABLE_REASON_CRASHFLIP          = 26;
constexpr int VT_TLM_ARMING_DISABLE_REASON_ALTHOLD            = 27;
constexpr int VT_TLM_ARMING_DISABLE_REASON_POSHOLD            = 28;
constexpr int VT_TLM_ARMING_DISABLE_REASON_AUTOPILOT          = 29;
constexpr int VT_TLM_ARMING_DISABLE_REASON_ARM_SWITCH         = 30;

// ─── КЕРУВАННЯ РОЗКЛАДКОЮ (150..164) ────────────────────────────────────
//
// Єдиний блок каналів, який station СЛУХАЄ як команду, а не як показання.
// Окремого протоколу для цього свідомо немає: телеметрія вже прокладена,
// вже стабільна й уже має CRC — другий канал керування був би другим
// місцем, де щось ламається, і другим, що треба тримати живим у польоті.
//
// Числа тут лежать у проміжку між прошивкою (0..127) і локальними
// каналами станції (200+), тобто не чіпають ні тих, ні тих.
//
// ЩО ОЗНАЧАЮТЬ ЗНАЧЕННЯ:
//   W, H  — коробка, у яку вписується кадр: максимум, який йому дозволено
//           зайняти. Частки екрана 0..1. Менше kMinSize (0.05) підтягується
//           до неї — нуль зробив би картинку невидимою без ознак поломки.
//   X, Y  — точка на екрані, куди кріпиться ЯКІР. Частки від верхнього
//           лівого кута. За межі екрана виходити дозволено: показати
//           частину картинки — законний намір.
//   ANCHOR — яка точка САМОЇ КАРТИНКИ стає в (X,Y), 0..8 рядками:
//           0 лівий верх    1 середина верху   2 правий верх
//           3 середина ліва 4 центр            5 середина права
//           6 лівий низ     7 середина низу    8 правий низ
//
// НЕМАЄ ДАНИХ = канал не приходив ЖОДНОГО РАЗУ; тоді діє вкомпільоване
// значення. Протухання (телеметрія замовкла) значенням НЕ вважається:
// інакше кожен пробій лінка на три секунди перекидав би картинку на
// типову розкладку, а це гірше за будь-яку неправильну.
constexpr uint8_t VT_TLM_LAYOUT_MAIN_W      = 150;
constexpr uint8_t VT_TLM_LAYOUT_MAIN_H      = 151;
constexpr uint8_t VT_TLM_LAYOUT_MAIN_X      = 152;
constexpr uint8_t VT_TLM_LAYOUT_MAIN_Y      = 153;
constexpr uint8_t VT_TLM_LAYOUT_MAIN_ANCHOR = 154;
constexpr uint8_t VT_TLM_LAYOUT_PIP_W       = 155;
constexpr uint8_t VT_TLM_LAYOUT_PIP_H       = 156;
constexpr uint8_t VT_TLM_LAYOUT_PIP_X       = 157;
constexpr uint8_t VT_TLM_LAYOUT_PIP_Y       = 158;
constexpr uint8_t VT_TLM_LAYOUT_PIP_ANCHOR  = 159;
// Третій потік — локальний захват із плати відеозахвату (MS2106),
// V4L2-джерело, не мережеве. Керується так само, як два вищі.
constexpr uint8_t VT_TLM_LAYOUT_CAP_W       = 160;
constexpr uint8_t VT_TLM_LAYOUT_CAP_H       = 161;
constexpr uint8_t VT_TLM_LAYOUT_CAP_X       = 162;
constexpr uint8_t VT_TLM_LAYOUT_CAP_Y       = 163;
constexpr uint8_t VT_TLM_LAYOUT_CAP_ANCHOR  = 164;

// ─── VRX-локальні (не мережеві) канали ──────────────────────────────────
// НЕ надсилаються прошивкою і ніколи не будуть — прошивка нумерує 0..89
// (є запас до 127), тому локальні канали VRX починаються з 200, з великим
// відступом, щоб точно не перетнутись з майбутнім розширенням прошивки.
// Пишуться напряму в VtTelemetryStorage тим, хто їх рахує (main.cpp), і
// читаються звідти ж рендером OSD — жодної спеціальної обробки не треба,
// це звичайні канали з точки зору OsdSource.
constexpr uint16_t VT_TLM_LOCAL_RECORDING_STATE = 528; // 0=немає носія, 1=запис, 2=носій є/не пише
constexpr uint16_t VT_TLM_LOCAL_LINE_LOSS       = 529; // SFP_TX_DBM_POINT - SFP_RX_DBM_STATION, дБ
constexpr uint16_t VT_TLM_LOCAL_H265_FPS        = 530; // реально отриманий/декодований fps h265-потоку
constexpr uint16_t VT_TLM_LOCAL_MJPEG_FPS       = 531; // реально отриманий/декодований fps mjpeg-потоку
constexpr uint16_t VT_TLM_LOCAL_DISPLAY_FPS     = 533; // реальна частота показів на екрані (підтверджені flip'и за секунду) — на відміну від 204 не залежить від того, чи був кадр новим
constexpr uint16_t VT_TLM_LOCAL_H265_SHOWN_FPS  = 532; // реально ПОКАЗАНИЙ на екрані fps h265 (VideoSource::total_presented_frames()) — відрізняється від LOCAL_H265_FPS, якщо декодовані кадри губляться саме на показі (KmsDisplay), а не на прийомі/декодуванні

// ─── стан тракту показу ─────────────────────────────────────────────────
// Чотири числа, які разом відповідають на "чи здоровий показ і чим за це
// заплачено". Порізно кожне бреше: затримка може бути маленькою через те,
// що кадри летять повз екран, а нуль пропусків — стояти при затримці в
// два періоди.
constexpr uint16_t VT_TLM_LOCAL_PHASE_LOCK      = 534; // ФАПЧ: 0=не веде, 1=веде, 2=захоплено
constexpr uint16_t VT_TLM_LOCAL_LATENCY_MS      = 535; // затримка тракту: вихід декодера -> підтверджений показ, мс
constexpr uint16_t VT_TLM_LOCAL_DROPPED_FPS     = 536; // кадрів на секунду, що НЕ дійшли до екрана (зрізані чергою)
constexpr uint16_t VT_TLM_LOCAL_LATE_FPS        = 537; // кадрів на секунду, що прийшли після опиту: не втрачені, але поїдуть через розгортку

// КІЛЬКІСТЮ, А НЕ ЧАСТОТОЮ. Решта каналів тракту — темп подій, бо по них
// судять про стан ПРЯМО ЗАРАЗ. Цей — накопичувальний: питання до нього
// інше, "скільки всього загубилось за політ", і відповідь на нього має
// пам'ятати те, що сталося дві хвилини тому.
constexpr uint16_t VT_TLM_LOCAL_LOST_FRAMES     = 538; // кадрів, які мали прийти й не прийшли (діри в приході), за весь час

// ─── СИСТЕМНИЙ ГОДИННИК СТАНЦІЇ ─────────────────────────────────────────
//
// Інтернету в полі немає, синхронізуватись нема з чим, а час у записі й
// на екрані потрібен: за ним потім розбирають політ.
//
// ЧОМУ ДАТА ЦЕ YYMMDD, А НЕ YYYYMMDD. Канал передається float32, у якого
// точних цілих лише до 16 777 216. 20260802 туди не влазить і зіпсувалось
// би тихо, зсунувши день. 260802 влазить із запасом.
//
// Час — секунди від опівночі: те саме міркування, тільки простіше, і
// заразом дає безкоштовне сортування.
constexpr uint16_t VT_TLM_LOCAL_CLOCK           = 539; // секунд від опівночі, локальний час
constexpr uint16_t VT_TLM_LOCAL_DATE            = 540; // дата як YYMMDD (260802 = 2 серпня 2026)
constexpr uint16_t VT_TLM_LOCAL_DRIVE_FREE      = 541; // вільно на носії, ГБ
constexpr uint16_t VT_TLM_LOCAL_CAPTURE_FPS     = 542; // fps локального захвату (MS2106), реально декодований
