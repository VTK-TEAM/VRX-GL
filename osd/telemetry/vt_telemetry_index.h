#pragma once

// Заголовок користується uint8_t, тож включає його сам: досі це
// працювало лише тому, що його завжди включали після когось, хто
// вже підтягнув <cstdint>. Перший же самостійний споживач (стенд
// перевірки імен каналів) на цьому й спіткнувся.
#include <cstdint>

// Дзеркало Core/VT_TLM/telemetry_index.h з прошивки UNI_DLOF_FIMWARES
// (POINT/STATION, STM32). ТРИМАТИ РУЧНИМИ В СИНХРОНІ з прошивкою — ID тут
// мають збігатись 1:1 з тим, що шле VT_ethernet_pipe/telemetry_sync_cls,
// інакше VRX прочитає чуже поле як своє.
//
// 0..24 — дзеркало Betaflight tosTelemetryFieldId_e (1:1, той самий номер,
// TOS про VT_TLM_* нічого не знає). 25+ — власні поля VRX/прошивки.
//
// ВАЖЛИВО: після розгортання на реальні пристрої ця нумерація
// ЗАМОРОЖУЄТЬСЯ — далі можна лише ДОДАВАТИ нові id в кінець, ніколи не
// переномеровувати/перевикористовувати наявні (це wire-протокол між уже
// прошитими платами, зміна номера на льоту зламає сумісність).
typedef enum {

    VT_TLM_MAH_DRAWN                   = 0,  // Витрачений заряд, мАг
    VT_TLM_AMPERAGE                    = 1,  // Струм, А
    VT_TLM_VBAT_PRECISE                = 2,  // Напруга акума, В (нефільтрована)

    VT_TLM_GPS_DISTANCE_TO_HOME        = 3,  // Відстань до точки старту, м (GPS_distanceToHome)
    VT_TLM_G_FORCE                     = 4,  // Поточне G-навантаження (модуль вектора акселерометра), 1.0 = висіння
    VT_TLM_CURRENT_PROFILE_INDEX       = 5,  // Індекс активного PID-профілю

    VT_TLM_GPS_FIX_TYPE                = 6,  // tosTelemetryGpsFixType_e (0=NONE,2=2D,3=3D)
    VT_TLM_GPS_NUM_SAT                 = 7,  // Кількість супутників
    VT_TLM_GPS_LATITUDE                = 8,  // Широта, град
    VT_TLM_GPS_LONGITUDE               = 9,  // Довгота, град
    VT_TLM_GPS_ALTITUDE                = 10, // Висота за GPS, м
    VT_TLM_GPS_SPEED                   = 11, // Швидкість над землею, км/год
    VT_TLM_GPS_GROUND_COURSE           = 12, // Курс за GPS, град

    VT_TLM_ROLL                        = 13, // Крен, град
    VT_TLM_PITCH                       = 14, // Тангаж, град
    VT_TLM_YAW                         = 15, // Рискання, град

    VT_TLM_ESTIMATED_ALTITUDE          = 16, // Оцінена висота (баро+GPS ф'южн), м
    VT_TLM_CLIMB_RATE                  = 17, // Вертикальна швидкість, км/год

    VT_TLM_TOTAL_TRAVELLED_DIST        = 18, // Пройдена відстань з моменту старту (одометр), м

    VT_TLM_FLIGHT_TIME                 = 19, // Накопичений час у армі, с
    VT_TLM_THROTTLE                    = 20, // Газ (оброблена команда), 1000-2000
    VT_TLM_ARM_STATUS                  = 21, // Армовано/ні, 0/1

    VT_TLM_DISARM_REASON               = 22, // flightLogDisarmReason_e (fc/core.h)
    VT_TLM_ARMING_DISABLE_REASON       = 23, // tosTelemetryArmingDisableReason_e — найнижчий встановлений біт armingDisableFlags_e, +1 (0=OK)
    VT_TLM_FLIGHT_MODE                 = 24, // tosTelemetryFlightMode_e (пріоритетний вибір активного режиму)

    // ── 25..40: діагностика POINT/STATION (та сама плата, різні ролі,
    // той самий id-простір і порт 50122) ────────────────────────────────
    VT_TLM_SFP_TEMP_POINT              = 25, // Температура SFP-модуля POINT, °C
    VT_TLM_SFP_VCC_POINT               = 26, // Напруга живлення SFP POINT, В
    VT_TLM_SFP_BIAS_MA_POINT           = 27, // Струм зміщення TX SFP POINT, мА
    VT_TLM_SFP_TX_DBM_POINT            = 28, // Потужність TX SFP POINT, дБм
    VT_TLM_SFP_RX_DBM_POINT            = 29, // Потужність RX SFP POINT, дБм

    VT_TLM_SFP_TEMP_STATION            = 30, // Те саме, STATION
    VT_TLM_SFP_VCC_STATION             = 31,
    VT_TLM_SFP_BIAS_MA_STATION         = 32,
    VT_TLM_SFP_TX_DBM_STATION          = 33,
    VT_TLM_SFP_RX_DBM_STATION          = 34,

    VT_TLM_POWER_SUPPLY_VOLTAGE_POINT   = 35, // Напруга живлення плати POINT (резистивний дільник, ADC), В
    VT_TLM_POWER_SUPPLY_VOLTAGE_STATION = 36, // Те саме, STATION

    VT_TLM_BOSA_RX_MON_POINT            = 37, // Аналоговий монітор-пін оптики RX, POINT, В (пряма напруга з ADC, без дільника)
    VT_TLM_BOSA_TX_MON_POINT            = 38, // Те саме, TX, POINT
    VT_TLM_BOSA_RX_MON_STATION          = 39, // Те саме, RX, STATION
    VT_TLM_BOSA_TX_MON_STATION          = 40, // Те саме, TX, STATION

    // ── 41..50: ERLS лінк-статистика і службові ─────────────────────────
    VT_TLM_ERLS_RSSI1                  = 41, // Uplink RSSI, антена 1
    VT_TLM_ERLS_RSSI2                  = 42, // Uplink RSSI, антена 2
    VT_TLM_ERLS_LQ                     = 43, // Uplink link quality, %
    VT_TLM_ERLS_SNR                    = 44, // Uplink SNR, дБ
    VT_TLM_ERLS_TX_PWR                 = 45, // Uplink TX power (enum-код потужності)
    VT_TLM_ERLS_DL_LQ                  = 46, // Downlink link quality, %
    VT_TLM_ERLS_DL_SNR                 = 47, // Downlink SNR, дБ

    VT_TLM_ACTIVE_CRSF_DATA_SRC_INDEX  = 48, // Індекс активного джерела керування (CRSF_data_selector), -1 = немає

    VT_TLM_RC_UPDATE_TIME_MS           = 49, // Час з моменту останнього RC-пакету, мс
    VT_TLM_ERLS_UPDATE_TIME_MS         = 50, // Те саме для ERLS, мс

    // ── 51..68: дзеркало RC-каналів (з мережі/пульта), сирі 11-біт CRSF
    // (0..2047). CH17/CH18 зарезервовані — CRSF несе лише 16 реальних. ──
    VT_TLM_RC_CH1                      = 51,
    VT_TLM_RC_CH2                      = 52,
    VT_TLM_RC_CH3                      = 53,
    VT_TLM_RC_CH4                      = 54,
    VT_TLM_RC_CH5                      = 55,
    VT_TLM_RC_CH6                      = 56,
    VT_TLM_RC_CH7                      = 57,
    VT_TLM_RC_CH8                      = 58,
    VT_TLM_RC_CH9                      = 59,
    VT_TLM_RC_CH10                     = 60,
    VT_TLM_RC_CH11                     = 61,
    VT_TLM_RC_CH12                     = 62,
    VT_TLM_RC_CH13                     = 63,
    VT_TLM_RC_CH14                     = 64,
    VT_TLM_RC_CH15                     = 65,
    VT_TLM_RC_CH16                     = 66,
    VT_TLM_RC_CH17                     = 67, // поки не заповнюється
    VT_TLM_RC_CH18                     = 68, // поки не заповнюється

    // ── 69..86: дзеркало ERLS-каналів, та сама структура, що RC_CH1-18,
    // джерело — ERLS-приймач ─────────────────────────────────────────────
    VT_TLM_ERLS_CH1                    = 69,
    VT_TLM_ERLS_CH2                    = 70,
    VT_TLM_ERLS_CH3                    = 71,
    VT_TLM_ERLS_CH4                    = 72,
    VT_TLM_ERLS_CH5                    = 73,
    VT_TLM_ERLS_CH6                    = 74,
    VT_TLM_ERLS_CH7                    = 75,
    VT_TLM_ERLS_CH8                    = 76,
    VT_TLM_ERLS_CH9                    = 77,
    VT_TLM_ERLS_CH10                   = 78,
    VT_TLM_ERLS_CH11                   = 79,
    VT_TLM_ERLS_CH12                   = 80,
    VT_TLM_ERLS_CH13                   = 81,
    VT_TLM_ERLS_CH14                   = 82,
    VT_TLM_ERLS_CH15                   = 83,
    VT_TLM_ERLS_CH16                   = 84,
    VT_TLM_ERLS_CH17                   = 85,
    VT_TLM_ERLS_CH18                   = 86,

    // ── 87..89: внутрішня діагностика ───────────────────────────────────
    VT_TLM_TLM_QUEUE_FALLBACK_COUNT    = 87, // Скільки разів чергу брали через "soonest"-fallback (черга впритул до межі чи за нею)
    VT_TLM_TLM_QUEUE_IS_CHANGE_COUNT   = 88, // Скільки разів через м'якший "is_change"-fallback
    VT_TLM_TOS_RX_PERIOD_MS            = 89, // Реально виміряний період вхідних TOS-пакетів від Betaflight, мс

    // ── 90..107: ДРУГИЙ ПУЛЬТ ───────────────────────────────────────────
    //
    // Станція підтримує два передавачі одночасно. Канали дзеркалять
    // RC_CH1-18 один в один: та сама структура, той самий діапазон
    // значень, різниця лише в тому, чий стік крутять.
    //
    // Навіщо окремий блок, а не "активний пульт" одним набором. Пульти
    // можуть працювати РАЗОМ — інструктор і учень, оператор і стрілець, —
    // і в такій схемі питання "чиє це положення" важливіше за саме
    // положення. Один набір із перемикачем джерела відповісти на нього не
    // може в принципі.
    VT_TLM_RC2_CH1                     = 90,
    VT_TLM_RC2_CH2                     = 91,
    VT_TLM_RC2_CH3                     = 92,
    VT_TLM_RC2_CH4                     = 93,
    VT_TLM_RC2_CH5                     = 94,
    VT_TLM_RC2_CH6                     = 95,
    VT_TLM_RC2_CH7                     = 96,
    VT_TLM_RC2_CH8                     = 97,
    VT_TLM_RC2_CH9                     = 98,
    VT_TLM_RC2_CH10                    = 99,
    VT_TLM_RC2_CH11                    = 100,
    VT_TLM_RC2_CH12                    = 101,
    VT_TLM_RC2_CH13                    = 102,
    VT_TLM_RC2_CH14                    = 103,
    VT_TLM_RC2_CH15                    = 104,
    VT_TLM_RC2_CH16                    = 105,
    VT_TLM_RC2_CH17                    = 106,
    VT_TLM_RC2_CH18                    = 107,

} VT_telemetry_index_e;

