#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

class VtTelemetryStorage;
class udp_listener;

// Незалежний прийом UDP-broadcast нового протоколу телеметрії
// (VT_ethernet_pipe/telemetry_sync_cls з прошивки UNI_DLOF_FIMWARES, порт
// 50122 — POINT і STATION шлють на нього обидва, розрізняти відправника
// не треба: id вже унікальні по ролі, напр. SFP_TEMP_POINT != SFP_TEMP_STATION).
//
// Сам прийом (сокет + робочий потік) — спільний udp_listener (include/
// udp_listener.h), той самий клас, що й для JSON-лейауту/легасі-керування.
// Тут лише розбір кадру (vt_telemetry::parse_frame — довжина+CRC8) і
// розпакування sync-запису(ів) [id][float32] у storage.
//
// На відміну від старого OsdDataClient — немає ping-pong (нікуди нічого
// не шлемо) і немає прив'язки до конкретного host/IP: приймаємо від
// будь-кого на порту.
class VtTelemetryListener {
public:
    VtTelemetryListener();
    ~VtTelemetryListener();

    bool start(VtTelemetryStorage& storage, uint16_t port = 50122);
    void stop();

    // Скільки пакетів відкинуто через провал CRC/довжини — діагностика,
    // не впливає на роботу.
    uint64_t crc_fail_count() const { return crc_fail_count_.load(std::memory_order_relaxed); }
    uint64_t packet_count() const { return packet_count_.load(std::memory_order_relaxed); }

private:
    void on_packet(const std::string& raw);

    VtTelemetryStorage* storage_ = nullptr;
    std::unique_ptr<udp_listener> listener_;

    std::atomic<uint64_t> packet_count_{0};
    std::atomic<uint64_t> crc_fail_count_{0};
};
