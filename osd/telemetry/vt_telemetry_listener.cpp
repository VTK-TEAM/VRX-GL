#include "vt_telemetry_listener.hpp"

#include "udp_listener.h"
#include "vt_telemetry_frame.h"
#include "vt_telemetry_storage.h"

#include <cstring>

VtTelemetryListener::VtTelemetryListener() = default;
VtTelemetryListener::~VtTelemetryListener() { stop(); }

bool VtTelemetryListener::start(VtTelemetryStorage& storage, uint16_t port) {
    storage_ = &storage;

    listener_ = std::make_unique<udp_listener>(static_cast<int>(port));
    listener_->set_callback([this](const std::string& raw) { on_packet(raw); });

    if (!listener_->start()) {
        listener_.reset();
        return false;
    }
    return true;
}

void VtTelemetryListener::stop() {
    if (listener_) {
        listener_->stop();
        listener_.reset();
    }
    storage_ = nullptr;
}

void VtTelemetryListener::on_packet(const std::string& raw) {
    packet_count_.fetch_add(1, std::memory_order_relaxed);

    uint8_t type = 0;
    const uint8_t* payload = nullptr;
    uint8_t payload_size = 0;
    const auto* raw_bytes = reinterpret_cast<const uint8_t*>(raw.data());
    if (!vt_telemetry::parse_frame(raw_bytes, static_cast<uint16_t>(raw.size()), &type, &payload, &payload_size)) {
        crc_fail_count_.fetch_add(1, std::memory_order_relaxed);
        return; // бите/чуже — тихо ігноруємо, той самий підхід, що й у telemetry_gui.py
    }
    if (type != vt_telemetry::kDataType) return; // хартбіт (100) шлеться на інший порт, сюди не мав би прилетіти

    const uint16_t entry_count = payload_size / vt_telemetry::kEntrySize;
    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint16_t off = static_cast<uint16_t>(i * vt_telemetry::kEntrySize);
        const uint8_t id = payload[off];
        float value = 0.f;
        std::memcpy(&value, &payload[off + 1], sizeof(value));
        storage_->set_value(id, value);
    }
}
