#include "license/license_gate.hpp"

#include "license/ed25519.hpp"
#include "license/license_store.hpp"
#include "license/pubkey.h"
#include "license/sd_identity.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace vrx::license {

GateResult check_license(const std::string& store_path) {
    const SdIdentity id = read_sd_identity();
    if (!id.present) return GateResult::NoCard;

    // Явну підробку відкидаємо ще до крипто: у неї немає власного serial,
    // на який можна було б видати ліцензію індивідуально.
    if (id.serial_looks_fake()) return GateResult::FakeSerial;

    uint8_t sig[64];
    if (!read_signature(store_path, sig)) return GateResult::NoSignature;

    const std::vector<uint8_t> msg = id.license_message();
    if (!ed25519_verify(msg.data(), msg.size(), sig, kVendorPublicKey)) {
        return GateResult::BadSignature;
    }
    return GateResult::Ok;
}

const char* gate_result_note(GateResult r) {
    switch (r) {
        case GateResult::Ok:           return "ok";
        case GateResult::NoCard:       return "картку не знайдено";
        case GateResult::FakeSerial:   return "serial схожий на підробку";
        case GateResult::NoSignature:  return "підпису немає у сховку";
        case GateResult::BadSignature: return "підпис недійсний для цієї картки";
    }
    return "?";
}

int print_sdinfo() {
    const SdIdentity id = read_sd_identity();
    std::printf("%s\n", id.describe().c_str());
    if (!id.present) return 2;

    // Рядок, який і потрібен активаційному скрипту: hex канонічного
    // повідомлення. Саме ЦІ байти підписує вендор приватним ключем.
    const std::vector<uint8_t> msg = id.license_message();
    std::printf("MESSAGE ");
    for (uint8_t b : msg) std::printf("%02x", b);
    std::printf("\n");
    return 0;
}

} // namespace vrx::license
