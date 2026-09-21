#pragma once

// Імена каналів телеметрії — для логів, стендів і редактора.
//
// ФАЙЛ ЗГЕНЕРОВАНО з vt_telemetry_index.h. Писати руками не треба: таблиця
// імен, яка розходиться з переліком id, гірша за її відсутність — вона
// впевнено називає чуже поле своїм.

#include "vt_telemetry_index.h"

#include <cstdio>
#include <string>

inline const char* vt_telemetry_channel_name(int id) {
    switch (id) {
        case VT_TLM_ROLL: return "ROLL";
        case VT_TLM_PITCH: return "PITCH";
        case VT_TLM_YAW: return "YAW";
        case VT_TLM_THROTTLE: return "THROTTLE";
        case VT_TLM_EST_ALTITUDE: return "EST_ALTITUDE";
        case VT_TLM_CLIMB_RATE: return "CLIMB_RATE";
        case VT_TLM_G_FORCE: return "G_FORCE";
        case VT_TLM_ARM_STATUS: return "ARM_STATUS";
        case VT_TLM_DISARM_REASON: return "DISARM_REASON";
        case VT_TLM_ARMING_DISABLE_REASON: return "ARMING_DISABLE_REASON";
        case VT_TLM_FLIGHT_MODE: return "FLIGHT_MODE";
        case VT_TLM_CURRENT_PROFILE: return "CURRENT_PROFILE";
        case VT_TLM_FLIGHT_TIME_S: return "FLIGHT_TIME_S";
        case VT_TLM_UPTIME_S: return "UPTIME_S";
        case VT_TLM_TRAVELLED_DIST_M: return "TRAVELLED_DIST_M";
        case VT_TLM_GYRO_TEMP: return "GYRO_TEMP";
        case VT_TLM_BARO_TEMP: return "BARO_TEMP";
        case VT_TLM_CORE_TEMP: return "CORE_TEMP";
        case VT_TLM_CPU_LOAD: return "CPU_LOAD";
        case VT_TLM_GYRO_CALIBRATED: return "GYRO_CALIBRATED";
        case VT_TLM_ACC_CALIBRATED: return "ACC_CALIBRATED";
        case VT_TLM_GPS_FIX_TYPE: return "GPS_FIX_TYPE";
        case VT_TLM_GPS_NUM_SAT: return "GPS_NUM_SAT";
        case VT_TLM_GPS_LAT: return "GPS_LAT";
        case VT_TLM_GPS_LON: return "GPS_LON";
        case VT_TLM_GPS_ALT: return "GPS_ALT";
        case VT_TLM_GPS_SPEED: return "GPS_SPEED";
        case VT_TLM_GPS_COURSE: return "GPS_COURSE";
        case VT_TLM_GPS_DIST_TO_HOME: return "GPS_DIST_TO_HOME";
        case VT_TLM_GPS_HDOP: return "GPS_HDOP";
        case VT_TLM_VBAT: return "VBAT";
        case VT_TLM_AMPERAGE: return "AMPERAGE";
        case VT_TLM_MAH_DRAWN: return "MAH_DRAWN";
        case VT_TLM_ESC_POWER_ON: return "ESC_POWER_ON";
        case VT_TLM_CAMERA_POWER_ON: return "CAMERA_POWER_ON";
        case VT_TLM_ESC_TEMP_0: return "ESC_TEMP_0";
        case VT_TLM_ESC_VOLTAGE_0: return "ESC_VOLTAGE_0";
        case VT_TLM_ESC_CURRENT_0: return "ESC_CURRENT_0";
        case VT_TLM_ESC_RPM_0: return "ESC_RPM_0";
        case VT_TLM_MOTOR_OUT_0: return "MOTOR_OUT_0";
        case VT_TLM_ACTIVE_RC_SOURCE: return "ACTIVE_RC_SOURCE";
        case VT_TLM_CONTROL_DELAY_MS: return "CONTROL_DELAY_MS";
        case VT_TLM_RC1_AGE_MS: return "RC1_AGE_MS";
        case VT_TLM_RC2_AGE_MS: return "RC2_AGE_MS";
        case VT_TLM_RC3_AGE_MS: return "RC3_AGE_MS";
        case VT_TLM_ERLS_AGE_MS: return "ERLS_AGE_MS";
        case VT_TLM_HANDOVER_COUNT: return "HANDOVER_COUNT";
        case VT_TLM_FAILSAFE_ACTIVE: return "FAILSAFE_ACTIVE";
        case VT_TLM_RC_FRAMES_ETH: return "RC_FRAMES_ETH";
        case VT_TLM_RC_FRAMES_ERLS: return "RC_FRAMES_ERLS";
        case VT_TLM_ERLS_RSSI1: return "ERLS_RSSI1";
        case VT_TLM_ERLS_RSSI2: return "ERLS_RSSI2";
        case VT_TLM_ERLS_LQ: return "ERLS_LQ";
        case VT_TLM_ERLS_SNR: return "ERLS_SNR";
        case VT_TLM_ERLS_TX_PWR: return "ERLS_TX_PWR";
        case VT_TLM_ERLS_DL_LQ: return "ERLS_DL_LQ";
        case VT_TLM_ERLS_DL_SNR: return "ERLS_DL_SNR";
        case VT_TLM_RC1_CH1: return "RC1_CH1";
        case VT_TLM_RC2_CH1: return "RC2_CH1";
        case VT_TLM_RC3_CH1: return "RC3_CH1";
        case VT_TLM_ERLS_CH1: return "ERLS_CH1";
        case VT_TLM_SFP_TEMP: return "SFP_TEMP";
        case VT_TLM_SFP_VCC: return "SFP_VCC";
        case VT_TLM_SFP_TX_DBM: return "SFP_TX_DBM";
        case VT_TLM_SFP_RX_DBM: return "SFP_RX_DBM";
        case VT_TLM_INIT_DEV_COMMAND: return "INIT_DEV_COMMAND";
        case VT_TLM_INIT_DEV_STATUS: return "INIT_DEV_STATUS";
        case VT_TLM_INIT_DEV_TIMER_S: return "INIT_DEV_TIMER_S";
        case VT_TLM_STATION_SUPPLY_V: return "STATION_SUPPLY_V";
        case VT_TLM_STATION_BUILD: return "STATION_BUILD";
        case VT_TLM_STATION_SFP_PRESENT: return "STATION_SFP_PRESENT";
        case VT_TLM_STATION_SFP_TEMP: return "STATION_SFP_TEMP";
        case VT_TLM_STATION_SFP_VCC: return "STATION_SFP_VCC";
        case VT_TLM_STATION_SFP_TX_DBM: return "STATION_SFP_TX_DBM";
        case VT_TLM_STATION_SFP_RX_DBM: return "STATION_SFP_RX_DBM";
        case VT_TLM_BUILD_POINT: return "BUILD_POINT";
        case VT_TLM_BF_VERSION: return "BF_VERSION";
        case VT_TLM_PROTO_TRANSPORT_VER: return "PROTO_TRANSPORT_VER";
        case VT_TLM_PROTO_CONTROL_VER: return "PROTO_CONTROL_VER";
        case VT_TLM_PROTO_TELEMETRY_VER: return "PROTO_TELEMETRY_VER";
        case VT_TLM_PROTO_MSP_VER: return "PROTO_MSP_VER";
        case VT_TLM_PROTO_HEARTBEAT_VER: return "PROTO_HEARTBEAT_VER";
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
        case VT_TLM_LOCAL_CAPTURE_FPS: return "LOCAL_CAPTURE_FPS";
        default: break;
    }

    // Канали пультів — блоками по 32 на джерело. Перелічувати всі сто
    // двадцять вісім іменами нема сенсу: номер усередині блоку і є іменем.
    if (id >= VT_TLM_RC1_CH1  && id < VT_TLM_RC1_CH1  + 32) return "RC1_CH";
    if (id >= VT_TLM_RC2_CH1  && id < VT_TLM_RC2_CH1  + 32) return "RC2_CH";
    if (id >= VT_TLM_RC3_CH1  && id < VT_TLM_RC3_CH1  + 32) return "RC3_CH";
    if (id >= VT_TLM_ERLS_CH1 && id < VT_TLM_ERLS_CH1 + 32) return "ERLS_CH";

    // ESC і мотори — по чотири на поле, номер мотора в молодших двох.
    if (id >= VT_TLM_ESC_TEMP_0    && id < VT_TLM_ESC_TEMP_0    + 4) return "ESC_TEMP";
    if (id >= VT_TLM_ESC_VOLTAGE_0 && id < VT_TLM_ESC_VOLTAGE_0 + 4) return "ESC_VOLTAGE";
    if (id >= VT_TLM_ESC_CURRENT_0 && id < VT_TLM_ESC_CURRENT_0 + 4) return "ESC_CURRENT";
    if (id >= VT_TLM_ESC_RPM_0     && id < VT_TLM_ESC_RPM_0     + 4) return "ESC_RPM";
    if (id >= VT_TLM_MOTOR_OUT_0   && id < VT_TLM_MOTOR_OUT_0   + 4) return "MOTOR_OUT";

    return "UNKNOWN";
}

inline std::string vt_telemetry_channel_label(int id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d (%s)", id, vt_telemetry_channel_name(id));
    return buf;
}
