#pragma once

// ЖУРНАЛ СЕАНСУ, ПРОЧИТАНИЙ ДЛЯ ПЛЕЄРА.
//
// Журнал пишеться рекордерами (див. index_open/index_mark/index_close у
// recorder.cpp) і містить те, чого немає в самих файлах: коли шматок
// почався, скільки тривав, чому обірвався, і — головне — пари
// "PTS — записані байти" кожні п'ять секунд.
//
// Для плеєра це три різні речі одразу:
//   1) таймлайн сеансу: де які шматки стоять у часі й де провали;
//   2) таблиця перемотки, якої контейнеру бракує через streamable=true;
//   3) МЕЖА БЕЗПЕЧНОГО ЧИТАННЯ для сеансу, що ще пишеться: далі останньої
//      мітки дані можуть ще не дійти до носія.

#include <cstdint>
#include <string>
#include <vector>

namespace vrx::record {

// Один записаний шматок — те, що між відкриттям і закриттям файлу.
struct IndexFile {
    std::string name;
    std::string channel;
    int64_t anchor_us = 0;     // настінний час, якому відповідає PTS=0
    int64_t end_us = 0;        // настінний час кінця (закриття або остання мітка)
    int64_t safe_bytes = 0;    // скільки байтів напевно на носії
    bool closed = false;       // false = обірваний або ще пишеться
    std::string reason;

    int64_t len_us() const { return end_us > anchor_us ? end_us - anchor_us : 0; }
};

// Куди стрибати, щоб опинитись у потрібному місці запису.
struct SeekPoint {
    std::string name;          // файл у теці сеансу
    int64_t byte_off = 0;      // зсув мітки, НЕ ПІЗНІШЕ за ціль
    int64_t pts_us = 0;        // PTS цієї мітки; далі дочитувати вперед
    bool valid = false;
};

// Коротко про сеанс — рівно те, що треба показати в списку вибору.
struct SessionBrief {
    std::string id;            // 20260830_193028, воно ж час вмикання
    std::string journal;       // повний шлях до журналу

    // ЧАС ВМИКАННЯ СТАНЦІЇ — з імені сеансу. Саме за ним список
    // упорядковано й підписано: людина шукає "той політ, коли я ввімкнув
    // о пів на восьму", а не момент, коли з'явився перший кадр.
    int64_t power_on_us = 0;

    // Час ПЕРШОГО КАДРУ. Може бути помітно пізніший за ввімкнення: поки
    // немає сигналу, рекордер файлів не відкриває взагалі.
    int64_t start_us = 0;
    int64_t length_us = 0;
    bool live = false;         // ще пишеться
    int files = 0;
};

// Усі сеанси на носії, найновіші першими.
//
// Журнал свій на кожне вмикання (session_<мітка>.jsonl), тож список — це
// просто перелік цих файлів по теках днів. Читаються вони цілком: журнал
// навіть за довгий політ важить сотні кілобайтів, а знати треба початок,
// кінець і чи сеанс іще живий.
std::vector<SessionBrief> list_sessions(const std::string& root);

class SessionIndex {
public:
    // Читає журнал. Пошкоджений останній рядок (обірваний запис) просто
    // не враховується — розбір посимвольний і винятків не кидає.
    bool load(const std::string& journal_path);

    const std::string& dir() const { return dir_; }
    const std::string& id() const { return id_; }

    int64_t start_us() const { return start_us_; }
    int64_t end_us() const { return end_us_; }
    int64_t length_us() const { return end_us_ - start_us_; }
    bool live() const { return live_; }          // хоч один файл не закритий

    const std::vector<IndexFile>& files() const { return files_; }

    // Місце в каналі на момент t від ПОЧАТКУ СЕАНСУ.
    //
    // Повертає мітку НЕ ПІЗНІШЕ за ціль: точне попадання дає дочитування
    // вперед щонайбільше на крок мітки (5 с), а це для декодера 0.2 с.
    // Якщо в цей момент канал нічого не писав — valid=false, і це не
    // помилка, а провал у записі.
    SeekPoint locate(const std::string& channel, int64_t t_us) const;

private:
    struct Mark { int64_t pts_us; int64_t bytes; };
    struct Entry { IndexFile f; std::vector<Mark> marks; };

    std::string dir_, id_;
    int64_t start_us_ = 0, end_us_ = 0;
    bool live_ = false;
    std::vector<Entry> entries_;
    std::vector<IndexFile> files_;
};

} // namespace vrx::record
