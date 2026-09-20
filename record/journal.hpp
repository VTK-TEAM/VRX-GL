#pragma once

// ЖУРНАЛ СЕАНСУ — спільне для всіх, хто в нього пише.
//
// Файл один на вмикання станції: session_<мітка>.jsonl у теці дня. Пишуть
// у нього рекордери (по рядку на відкриття, звірку й закриття файлу) і
// приймач чорної скриньки. Читають — плеєр і майбутня програма на ПК.
//
// Тут лише те, що МУСИТЬ бути однаковим у всіх: формат часу й ім'я файлу.
// Спосіб запису кожен обирає сам — рекордер тримає дескриптор відкритим на
// весь файл (він пише часто), скринька відкриває на два рядки (вона пише
// двічі за лог).

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace vrx::record {

// Час рівно в тому ж вигляді, що й мітка Matroska DateUTC: UTC,
// мікросекунди. Формат однаковий навмисно — щоб журнал і заголовок самого
// файлу можна було звірити очима, без перерахунків.
inline std::string iso_utc(int64_t us) {
    const time_t sec = (time_t)(us / 1000000);
    struct tm tm {};
    gmtime_r(&sec, &tm);
    char buf[64];
    const size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::snprintf(buf + n, sizeof(buf) - n, ".%06lldZ", (long long)(us % 1000000));
    return buf;
}

inline std::string journal_name(const std::string& session) {
    return session.empty() ? std::string("index.jsonl")
                           : "session_" + session + ".jsonl";
}

// Дописати один рядок. Лише O_APPEND, без переходу назад — тими ж
// правилами, що й сам запис відео: вирвана флешка псує щонайбільше
// останній рядок.
//
// fsync не робимо навмисно: на FAT32 він чіпає таблицю розміщення й
// каталог, тобто блокує саме тоді, коли носій і так захлинається.
inline void journal_append(const std::string& dir, const std::string& session,
                           const std::string& line) {
    if (dir.empty()) return;
    const int fd = ::open((dir + "/" + journal_name(session)).c_str(),
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    (void)!::write(fd, line.data(), line.size());
    ::close(fd);
}

} // namespace vrx::record
