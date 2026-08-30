#include "record/session_mlt.hpp"
#include "record/storage.hpp"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <unistd.h>

namespace vrx::record {
namespace {

// --- дрібний розбір JSONL ------------------------------------------------
//
// Свій, а не бібліотечний, і навмисно. Рядки пише той самий проєкт, форма
// їх стала, а головна вимога тут не гнучкість, а СТІЙКІСТЬ ДО ОБРИВУ:
// останній рядок журналу цілком може виявитись половиною, якщо флешку
// висмикнули посеред запису. Пошук ключа просто не знайде його й поверне
// "немає" — тоді як повноцінний розбирач кинув би виняток на весь файл.

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

// --- час -----------------------------------------------------------------

// "2026-08-30T16:14:48.820946Z" -> мікросекунди епохи. Формат наш власний,
// той самий, що й у мітці Matroska DateUTC, — щоб журнал і заголовок файлу
// звірялись очима.
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

// MLT приймає час рядком "гг:хх:сс.ммм". Беремо саме його, а не номери
// кадрів: канали мають різну частоту, і в кадрах довелося б перераховувати
// кожен у свою сітку, множачи похибку.
std::string clock_of(int64_t us) {
    if (us < 0) us = 0;
    const int64_t ms = us / 1000;
    char b[32];
    std::snprintf(b, sizeof(b), "%02lld:%02lld:%02lld.%03lld",
                  (long long)(ms / 3600000), (long long)(ms / 60000 % 60),
                  (long long)(ms / 1000 % 60), (long long)(ms % 1000));
    return b;
}

std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&':  o += "&amp;";  break;
            case '<':  o += "&lt;";   break;
            case '>':  o += "&gt;";   break;
            case '"':  o += "&quot;"; break;
            default:   o += c;
        }
    }
    return o;
}

// --- модель --------------------------------------------------------------

struct FileRec {
    std::string name;        // 20260830_191423_main.mkv
    std::string channel;     // main | pip | capture
    std::string dir;         // повна тека дня, де лежить файл
    int64_t opened_us = -1;
    int64_t closed_us = -1;

    // Пари "настінний час — PTS" із міток звірки. Саме вони дають ЯКІР:
    // момент, коли в цьому файлі PTS дорівнював нулю.
    int64_t anchor_sum = 0;
    int     anchor_n = 0;
    int64_t last_mark_us = -1;

    // Якір = середнє від (час мітки − PTS мітки).
    //
    // Чому не час відкриття: заміряно, між "рекордер відкрив файл" і
    // "перший кадр дійшов до муксера" у основного каналу 203 мс (буфер
    // пересортування), у решти 5-7 мс. Тобто час відкриття зсунув би
    // канали один відносно одного на дві десятих секунди.
    //
    // Чому середнє, а не одна мітка: PTS знімається з останнього кадру,
    // тобто з точністю до кадру — ±17 мс на 60 к/с. Десяток міток це
    // усереднює.
    int64_t anchor_us() const {
        if (anchor_n > 0) return anchor_sum / anchor_n;
        return opened_us;
    }

    int64_t end_us() const {
        if (closed_us > 0) return closed_us;
        return last_mark_us;      // обірваний запис: доки встигли звірити
    }
};

} // namespace

struct SessionMlt::Impl {
    Config cfg;
    Storage& storage;
    std::atomic<bool> running{false};
    std::thread th;
    uint64_t last_sig = 0;        // сумарний розмір журналів минулого разу

    Impl(Config c, Storage& s) : cfg(std::move(c)), storage(s) {}

    // Читає index.jsonl з УСІХ тек дня на носії, а не з однієї.
    //
    // Сеанс може перетнути північ: тоді нові файли підуть у теку
    // наступного дня, а проєкт має зібрати обидві половини.
    bool collect(const std::string& root, std::map<std::string, FileRec>& out,
                 uint64_t* sig) {
        DIR* d = ::opendir(root.c_str());
        if (!d) return false;
        *sig = 0;

        while (dirent* e = ::readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string dir = root + "/" + e->d_name;
            // Шукаємо СВІЙ журнал за іменем, а не спільний. Тому чужі
            // сеанси не доводиться ні читати, ні відсіювати: їхніх рядків
            // тут просто немає.
            FILE* f = std::fopen((dir + "/session_" + cfg.session + ".jsonl").c_str(), "r");
            if (!f) continue;                      // не тека цього сеансу

            char line[1024];
            while (std::fgets(line, sizeof(line), f)) {
                const std::string s(line);
                *sig += s.size();

                std::string sess, ev, file, ch;
                if (!jstr(s, "session", &sess) || sess != cfg.session) continue;
                if (!jstr(s, "event", &ev) || !jstr(s, "file", &file)) continue;

                FileRec& r = out[file];
                r.name = file;
                r.dir = dir;
                if (jstr(s, "channel", &ch)) r.channel = ch;

                std::string t;
                if (ev == "open") {
                    if (jstr(s, "opened", &t)) r.opened_us = parse_iso(t);
                } else if (ev == "close") {
                    if (jstr(s, "closed", &t)) r.closed_us = parse_iso(t);
                } else if (ev == "mark") {
                    double pts = -1;
                    if (jstr(s, "wall", &t) && jnum(s, "pts_s", &pts)) {
                        const int64_t w = parse_iso(t);
                        if (w > 0) {
                            r.last_mark_us = w;
                            if (pts >= 0) {        // -1 = кадрів ще не було
                                r.anchor_sum += w - (int64_t)(pts * 1e6);
                                r.anchor_n++;
                            }
                        }
                    }
                }
            }
            std::fclose(f);
        }
        ::closedir(d);
        return !out.empty();
    }

