#pragma once

// ДЕТАЛЬНИЙ ЛОГ ЗАПИСУ. Вмикається дефайном VRX_RECORD_LOG.
//
// ЩО ЦЕ І ЧОМУ ОКРЕМО ВІД ШТАТНОГО ЛОГУ. Штатний друк показує СТАН раз на
// дві секунди: скільки мегабайтів, скільки файлів, чи є носій. Цього
// досить, поки все гаразд, і не досить, щойно щось зламалось: подія
// триває мілісекунди, а між знімками дві секунди, і в них не видно ні
// порядку, ні тривалості.
//
// А розбір зриву — це саме про порядок і тривалість. Коли флешку
// висмикують, питання завжди однакові: коли ядро прибрало пристрій, коли
// це побачила станція, скільки при цьому висіла перевірка носія, скільки
// тривало закриття файлу, скільки байтів лишилось незаписаними. Жодне з
// цих чисел зі зведення не здобувається.
//
// ТРИ РЕЧІ ЗРОБЛЕНО НАВМИСНО.
//
// 1. Пише у ФАЙЛ, і не на носій. Логувати зрив флешки на ту саму флешку
//    безглуздо: запис піде в ту саму діру. Типово /tmp/vrx_record.log.
//
// 2. Кожен рядок СКИДАЄТЬСЯ ОДРАЗУ. Цікаве стається рівно тоді, коли
//    станцію от-от знеструмлять, і буферизований хвіст загубиться саме
//    той, заради якого все й вмикалось. Так само зроблено у спостерігача
//    лінка.
//
// 3. Дві мітки часу в кожному рядку: монотонна від старту (щоб рахувати
//    інтервали) і час доби з мілісекундами (щоб зіставити з dmesg і з
//    тим, коли саме людина смикнула флешку). Одна без другої не працює:
//    монотонну не зіставиш із зовнішніми подіями, а по часу доби не
//    порахуєш тривалість через перевід годинника.

#include "../build_config.h"

#if VRX_RECORD_LOG

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace vrx::diag {

class RecordLog {
public:
    static RecordLog& get() {
        static RecordLog inst;
        return inst;
    }

    void line(const char* tag, const char* fmt, ...) {
        struct timespec mono, wall;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        clock_gettime(CLOCK_REALTIME, &wall);

        struct tm tm_buf;
        localtime_r(&wall.tv_sec, &tm_buf);

        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);

        const double t = (mono.tv_sec - start_.tv_sec)
                       + (mono.tv_nsec - start_.tv_nsec) / 1e9;

        // Вирівнюємо колонку самі: printf рахує ширину в БАЙТАХ, а теги
        // тут кирилицею, тобто по два байти на літеру — з "%-10s"
        // колонка виходить рваною.
        char tagbuf[48];
        int w = 0;
        for (const char* p = tag; *p; ++p) {
            if ((*p & 0xC0) != 0x80) w++;      // рахуємо лише початки символів
        }
        std::snprintf(tagbuf, sizeof(tagbuf), "%s%*s", tag, w < 10 ? 10 - w : 1, "");

        std::lock_guard<std::mutex> lk(mtx_);
        if (!f_) return;
        std::fprintf(f_, "%9.3f %02d:%02d:%02d.%03ld %s %s\n",
                     t, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                     wall.tv_nsec / 1000000, tagbuf, msg);
        std::fflush(f_);
    }

    // Скільки в ядрі лежить незаписаного. Пряма міра того, що втратиться
    // при зриві носія прямо зараз: /proc/meminfo не блокується навіть
    // тоді, коли сама флешка вже не відповідає.
    static long dirty_kb() {
        long kb = -1;
        if (std::FILE* m = std::fopen("/proc/meminfo", "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), m)) {
                if (std::sscanf(line, "Dirty: %ld kB", &kb) == 1) break;
            }
            std::fclose(m);
        }
        return kb;
    }

private:
    RecordLog() {
        clock_gettime(CLOCK_MONOTONIC, &start_);
        const char* p = std::getenv("VRX_RECORD_LOG_OUT");
        f_ = std::fopen(p ? p : "/tmp/vrx_record.log", "w");
        if (f_) {
            std::fprintf(f_, "# детальний лог запису (VRX_RECORD_LOG)\n"
                             "#  час_від_старту  час_доби  хто  подія\n");
            std::fflush(f_);
        } else {
            std::fprintf(stderr, "[лог запису] не відкрився %s\n",
                         p ? p : "/tmp/vrx_record.log");
        }
    }
    ~RecordLog() {
        if (f_) std::fclose(f_);
    }

    std::mutex mtx_;
    std::FILE* f_ = nullptr;
    struct timespec start_ {};
};

} // namespace vrx::diag

#define VRX_RLOG(tag, ...)  ::vrx::diag::RecordLog::get().line((tag), __VA_ARGS__)
#define VRX_RLOG_DIRTY()    ::vrx::diag::RecordLog::dirty_kb()

#else   // VRX_RECORD_LOG == 0

#define VRX_RLOG(tag, ...)  ((void)0)
#define VRX_RLOG_DIRTY()    (-1L)

#endif
