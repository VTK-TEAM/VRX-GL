#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include "vt_telemetry_index.h"

// Той самий "сторейдж", що і telemetry_storage_cls на STM (Core/VT_TLM/
// telemetry_storage_cls.h) — масив слотів id -> (значення, час останнього
// оновлення), TELEMETRY_CAPACITY той самий (128).
//
// БЕЗ ready/far-черги: та черга на STM потрібна ЛИШЕ відправнику, щоб
// вирішувати, яке з полів взяти в НАСТУПНИЙ вихідний sync-пакет
// (build_sync_packet). VRX ніколи нічого не відправляє назад цим
// протоколом — лише приймає, тому черга тут зайва: set_value() просто
// пише в слот, get_value() просто читає. Це свідоме спрощення, а не
// недогляд.
//
// ПОТОКОБЕЗПЕКА: set_value() викликається з потоку UDP-слухача
// (VtTelemetryListener), get_value()/is_stale() — з потоку рендеру OSD.
// Обидва боки незалежні одне від одного, зв'язок лише через це сховище —
// саме так, як просив Олег: слухач і рендер — дві незалежні задачі.
class VtTelemetryStorage {
public:
    // 256, не VT_TELEMETRY_CAPACITY (128 — те, що влазить у прошивку) —
    // тут ще й VRX-локальні канали (id >= 200, vt_telemetry_index.h),
    // яких прошивка не знає. id завжди uint8_t (одна байта в кадрі), тож
    // 256 — природна верхня межа.
    static constexpr uint32_t CAPACITY = 256;

    // Викликається з потоку UDP-слухача при кожному валідному записі
    // sync-пакета. Оновлює і значення, і "час останнього оновлення" —
    // те саме, що last_update_tick_ms на STM.
    void set_value(uint8_t id, float value) {
        if (id >= CAPACITY) return;
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[id];
        slot.value = value;
        slot.last_update = std::chrono::steady_clock::now();
        slot.has_value = true;
    }

    // out_age_ms (якщо не nullptr) отримує час у мс з моменту останнього
    // set_value() для цього id — прямий аналог last_update_tick_ms/
    // "давності" на STM. Повертає false, якщо для цього id ще не було
    // жодного значення (не те саме, що VT_TELEMETRY_SOURCE_NOT_AVAILABLE —
    // те приходить як РЕАЛЬНЕ значення -9999.0, яке get_value() поверне
    // як є, це відповідальність читача розпізнати сентинел).
    bool get_value(uint8_t id, float* out_value, uint32_t* out_age_ms = nullptr) const {
        if (id >= CAPACITY) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        const Slot& slot = slots_[id];
        if (!slot.has_value) return false;
        *out_value = slot.value;
        if (out_age_ms) {
            const auto age = std::chrono::steady_clock::now() - slot.last_update;
            *out_age_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
        }
        return true;
    }

    // true, якщо для id ще не було жодного значення АБО останнє прийшло
    // давніше за timeout_ms — той самий поріг-підхід, що і is_channel_stale()
    // в старому MapOsdDataSource, просто ключ тепер raw id, а не хеш рядка.
    bool is_stale(uint8_t id, uint32_t timeout_ms) const {
        float value = 0.f;
        uint32_t age_ms = 0;
        if (!get_value(id, &value, &age_ms)) return true;
        return age_ms > timeout_ms;
    }

private:
    struct Slot {
        float value = 0.f;
        std::chrono::steady_clock::time_point last_update{};
        bool has_value = false;
    };

    mutable std::mutex mutex_;
    Slot slots_[CAPACITY];
};
