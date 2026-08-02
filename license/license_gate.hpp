// license_gate.hpp — одна точка, що відповідає на питання "чи ліцензована
// ця станція на цій картці".
//
// Збирає докупи три частини: особистість картки (sd_identity), підпис зі
// сховку (license_store) і перевірку підпису публічним ключем (ed25519).
// Жодного секрету тут немає — лише публічний ключ і арифметика.
#pragma once

#include <string>

namespace vrx::license {

enum class GateResult {
    Ok,                // картка є, serial справжній, підпис дійсний
    NoCard,            // корінь не на мікроСД / CID недоступний
    FakeSerial,        // serial усі нулі або 0xFF — типова підробка
    NoSignature,       // сховок відсутній або закороткий
    BadSignature,      // підпис є, але не проходить перевірку
};

// Дефолтний шлях сховку. Файл-приманка лежить поруч із рештою ресурсів
// станції й виглядає як ще один бінарний ресурс.
inline constexpr const char* kDefaultStorePath = "osd_glyph_kern.bin";

// Повна перевірка. store_path — шлях до файлу-приманки.
GateResult check_license(const std::string& store_path);

// Людський опис результату — для внутрішніх логів (НЕ для екрана: на
// екран іде нейтральне "проблема з SD", щоб не підказувати, що саме
// перевіряється).
const char* gate_result_note(GateResult r);

// Режим `-sdinfo`: друкує в stdout поля картки й hex канонічного
// повідомлення, яке має підписати вендор. Повертає код виходу процесу.
int print_sdinfo();

} // namespace vrx::license
