#pragma once

#include "vt_telemetry_index.h"
#include <cstdio>
#include <string>

// Людяне ім'я каналу за id — лише для UI/діагностики (редактор, дебаг-
// субтитри). Рушій телеметрії цю функцію не використовує: там усе працює
// через сирі числа. Тримати в синхроні з vt_telemetry_index.h вручну.
inline const char* vt_telemetry_channel_name(int id) {
    switch (id) {
        case VT_TLM_MAH_DRAWN: return "MAH_DRAWN";
        case VT_TLM_AMPERAGE: return "AMPERAGE";
        case VT_TLM_VBAT_PRECISE: return "VBAT_PRECISE";
        case VT_TLM_GPS_DISTANCE_TO_HOME: return "GPS_DISTANCE_TO_HOME";
        case VT_TLM_G_FORCE: return "G_FORCE";
        case VT_TLM_CURRENT_PROFILE_INDEX: return "CURRENT_PROFILE_INDEX";
        case VT_TLM_GPS_FIX_TYPE: return "GPS_FIX_TYPE";
        case VT_TLM_GPS_NUM_SAT: return "GPS_NUM_SAT";
        case VT_TLM_GPS_LATITUDE: return "GPS_LATITUDE";
        case VT_TLM_GPS_LONGITUDE: return "GPS_LONGITUDE";
        case VT_TLM_GPS_ALTITUDE: return "GPS_ALTITUDE";
        case VT_TLM_GPS_SPEED: return "GPS_SPEED";
        case VT_TLM_GPS_GROUND_COURSE: return "GPS_GROUND_COURSE";
        case VT_TLM_ROLL: return "ROLL";
        case VT_TLM_PITCH: return "PITCH";
        case VT_TLM_YAW: return "YAW";
        case VT_TLM_ESTIMATED_ALTITUDE: return "ESTIMATED_ALTITUDE";
        case VT_TLM_CLIMB_RATE: return "CLIMB_RATE";
        case VT_TLM_TOTAL_TRAVELLED_DIST: return "TOTAL_TRAVELLED_DIST";
        case VT_TLM_FLIGHT_TIME: return "FLIGHT_TIME";
        case VT_TLM_THROTTLE: return "THROTTLE";
        case VT_TLM_ARM_STATUS: return "ARM_STATUS";
        case VT_TLM_DISARM_REASON: return "DISARM_REASON";
        case VT_TLM_ARMING_DISABLE_REASON: return "ARMING_DISABLE_REASON";
        case VT_TLM_FLIGHT_MODE: return "FLIGHT_MODE";
        case VT_TLM_SFP_TEMP_POINT: return "SFP_TEMP_POINT";
        case VT_TLM_SFP_VCC_POINT: return "SFP_VCC_POINT";
        case VT_TLM_SFP_BIAS_MA_POINT: return "SFP_BIAS_MA_POINT";
        case VT_TLM_SFP_TX_DBM_POINT: return "SFP_TX_DBM_POINT";
        case VT_TLM_SFP_RX_DBM_POINT: return "SFP_RX_DBM_POINT";
        case VT_TLM_SFP_TEMP_STATION: return "SFP_TEMP_STATION";
        case VT_TLM_SFP_VCC_STATION: return "SFP_VCC_STATION";
        case VT_TLM_SFP_BIAS_MA_STATION: return "SFP_BIAS_MA_STATION";
        case VT_TLM_SFP_TX_DBM_STATION: return "SFP_TX_DBM_STATION";
        case VT_TLM_SFP_RX_DBM_STATION: return "SFP_RX_DBM_STATION";
        case VT_TLM_POWER_SUPPLY_VOLTAGE_POINT: return "POWER_SUPPLY_VOLTAGE_POINT";
        case VT_TLM_POWER_SUPPLY_VOLTAGE_STATION: return "POWER_SUPPLY_VOLTAGE_STATION";
        case VT_TLM_BOSA_RX_MON_POINT: return "BOSA_RX_MON_POINT";
        case VT_TLM_BOSA_TX_MON_POINT: return "BOSA_TX_MON_POINT";
        case VT_TLM_BOSA_RX_MON_STATION: return "BOSA_RX_MON_STATION";
        case VT_TLM_BOSA_TX_MON_STATION: return "BOSA_TX_MON_STATION";
        case VT_TLM_ERLS_RSSI1: return "ERLS_RSSI1";
        case VT_TLM_ERLS_RSSI2: return "ERLS_RSSI2";
        case VT_TLM_ERLS_LQ: return "ERLS_LQ";
        case VT_TLM_ERLS_SNR: return "ERLS_SNR";
        case VT_TLM_ERLS_TX_PWR: return "ERLS_TX_PWR";
        case VT_TLM_ERLS_DL_LQ: return "ERLS_DL_LQ";
        case VT_TLM_ERLS_DL_SNR: return "ERLS_DL_SNR";
        case VT_TLM_ACTIVE_CRSF_DATA_SRC_INDEX: return "ACTIVE_CRSF_DATA_SRC_INDEX";
        case VT_TLM_RC_UPDATE_TIME_MS: return "RC_UPDATE_TIME_MS";
        case VT_TLM_ERLS_UPDATE_TIME_MS: return "ERLS_UPDATE_TIME_MS";
        case VT_TLM_TLM_QUEUE_FALLBACK_COUNT: return "TLM_QUEUE_FALLBACK_COUNT";
        case VT_TLM_TLM_QUEUE_IS_CHANGE_COUNT: return "TLM_QUEUE_IS_CHANGE_COUNT";
        case VT_TLM_TOS_RX_PERIOD_MS: return "TOS_RX_PERIOD_MS";
        case VT_TLM_LOCAL_RECORDING_STATE: return "LOCAL_RECORDING_STATE";
        case VT_TLM_LOCAL_LINE_LOSS: return "LOCAL_LINE_LOSS";
        case VT_TLM_LOCAL_H265_FPS: return "LOCAL_H265_FPS";
        case VT_TLM_LOCAL_MJPEG_FPS: return "LOCAL_MJPEG_FPS";
        case VT_TLM_LOCAL_H265_SHOWN_FPS: return "LOCAL_H265_SHOWN_FPS";
        case VT_TLM_LOCAL_DISPLAY_FPS: return "LOCAL_DISPLAY_FPS";
        case VT_TLM_LOCAL_PHASE_LOCK: return "LOCAL_PHASE_LOCK";
        case VT_TLM_LOCAL_LATENCY_MS: return "LOCAL_LATENCY_MS";
        case VT_TLM_LOCAL_DROPPED_FPS: return "LOCAL_DROPPED_FPS";
        case VT_TLM_LOCAL_LATE_FPS: return "LOCAL_LATE_FPS";
        case VT_TLM_LOCAL_LOST_FRAMES: return "LOCAL_LOST_FRAMES";
        case VT_TLM_LOCAL_CLOCK: return "LOCAL_CLOCK";
        case VT_TLM_LOCAL_DATE: return "LOCAL_DATE";
    case VT_TLM_LOCAL_DRIVE_FREE: return "LOCAL_DRIVE_FREE";

        case VT_TLM_RC_CH1: return "RC_CH1";
        case VT_TLM_RC_CH2: return "RC_CH2";
        case VT_TLM_RC_CH3: return "RC_CH3";
        case VT_TLM_RC_CH4: return "RC_CH4";
        case VT_TLM_RC_CH5: return "RC_CH5";
        case VT_TLM_RC_CH6: return "RC_CH6";
        case VT_TLM_RC_CH7: return "RC_CH7";
        case VT_TLM_RC_CH8: return "RC_CH8";
        case VT_TLM_RC_CH9: return "RC_CH9";
        case VT_TLM_RC_CH10: return "RC_CH10";
        case VT_TLM_RC_CH11: return "RC_CH11";
        case VT_TLM_RC_CH12: return "RC_CH12";
        case VT_TLM_RC_CH13: return "RC_CH13";
        case VT_TLM_RC_CH14: return "RC_CH14";
        case VT_TLM_RC_CH15: return "RC_CH15";
        case VT_TLM_RC_CH16: return "RC_CH16";
        case VT_TLM_RC_CH17: return "RC_CH17";
        case VT_TLM_RC_CH18: return "RC_CH18";

        case VT_TLM_RC2_CH1: return "RC2_CH1";
        case VT_TLM_RC2_CH2: return "RC2_CH2";
        case VT_TLM_RC2_CH3: return "RC2_CH3";
        case VT_TLM_RC2_CH4: return "RC2_CH4";
        case VT_TLM_RC2_CH5: return "RC2_CH5";
        case VT_TLM_RC2_CH6: return "RC2_CH6";
        case VT_TLM_RC2_CH7: return "RC2_CH7";
        case VT_TLM_RC2_CH8: return "RC2_CH8";
        case VT_TLM_RC2_CH9: return "RC2_CH9";
        case VT_TLM_RC2_CH10: return "RC2_CH10";
        case VT_TLM_RC2_CH11: return "RC2_CH11";
        case VT_TLM_RC2_CH12: return "RC2_CH12";
        case VT_TLM_RC2_CH13: return "RC2_CH13";
        case VT_TLM_RC2_CH14: return "RC2_CH14";
        case VT_TLM_RC2_CH15: return "RC2_CH15";
        case VT_TLM_RC2_CH16: return "RC2_CH16";
        case VT_TLM_RC2_CH17: return "RC2_CH17";
        case VT_TLM_RC2_CH18: return "RC2_CH18";

        case VT_TLM_ERLS_CH1: return "ERLS_CH1";
        case VT_TLM_ERLS_CH2: return "ERLS_CH2";
        case VT_TLM_ERLS_CH3: return "ERLS_CH3";
        case VT_TLM_ERLS_CH4: return "ERLS_CH4";
        case VT_TLM_ERLS_CH5: return "ERLS_CH5";
        case VT_TLM_ERLS_CH6: return "ERLS_CH6";
        case VT_TLM_ERLS_CH7: return "ERLS_CH7";
        case VT_TLM_ERLS_CH8: return "ERLS_CH8";
        case VT_TLM_ERLS_CH9: return "ERLS_CH9";
        case VT_TLM_ERLS_CH10: return "ERLS_CH10";
        case VT_TLM_ERLS_CH11: return "ERLS_CH11";
        case VT_TLM_ERLS_CH12: return "ERLS_CH12";
        case VT_TLM_ERLS_CH13: return "ERLS_CH13";
        case VT_TLM_ERLS_CH14: return "ERLS_CH14";
        case VT_TLM_ERLS_CH15: return "ERLS_CH15";
        case VT_TLM_ERLS_CH16: return "ERLS_CH16";
        case VT_TLM_ERLS_CH17: return "ERLS_CH17";
        case VT_TLM_ERLS_CH18: return "ERLS_CH18";

        default: return "?";
    }
}

// "12 (ROLL)" для дійсного каналу, "" якщо каналу нема (id < 0) — для UI
// (osd_editor), де поряд з номером треба показати людяне ім'я.
inline std::string vt_telemetry_channel_label(int id) {
    if (id < 0) return "";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d (%s)", id, vt_telemetry_channel_name(id));
    return buf;
}
