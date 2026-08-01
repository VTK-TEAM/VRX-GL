#pragma once

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
// OK=0, далі 1..30 — 1:1 з бітовими позиціями armingDisableFlags_e
// (fc/runtime_config.h) з офсетом +1, найнижчий встановлений біт виграє.
// Тут ВІДОМІ лише крайні значення — повний список 1..29 не надано,
// уточнити перед тим, як робити ENUM_SWITCH на всі причини блокування арму.
constexpr int VT_TLM_ARMING_DISABLE_REASON_OK          = 0;
constexpr int VT_TLM_ARMING_DISABLE_REASON_NO_GYRO     = 1;
constexpr int VT_TLM_ARMING_DISABLE_REASON_FAILSAFE    = 2;
constexpr int VT_TLM_ARMING_DISABLE_REASON_RX_FAILSAFE = 3;
// ... 4..29 поки не задокументовано (звір з armingDisableFlags_e при потребі) ...
constexpr int VT_TLM_ARMING_DISABLE_REASON_ARM_SWITCH  = 30;

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