// Той самий запас, що і TELEMETRY_CAPACITY на STM (Core/VT_TLM/telemetry_storage_cls.h).
constexpr unsigned VT_TELEMETRY_CAPACITY = 128u;

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

// ─── КЕРУВАННЯ РОЗКЛАДКОЮ (150..159) ────────────────────────────────────
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

// ─── VRX-локальні (не мережеві) канали ──────────────────────────────────
// НЕ надсилаються прошивкою і ніколи не будуть — прошивка нумерує 0..89
// (є запас до 127), тому локальні канали VRX починаються з 200, з великим
// відступом, щоб точно не перетнутись з майбутнім розширенням прошивки.
// Пишуться напряму в VtTelemetryStorage тим, хто їх рахує (main.cpp), і
// читаються звідти ж рендером OSD — жодної спеціальної обробки не треба,
// це звичайні канали з точки зору OsdSource.
constexpr uint8_t VT_TLM_LOCAL_RECORDING_STATE = 200; // 0=немає носія, 1=запис, 2=носій є/не пише
constexpr uint8_t VT_TLM_LOCAL_LINE_LOSS       = 201; // SFP_TX_DBM_POINT - SFP_RX_DBM_STATION, дБ
constexpr uint8_t VT_TLM_LOCAL_H265_FPS        = 202; // реально отриманий/декодований fps h265-потоку
constexpr uint8_t VT_TLM_LOCAL_MJPEG_FPS       = 203; // реально отриманий/декодований fps mjpeg-потоку
constexpr uint8_t VT_TLM_LOCAL_DISPLAY_FPS     = 205; // реальна частота показів на екрані (підтверджені flip'и за секунду) — на відміну від 204 не залежить від того, чи був кадр новим
constexpr uint8_t VT_TLM_LOCAL_H265_SHOWN_FPS  = 204; // реально ПОКАЗАНИЙ на екрані fps h265 (VideoSource::total_presented_frames()) — відрізняється від LOCAL_H265_FPS, якщо декодовані кадри губляться саме на показі (KmsDisplay), а не на прийомі/декодуванні

