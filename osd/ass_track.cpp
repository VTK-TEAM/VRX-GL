#include "osd/ass_track.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace vrx::osd {
namespace {

// "0:01:23.45" -> мікросекунди. Сотки секунди, як велить формат.
int64_t parse_ts(const char* s, const char* end) {
    int h = 0, m = 0, sec = 0, cs = 0;
    if (std::sscanf(std::string(s, end).c_str(), "%d:%d:%d.%d",
                    &h, &m, &sec, &cs) != 4)
        return -1;
    return ((int64_t)h * 3600 + m * 60 + sec) * 1000000 + (int64_t)cs * 10000;
}

} // namespace

bool AssTrack::load(const std::string& path) {
    lines_.clear();
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string s;
    while (std::getline(f, s)) {
        if (s.rfind("PlayResX:", 0) == 0) { play_w_ = std::atoi(s.c_str() + 9); continue; }
        if (s.rfind("PlayResY:", 0) == 0) { play_h_ = std::atoi(s.c_str() + 9); continue; }
        if (s.rfind("Dialogue:", 0) != 0) continue;

        // Dialogue: 0,0:00:00.00,0:00:00.50,Default,,0,0,0,,{\pos(557,1015)}23:18:10
        //
        // Дев'ять полів через кому, і лише дев'яте може містити коми саме
        // тому воно останнє. Ріжемо рівно вісім разів, а решту беремо як є.
        const char* p = s.c_str() + 9;
        const char* fields[9] = {};
        int nf = 0;
        fields[nf++] = p;
        for (const char* c = p; *c && nf < 9; ++c)
            if (*c == ',') fields[nf++] = c + 1;
        if (nf < 9) continue;

        Line ln;
        ln.start_us = parse_ts(fields[1], fields[2] - 1);
        ln.end_us   = parse_ts(fields[2], fields[3] - 1);
        if (ln.start_us < 0 || ln.end_us < 0) continue;

        const char* text = fields[8];
        if (const char* pos = std::strstr(text, "{\\pos(")) {
            float x = 0, y = 0;
            if (std::sscanf(pos + 6, "%f,%f", &x, &y) == 2) { ln.x = x; ln.y = y; }
            if (const char* close = std::strchr(pos, '}')) text = close + 1;
        }
        ln.text = text;
        if (!ln.text.empty()) lines_.push_back(std::move(ln));
    }

    // За часом початку: пошук по моменту стає двійковим замість перебору
    // сотень тисяч рядків щокадру.
    std::stable_sort(lines_.begin(), lines_.end(),
                     [](const Line& a, const Line& b) { return a.start_us < b.start_us; });
    return !lines_.empty();
}

void AssTrack::at(int64_t t_us, std::vector<const Line*>& out) const {
    out.clear();
    if (lines_.empty()) return;

    // Праву межу знаходимо двійково, ліворуч ідемо назад, поки рядки ще
    // можуть перекривати момент. Довгих рядків тут не буває — станція
    // оновлює телеметрію двічі на секунду, — тож відхід короткий.
    auto it = std::upper_bound(lines_.begin(), lines_.end(), t_us,
                               [](int64_t t, const Line& l) { return t < l.start_us; });
    const int64_t horizon = t_us - 5000000;      // п'ять секунд назад із запасом
    for (auto i = it; i != lines_.begin();) {
        --i;
        if (i->start_us < horizon) break;
        if (t_us >= i->start_us && t_us < i->end_us) out.push_back(&*i);
    }
    std::reverse(out.begin(), out.end());        // повертаємо порядок файлу
}

} // namespace vrx::osd
