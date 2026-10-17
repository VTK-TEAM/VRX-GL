#pragma once

// Запис файлу СТАНУ так, щоб він пережив зняття живлення.
//
// НАВІЩО. Станцію вимикають, висмикуючи живлення, — чистих вимкнень у полі
// не буває. Корінь станції — ext4 з типовими для Armbian `commit=600` і
// `data=writeback`. Звичайний `ofstream` у таких умовах дає дві біди:
//
//   1. Запис не довговічний. Дані лежать у кеші сторінок до десяти хвилин.
//      Лог уже сказав "збережено", а на картці нічого немає.
//   2. Запис НЕ АТОМАРНИЙ. `ofstream` обрізає файл на відкритті, тож
//      живлення, зняте між обрізанням і скиданням даних, губить і новий
//      стан, І СТАРИЙ: файл лишається порожнім або заповненим 0xFF
//      (розмір закомічений, блоки — ні). Тобто спроба зберегти робить
//      гірше, ніж не зберігати взагалі.
//
// Саме так на релізній станції "не зберігався стан екрана".
//
// ЩО РОБИМО: пишемо в сусідній `.tmp`, fsync'аємо ЙОГО, перейменовуємо
// поверх цілі, далі fsync'аємо КАТАЛОГ. `rename` на ext4 атомарний, а
// fsync каталогу потрібен окремо: без нього сам факт перейменування може
// не дійти до носія. Після цього зняття живлення в будь-який момент
// лишає або старий файл цілим, або новий цілим — третього немає.
//
// ЧОМУ ТУТ fsync, А В ЗАПИСІ ВІДЕО ЙОГО НАВМИСНО НЕМА. Там FAT32 на
// змінному носії, де fsync чіпає таблицю розміщення й упирається в
// ступор флешки на десятки секунд (див. record/recorder.cpp, journal.hpp).
// Тут інша файлова система і файли по кілька кілобайт, що пишуться на дію
// користувача, — ціна нульова. Не «уніфікувати» ці два випадки.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace vrx::ui {

inline bool write_file_durably(const std::string& path, const std::string& data) {
    const std::string tmp = path + ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[стан] не відкрився %s: %s\n", tmp.c_str(), std::strerror(errno));
        return false;
    }

    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n <= 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[стан] запис %s: %s\n", tmp.c_str(), std::strerror(errno));
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= (size_t)n;
    }

    // Дані на носій ДО перейменування: інакше ім'я вкаже на файл, вмісту
    // якого ще немає.
    if (::fsync(fd) != 0) {
        std::fprintf(stderr, "[стан] fsync %s: %s\n", tmp.c_str(), std::strerror(errno));
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    }
    ::close(fd);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        std::fprintf(stderr, "[стан] rename -> %s: %s\n", path.c_str(), std::strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }

    // Саме перейменування теж мусить дійти до носія.
    const size_t slash = path.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".")
                                                         : path.substr(0, slash);
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}

} // namespace vrx::ui
