#include "record/session_index.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vrx::record {
namespace {

// Розбір рядка журналу — свій, а не бібліотечний, і навмисно.
//
// Форма рядків стала, їх пише цей самий проєкт. А головна вимога тут не
// гнучкість, а СТІЙКІСТЬ ДО ОБРИВУ: останній рядок цілком може виявитись
// половиною, якщо флешку висмикнули посеред запису. Пошук ключа просто не
// знайде його й поверне "немає", тоді як повноцінний розбирач кинув би
// виняток на весь файл — і плеєр не відкрив би сеанс саме тоді, коли він
// найцікавіший.

bool jfind(const std::string& s, const char* key, size_t* pos) {
    const std::string pat = std::string("\"") + key + "\":";
    const size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    *pos = p + pat.size();
    return true;
}

bool jstr(const std::string& s, const char* key, std::string* out) {
    size_t p;
    if (!jfind(s, key, &p) || p >= s.size() || s[p] != '"') return false;
    const size_t e = s.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = s.substr(p + 1, e - p - 1);
    return true;
}

bool jnum(const std::string& s, const char* key, double* out) {
    size_t p;
    if (!jfind(s, key, &p)) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    *out = v;
    return true;
}

// "2026-08-30T16:14:48.820946Z" -> мікросекунди епохи. Той самий формат,
// що й мітка DateUTC у контейнері, — щоб журнал і заголовок файлу можна
// було звірити очима.
int64_t parse_iso(const std::string& s) {
    struct tm tm {};
    if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return -1;
    const time_t sec = timegm(&tm);
    int64_t frac = 0;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        std::string f = s.substr(dot + 1, 6);
        while (f.size() < 6) f.push_back('0');
        frac = std::strtoll(f.c_str(), nullptr, 10);
    }
    return (int64_t)sec * 1000000 + frac;
}

} // namespace

bool SessionIndex::load(const std::string& journal_path) {
    entries_.clear();
    files_.clear();
    live_ = false;

    const size_t slash = journal_path.rfind('/');
    dir_ = slash == std::string::npos ? "." : journal_path.substr(0, slash);

    FILE* fp = std::fopen(journal_path.c_str(), "r");
    if (!fp) return false;

    // Якорі й кінці накопичуються ОКРЕМО від сум, бо якір — це середнє по
    // мітках, і поки файл не дочитано, ділити нема на що.
    std::vector<int64_t> asum;
    std::vector<int>     acnt;
    std::vector<int64_t> opened;

    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        const std::string s(line);
        std::string ev, file, ch;
        if (!jstr(s, "event", &ev) || !jstr(s, "file", &file)) continue;
        if (id_.empty()) jstr(s, "session", &id_);

        size_t i = 0;
        for (; i < entries_.size(); ++i)
            if (entries_[i].f.name == file) break;
        if (i == entries_.size()) {
            entries_.push_back({});
            entries_.back().f.name = file;
            asum.push_back(0); acnt.push_back(0); opened.push_back(-1);
        }
        Entry& e = entries_[i];
        if (jstr(s, "channel", &ch)) e.f.channel = ch;

        std::string t;
        if (ev == "open") {
            if (jstr(s, "opened", &t)) opened[i] = parse_iso(t);
        } else if (ev == "close") {
            double dur = 0;
            if (jstr(s, "closed", &t)) e.f.end_us = parse_iso(t);
            if (jnum(s, "bytes", &dur)) e.f.safe_bytes = (int64_t)dur;
            jstr(s, "reason", &e.f.reason);
            e.f.closed = true;
        } else if (ev == "mark") {
            double pts = -1, by = 0;
            if (!jstr(s, "wall", &t) || !jnum(s, "pts_s", &pts)) continue;
            const int64_t w = parse_iso(t);
            if (w <= 0) continue;
            if (jnum(s, "bytes", &by)) e.f.safe_bytes = (int64_t)by;
            if (pts < 0) continue;              // кадрів ще не було
            const int64_t pus = (int64_t)(pts * 1e6);
            asum[i] += w - pus;
            acnt[i]++;
            e.marks.push_back({pus, (int64_t)by});
            if (!e.f.closed) e.f.end_us = w;    // поки не закрився — доки звірили
        }
    }
    std::fclose(fp);

    // ЯКІР — середнє від (час мітки − PTS мітки), а не час відкриття.
    //
    // Заміряно: між "рекордер відкрив файл" і "перший кадр дійшов до
    // муксера" в основного каналу 203 мс, у решти 5–7 мс. За часом
    // відкриття канали розійшлися б на дві десятих секунди. Середнє, бо
    // PTS знімається з останнього кадру — з точністю до кадру (±17 мс).
    start_us_ = 0; end_us_ = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        e.f.anchor_us = acnt[i] > 0 ? asum[i] / acnt[i] : opened[i];
        if (e.f.anchor_us <= 0 || e.f.end_us <= e.f.anchor_us) continue;
        if (!e.f.closed) live_ = true;
        if (start_us_ == 0 || e.f.anchor_us < start_us_) start_us_ = e.f.anchor_us;
        if (e.f.end_us > end_us_) end_us_ = e.f.end_us;
        std::sort(e.marks.begin(), e.marks.end(),
                  [](const Mark& a, const Mark& b) { return a.pts_us < b.pts_us; });
        files_.push_back(e.f);
    }
    std::sort(files_.begin(), files_.end(),
              [](const IndexFile& a, const IndexFile& b) {
                  return a.anchor_us < b.anchor_us;
              });
    return !files_.empty();
}

SeekPoint SessionIndex::locate(const std::string& channel, int64_t t_us) const {
    SeekPoint sp;
    const int64_t want = start_us_ + t_us;

    for (const Entry& e : entries_) {
        if (e.f.channel != channel) continue;
        if (e.f.anchor_us <= 0) continue;
        if (want < e.f.anchor_us || want > e.f.end_us) continue;

        const int64_t rel = want - e.f.anchor_us;   // PTS, якого хочемо

        // Мітка НЕ ПІЗНІШЕ за ціль: з неї дочитуємо вперед. Пізніша мітка
        // проскочила б потрібний момент, і назад дороги немає.
        const Mark* best = nullptr;
        for (const Mark& m : e.marks) {
            if (m.pts_us > rel) break;
            best = &m;
        }
        sp.name = e.f.name;
        sp.byte_off = best ? best->bytes : 0;
        sp.pts_us = best ? best->pts_us : 0;
        sp.valid = true;
        return sp;
    }
    return sp;      // провал у записі — не помилка
}

} // namespace vrx::record