// ─── стан тракту показу ─────────────────────────────────────────────────
// Чотири числа, які разом відповідають на "чи здоровий показ і чим за це
// заплачено". Порізно кожне бреше: затримка може бути маленькою через те,
// що кадри летять повз екран, а нуль пропусків — стояти при затримці в
// два періоди.
constexpr uint8_t VT_TLM_LOCAL_PHASE_LOCK      = 206; // ФАПЧ: 0=не веде, 1=веде, 2=захоплено
constexpr uint8_t VT_TLM_LOCAL_LATENCY_MS      = 207; // затримка тракту: вихід декодера -> підтверджений показ, мс
constexpr uint8_t VT_TLM_LOCAL_DROPPED_FPS     = 208; // кадрів на секунду, що НЕ дійшли до екрана (зрізані чергою)
constexpr uint8_t VT_TLM_LOCAL_LATE_FPS        = 209; // кадрів на секунду, що прийшли після опиту: не втрачені, але поїдуть через розгортку

// КІЛЬКІСТЮ, А НЕ ЧАСТОТОЮ. Решта каналів тракту — темп подій, бо по них
// судять про стан ПРЯМО ЗАРАЗ. Цей — накопичувальний: питання до нього
// інше, "скільки всього загубилось за політ", і відповідь на нього має
// пам'ятати те, що сталося дві хвилини тому.
constexpr uint8_t VT_TLM_LOCAL_LOST_FRAMES     = 210; // кадрів, які мали прийти й не прийшли (діри в приході), за весь час
