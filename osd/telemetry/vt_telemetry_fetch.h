#pragma once

#include "vt_telemetry_storage.h"

// Спільна логіка читання одного каналу — та сама, що потрібна і
// OsdSource (рушій VRX, OSD/osd_source.cpp) для реального рендеру на
// платі, і CanvasRenderer (превʼю в osd_editor, osd_editor/src/
// canvas_renderer.cpp) для показу в редакторі. Раніше ці два місця мали
// дві незалежні копії однієї й тієї ж логіки — легко розсинхронити
// (напр. поріг протухання чи трактування сентинела), тепер джерело
// істини одне.
//
// Повертає false, якщо: channel_id < 0 (немає каналу), даних ще не було,
// вони прострочені (age_ms > kVtChannelStaleTimeoutMs) або прошивка сама
// позначила канал недоступним (сентинел VT_TELEMETRY_SOURCE_NOT_AVAILABLE).
//
// is_age_meta=true — особливий випадок (META="AGE_S" в osd_config.json):
// значенням стає сам вік каналу в секундах, а не його вміст; сентинел і
// поріг протухання тут не застосовуються (вік — завжди валідне число).
// is_rate_meta=true (META="RATE_HZ") — завжди false: VtTelemetryStorage
// не рахує частоту апдейтів (те, що рахував старий MapOsdDataSource).
constexpr uint32_t kVtChannelStaleTimeoutMs = 3000u;

inline bool vt_fetch_channel_value(VtTelemetryStorage& storage, int channel_id,
                                    bool is_age_meta, bool is_rate_meta,
                                    float* out_value) {
    if (channel_id < 0) return false;
    const uint8_t id = static_cast<uint8_t>(channel_id);

    uint32_t age_ms = 0;
    float value = 0.f;
    if (!storage.get_value(id, &value, &age_ms)) return false;

    if (is_age_meta) {
        *out_value = static_cast<float>(age_ms) / 1000.0f;
        return true;
    }
    if (is_rate_meta) {
        return false;
    }

    if (age_ms > kVtChannelStaleTimeoutMs) return false;
    if (value <= VT_TELEMETRY_SOURCE_NOT_AVAILABLE + 0.5f) return false; // сентинел прошивки "немає даних"

    *out_value = value;
    return true;
}