    // Шлях до файлу ВІДНОСНИЙ, і атрибута root у проєкті немає навмисно.
    //
    // MLT, не знайшовши root, бере теку самого проєкту. Значить теку дня
    // можна скопіювати на комп'ютер куди завгодно, і посилання не зламаються
    // — а з абсолютним шляхом станції вони б не працювали ніде, крім станції.
    std::string rel_path(const FileRec& r, const std::string& proj_dir) const {
        if (r.dir == proj_dir) return r.name;
        const size_t slash = r.dir.rfind('/');
        const std::string day = slash == std::string::npos ? r.dir
                                                           : r.dir.substr(slash + 1);
        return "../" + day + "/" + r.name;         // сеанс перетнув північ
    }

    std::string build(const std::map<std::string, FileRec>& files,
                      const std::string& proj_dir) const {
        // Канали в сталому порядку: основний нижнім шаром, решта над ним.
        static const char* kChannels[] = {"main", "pip", "capture"};

        struct Clip { const FileRec* r; int64_t at; int64_t len; };
        std::vector<Clip> track[3];
        int64_t t0 = -1, tend = 0;

        for (const auto& [_, r] : files) {
            const int64_t a = r.anchor_us(), e = r.end_us();
            if (a <= 0 || e <= a) continue;          // ще нічого не відомо
            int ti = -1;
            for (int i = 0; i < 3; ++i)
                if (r.channel == kChannels[i]) ti = i;
            if (ti < 0) continue;
            track[ti].push_back({&r, a, e - a});
            if (t0 < 0 || a < t0) t0 = a;
            if (e > tend) tend = e;
        }
        if (t0 < 0) return {};

        for (auto& v : track)
            std::sort(v.begin(), v.end(),
                      [](const Clip& a, const Clip& b) { return a.at < b.at; });

        const std::string total = clock_of(tend - t0);
        std::ostringstream x;
        x << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          << "<mlt LC_NUMERIC=\"C\" version=\"7.0.0\" title=\"VRX "
          << xml_escape(cfg.session) << "\">\n"
          << "  <profile description=\"VRX\" width=\"" << cfg.width
          << "\" height=\"" << cfg.height << "\" progressive=\"1\""
          << " sample_aspect_num=\"1\" sample_aspect_den=\"1\""
          << " display_aspect_num=\"" << cfg.width << "\""
          << " display_aspect_den=\"" << cfg.height << "\""
          << " frame_rate_num=\"" << cfg.fps_num << "\""
          << " frame_rate_den=\"" << cfg.fps_den << "\" colorspace=\"709\"/>\n";

        // Тло на всю довжину: без нього доріжки нема на що класти, і в
        // провалах між шматками кадр був би невизначеним.
        x << "  <producer id=\"black\" in=\"00:00:00.000\" out=\"" << total << "\">\n"
          << "    <property name=\"length\">" << total << "</property>\n"
          << "    <property name=\"eof\">pause</property>\n"
          << "    <property name=\"resource\">0</property>\n"
          << "    <property name=\"mlt_service\">color</property>\n"
          << "    <property name=\"mlt_image_format\">rgba</property>\n"
          << "  </producer>\n"
          << "  <playlist id=\"background\">\n"
          << "    <entry producer=\"black\" in=\"00:00:00.000\" out=\"" << total
          << "\"/>\n  </playlist>\n";

        // Вставки другого й третього каналів — у правий стовпчик. Основний
        // лишається повним кадром: він і є те, на що дивляться.
        const int iw = cfg.width * 26 / 100, ih = cfg.height * 26 / 100;
        const int ix = cfg.width - iw - cfg.width / 64;
        const int iy[3] = {0, cfg.height / 32, cfg.height / 32 * 2 + ih};

        int pid = 0;
        std::ostringstream prod, lists;
        for (int ti = 0; ti < 3; ++ti) {
            lists << "  <playlist id=\"playlist" << ti << "\">\n"
                  << "    <property name=\"shotcut:video\">1</property>\n"
                  << "    <property name=\"shotcut:name\">" << kChannels[ti]
                  << "</property>\n";
            int64_t cursor = 0;
            for (const Clip& c : track[ti]) {
                const std::string id = "p" + std::to_string(pid++);
                const std::string dur = clock_of(c.len);

                prod << "  <producer id=\"" << id << "\" in=\"00:00:00.000\" out=\""
                     << dur << "\">\n"
                     << "    <property name=\"length\">" << dur << "</property>\n"
                     << "    <property name=\"resource\">"
                     << xml_escape(rel_path(*c.r, proj_dir)) << "</property>\n"
                     << "    <property name=\"mlt_service\">avformat</property>\n"
                     << "    <property name=\"audio_index\">-1</property>\n";
                if (ti > 0)
                    prod << "    <filter id=\"f" << id << "\">\n"
                         << "      <property name=\"mlt_service\">qtblend</property>\n"
                         << "      <property name=\"rect\">" << ix << " " << iy[ti]
                         << " " << iw << " " << ih << " 1</property>\n"
                         << "    </filter>\n";
                prod << "  </producer>\n";

                // Провал перед шматком — це реальна дірка в записі: сигнал
                // зникав або файл ротувався. Її видно, і це правильно.
                const int64_t at = c.at - t0;
                if (at > cursor)
                    lists << "    <blank length=\"" << clock_of(at - cursor) << "\"/>\n";
                lists << "    <entry producer=\"" << id
                      << "\" in=\"00:00:00.000\" out=\"" << dur << "\"/>\n";
                cursor = at + c.len;
            }
            lists << "  </playlist>\n";
        }
        x << prod.str() << lists.str();

        x << "  <tractor id=\"tractor0\" title=\"VRX " << xml_escape(cfg.session)
          << "\" in=\"00:00:00.000\" out=\"" << total << "\">\n"
          << "    <property name=\"shotcut\">1</property>\n"
          << "    <track producer=\"background\"/>\n";
        for (int ti = 0; ti < 3; ++ti)
            x << "    <track producer=\"playlist" << ti << "\"/>\n";
        for (int ti = 0; ti < 3; ++ti)
            x << "    <transition id=\"t" << ti << "\">\n"
              << "      <property name=\"a_track\">0</property>\n"
              << "      <property name=\"b_track\">" << (ti + 1) << "</property>\n"
              << "      <property name=\"mlt_service\">frei0r.cairoblend</property>\n"
              << "      <property name=\"disable\">0</property>\n"
              << "    </transition>\n";
        x << "  </tractor>\n</mlt>\n";
        return x.str();
    }

