#pragma once

// ЧИТАЧ ЛОГУ ТЕЛЕМЕТРІЇ — зворотний бік TelemetryLog.
//
// Заповнює звичайне сховище телеметрії значеннями на потрібну мить. Далі
// OSD малює з нього так само, як із живого, — з поточною розкладкою, з
// іконками, з правилами приховування. Саме тому лог і пишеться сирими
// значеннями, а не готовою картинкою.
//
// Кадр сталого розміру, тож потрібний знаходиться арифметикою: номер = час
// поділити на крок. Ні індексу, ні пошуку.

#include <cstdint>
#include <memory>
#include <string>

class VtTelemetryStorage;

namespace vrx::osd {

class TelemetryReader {
public:
    TelemetryReader();
    ~TelemetryReader();

    bool open(const std::string& path);
    void close();
    bool valid() const;

    // Настінний час першого кадру, мікросекунди.
    int64_t epoch_us() const;

    // Покласти у сховище значення на момент t_us (настінний час).
    // Повертає false, якщо такого кадру в логу немає.
    bool fill(int64_t wall_us, VtTelemetryStorage& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::osd
