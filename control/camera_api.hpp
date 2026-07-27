#pragma once

// Клієнт HTTP-API камери. Рівно те, що треба: GET із параметром.
//
// Своя реалізація на сокеті замість libcurl — запит тривіальний
// (`GET /api/v1/set?key=value`), а тягнути залежність заради нього нема
// сенсу. Заразом повний контроль над таймаутами, що тут важливо: камера
// на тому кінці РЧ-лінка, і зависання запиту не має нікого чіпати.
//
// НЕ БЛОКУЄ нікого стороннього: усі виклики йдуть із власного потоку
// PhaseController. Прямо з потоку рендера сюди звертатися не можна.

#include <string>

namespace vrx::control {

class CameraApi {
public:
    struct Config {
        std::string host = "192.168.1.10";
        int port = 80;

        // Камера — маленький вбудований пристрій на тому кінці лінка.
        // Таймаути короткі: краще пропустити одне коригування, ніж
        // тримати потік у очікуванні.
        int connect_timeout_ms = 300;
        int io_timeout_ms = 500;
    };

    explicit CameraApi(Config cfg);

    // GET /api/v1/set?<key>=<value>. Блокуючий, до io_timeout_ms.
    // Повертає false і пише причину в лог, якщо не вдалося.
    bool set(const std::string& key, int value);

    // Статистика для діагностики: скільки запитів пішло і скільки впало.
    uint64_t requests() const { return requests_; }
    uint64_t failures() const { return failures_; }

private:
    Config cfg_;
    uint64_t requests_ = 0;
    uint64_t failures_ = 0;
};

} // namespace vrx::control
