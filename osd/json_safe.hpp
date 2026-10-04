#pragma once

// Безпечне читання поля JSON, що прийшло ЗЗОВНІ (камера, файл з носія).
//
// nlohmann::json::value(key, def) повертає def лише коли ключа НЕМАЄ. Якщо
// ключ є, але іншого типу ("ok": "true" замість true, "error": 404 замість
// рядка), вона кидає type_error. У потоці, де ніхто не ловить, це
// std::terminate — тобто падіння всієї станції через одне поле у відповіді
// пристрою, якого ми не контролюємо.
//
// Ловилось у полі: після оновлення станція зі СТАРОЮ камерою падала по колу
// одразу після старту (екран вмикався й тух), бо петля фази розбирала її
// відповідь новим контрактом. На стенді без камери відтворення не було —
// розбирати не було чого.
//
// Тут: ключа нема, він null або тип не той — повертаємо def і йдемо далі.
// Чисельні типи між собою конвертуються, як і в value() (7.0 -> int 7).

#include "json.hpp"

template <class T>
T json_get(const nlohmann::json& o, const char* key, T def) {
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end() || it->is_null()) return def;
    try {
        return it->template get<T>();
    } catch (const nlohmann::json::exception&) {
        return def;
    }
}