    // Пишемо через тимчасове ім'я й rename.
    //
    // Проєкт переписується цілком, і якщо живлення зникне посеред запису,
    // на місці робочого файлу лишиться половина XML — тобто ніщо. rename
    // же або відбувся, або ні: гірше за старий проєкт не стане.
    void publish(const std::string& dir, const std::string& body) {
        const std::string dst = dir + "/session_" + cfg.session + ".mlt";
        const std::string tmp = dst + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        std::fclose(f);
        if (!ok) { ::unlink(tmp.c_str()); return; }
        if (::rename(tmp.c_str(), dst.c_str()) != 0) ::unlink(tmp.c_str());
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            const DriveState st = storage.state();
            if (st.usable()) {
                std::map<std::string, FileRec> files;
                uint64_t sig = 0;
                if (collect(st.root, files, &sig) && sig != last_sig) {
                    // Тека проєкту — де лежить НАЙРАНІШИЙ файл сеансу:
                    // саме там його шукатимуть поруч із відео.
                    const FileRec* first = nullptr;
                    for (const auto& [_, r] : files)
                        if (r.anchor_us() > 0 &&
                            (!first || r.anchor_us() < first->anchor_us()))
                            first = &r;
                    if (first) {
                        const std::string body = build(files, first->dir);
                        if (!body.empty()) {
                            publish(first->dir, body);
                            last_sig = sig;
                        }
                    }
                }
            }
            for (int i = 0; i < cfg.period_ms / 100 &&
                            running.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

SessionMlt::SessionMlt(Config cfg, Storage& storage)
    : impl_(std::make_unique<Impl>(std::move(cfg), storage)) {}
SessionMlt::~SessionMlt() { stop(); }

bool SessionMlt::start() {
    if (impl_->running.exchange(true)) return true;
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[проєкт] сеанс %s, оновлення раз на %d с\n",
                 impl_->cfg.session.c_str(), impl_->cfg.period_ms / 1000);
    return true;
}

void SessionMlt::stop() {
    if (!impl_->running.exchange(false)) return;
    if (impl_->th.joinable()) impl_->th.join();
}

} // namespace vrx::record
