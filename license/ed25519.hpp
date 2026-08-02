// ed25519.hpp — перевірка підпису Ed25519 через OpenSSL.
//
// Станції потрібне ЛИШЕ verify. Підпис (sign) робить вендор своїм
// приватним ключем на своїй машині — тут його немає й бути не може.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vrx::license {

// true, якщо sig (64 байти) — дійсний підпис msg під публічним ключем
// pub (32 байти). Ніколи не кидає; будь-яка помилка OpenSSL -> false.
bool ed25519_verify(const uint8_t* msg, size_t msg_len,
                    const uint8_t* sig, const uint8_t* pub);

} // namespace vrx::license
