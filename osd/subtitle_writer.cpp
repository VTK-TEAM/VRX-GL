#include "subtitle_writer.hpp"

#include "json.hpp"
#include "telemetry/vt_telemetry_index.h"
#include "telemetry/vt_telemetry_names.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace vrx::osd {
namespace {

std::string format_ass_time(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    const int total_cs = (int)(seconds * 100.0 + 0.5);
    const int cs = total_cs % 100;
    const int total_s = total_cs / 100;
    const int s = total_s % 60;
    const int total_m = total_s / 60;
    const int m = total_m % 60;
    const int h = total_m / 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%02d", h, m, s, cs);
    return buf;
}

// Шлях субтитрів із шляху відео: міняємо ".mkv" на суфікс. Якщо
// розширення інше — просто дописуємо, щоб не втратити файл через
// несподіване ім'я.
std::string subtitle_path_for(const std::string& video, const std::string& suffix) {
    const auto pos = video.rfind(".mkv");
    if (pos == std::string::npos || pos != video.size() - 4) return video + suffix;
    return video.substr(0, pos) + suffix;
}

// Іконки в субтитрах не потрібні: у плеєра немає нашого атласу, і
// "<battery_low>" виглядало б як сміття посеред тексту.
std::string strip_icon_tokens(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        const size_t lt = text.find('<', i);
        if (lt == std::string::npos) { out.append(text, i, std::string::npos); break; }
        out.append(text, i, lt - i);
        const size_t gt = text.find('>', lt + 1);
        if (gt == std::string::npos) { out.push_back('<'); i = lt + 1; continue; }
        i = gt + 1;
    }
    return out;
}

std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// У .ass фігурна дужка відкриває тег, а перенос рядка руйнує запис
// події. Обидва мають потрапити в текст як символи.
std::string ass_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '{': out += "\\{"; break;
            case '}': out += "\\}"; break;
            case '\n': out += "\\N"; break;
            case '\r': break;
            default: out += c;
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------

struct SubtitleWriter::Impl {
    Config cfg;

    // Елемент розкладки в тому вигляді, в якому він потрібен субтитрам:
    // без розмірів, іконок і типів — лише позиція, підпис і канал.
    struct Element {
        int channel = -1;
        int decimals = 0;
        float l = 0.f, t = 0.f;
        std::string label;
        std::string units;
        std::string value_format;
    };
    std::vector<Element> elements;

    struct DebugChannel {
        std::string label;
        int channel = -1;
    };
    std::vector<DebugChannel> debug_channels;

    const record::Recorder* rec = nullptr;
    const record::Storage* drive = nullptr;
    VtTelemetryStorage* storage = nullptr;

    // Номер носія, на якому відкриті файли. Порівнюється саме він, а не
    // шлях: та сама точка монтування може дістатися іншій флешці, і
    // дописувати в "той самий" файл на ній не можна.
    uint32_t cur_generation = 0;

    std::thread th;
    std::atomic<bool> running{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    std::ofstream layout_out;
    std::ofstream debug_out;
    bool was_active = false;
    std::string cur_video;

    // Курсор часової шкали. Рахується від початку ЗАПИСУ, кроком
    // snapshot_ms — саме тому знімки лягають рівно, навіть якщо потік
    // прокинувся з запізненням.
    std::chrono::milliseconds cursor{0};
    std::chrono::steady_clock::time_point next_at{};

    mutable std::mutex st_mtx;
    SubtitleStats st{};

    explicit Impl(Config c) : cfg(std::move(c)) {}

    bool load_layout() {
        std::ifstream f(cfg.config_json);
        if (!f.is_open()) {
            std::fprintf(stderr, "[субтитри] не відкрився %s\n", cfg.config_json.c_str());
            return false;
        }
        nlohmann::json root;
        try {
            f >> root;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[субтитри] помилка розбору %s: %s\n",
                         cfg.config_json.c_str(), e.what());
            return false;
        }
        if (!root.contains("elements") || !root["elements"].is_object()) return false;

        for (auto& [key, val] : root["elements"].items()) {
            (void)key;
            if (!val.is_object()) continue;

            Element e;
            e.l = val.value("L", 0.f);
            e.t = val.value("T", 0.f);
            e.units = val.value("UNITS", std::string(""));
            e.decimals = val.value("DECIMALS", 0);
            e.value_format = val.value("FORMAT", std::string(""));
            e.label = trim_copy(strip_icon_tokens(val.value("LABEL", std::string(""))));
            e.channel = val.value("DATACHANNEL", -1);

            // Ні каналу, ні підпису — писати нічого.
            if (e.channel < 0 && e.label.empty()) continue;
            elements.push_back(std::move(e));
        }
        std::fprintf(stderr, "[субтитри] елементів розкладки %zu\n", elements.size());
        return true;
    }

    // Імена каналів беремо з тієї ж таблиці, що й редактор — щоб не
    // тримати другий список назв, який неминуче розсинхронізується.
    void load_debug_channels() {
        debug_channels.clear();
        for (int ch = 0; ch <= VT_TLM_TOS_RX_PERIOD_MS; ++ch) {
            debug_channels.push_back({vt_telemetry_channel_name(ch), ch});
        }
        // Локальні канали — усі, а не лише два. У VRX сюди внесли
        // RECORDING_STATE і LINE_LOSS, а частоти лишились поза таблицею,
        // хоча саме вони й потрібні при розборі польоту: по них видно,
        // де загубились кадри — у лінку, в декодері чи вже на показі.
        for (uint8_t ch : {VT_TLM_LOCAL_RECORDING_STATE, VT_TLM_LOCAL_LINE_LOSS,
                           VT_TLM_LOCAL_H265_FPS, VT_TLM_LOCAL_MJPEG_FPS,
                           VT_TLM_LOCAL_H265_SHOWN_FPS, VT_TLM_LOCAL_DISPLAY_FPS,
                           VT_TLM_LOCAL_PHASE_LOCK, VT_TLM_LOCAL_LATENCY_MS,
                           VT_TLM_LOCAL_DROPPED_FPS, VT_TLM_LOCAL_LATE_FPS,
                           VT_TLM_LOCAL_LOST_FRAMES}) {
            debug_channels.push_back({vt_telemetry_channel_name(ch), ch});
        }
        std::fprintf(stderr, "[субтитри] дебажних каналів %zu\n", debug_channels.size());
    }

    // --- відкриття й закриття ---

    void write_header(std::ofstream& out, const char* title) {
        const int font = std::max(14, cfg.video_w / 60);
        out << "[Script Info]\n"
            << "Title: " << title << "\n"
            << "ScriptType: v4.00+\n"
            << "PlayResX: " << cfg.video_w << "\n"
            << "PlayResY: " << cfg.video_h << "\n"
            << "WrapStyle: 2\n"
            << "ScaledBorderAndShadow: yes\n\n"
            << "[V4+ Styles]\n"
            << "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour,"
               " BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle,"
               " BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
            << "Style: Default,Consolas," << font
            << ",&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
            << "0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1\n\n"
            << "[Events]\n"
            << "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
        out.flush();
    }

    void open_for(const std::string& video) {
        if (video.empty()) return;

        cursor = std::chrono::milliseconds(0);
        next_at = std::chrono::steady_clock::now();
        cur_video = video;

        const std::string lp = subtitle_path_for(video, cfg.layout_suffix);
        layout_out.open(lp, std::ios::out | std::ios::trunc);
        if (layout_out.is_open()) {
            write_header(layout_out, "OSD telemetry (auto-generated)");
            std::fprintf(stderr, "[субтитри] %s\n", lp.c_str());
        } else {
            std::fprintf(stderr, "[субтитри] не відкрився на запис %s\n", lp.c_str());
        }

        if (cfg.write_debug) {
            const std::string dp = subtitle_path_for(video, cfg.debug_suffix);
            debug_out.open(dp, std::ios::out | std::ios::trunc);
            if (debug_out.is_open()) {
                write_header(debug_out, "OSD debug table (auto-generated)");
                std::fprintf(stderr, "[субтитри] %s\n", dp.c_str());
            }
        }

        std::lock_guard<std::mutex> lk(st_mtx);
        st.active = layout_out.is_open() || debug_out.is_open();
        st.file = lp;
        st.files++;
    }

    void close_output() {
        if (layout_out.is_open()) { layout_out.flush(); layout_out.close(); }
        if (debug_out.is_open()) { debug_out.flush(); debug_out.close(); }
        cur_video.clear();
        std::lock_guard<std::mutex> lk(st_mtx);
        st.active = false;
    }

    // --- знімки ---

    std::string value_of(const Element& e, bool* has) {
        *has = false;
        if (e.channel < 0) return {};

        float raw = 0.f;
        if (!storage->get_value((uint8_t)e.channel, &raw)) return {};
        // Сентинел прошивки: канал є, але джерело даних недоступне.
        if (raw <= VT_TELEMETRY_SOURCE_NOT_AVAILABLE + 0.5f) return {};

        *has = true;
        char buf[32];
        if (e.value_format == "MM:SS") {
            long total = (long)(raw + 0.5f);
            if (total < 0) total = 0;
            std::snprintf(buf, sizeof(buf), "%02ld:%02ld", total / 60, total % 60);
        } else {
            std::snprintf(buf, sizeof(buf), "%.*f", e.decimals, raw);
        }
        return buf;
    }

    void write_layout_snapshot(const std::string& start, const std::string& end) {
        if (!layout_out.is_open()) return;

        for (const Element& e : elements) {
            bool has = false;
            const std::string value = trim_copy(strip_icon_tokens(value_of(e, &has)));

            std::ostringstream text;
            if (!e.label.empty()) text << e.label;
            if (!e.label.empty() && has && !value.empty()) text << ": ";
            if (has) text << value;
            if (!e.units.empty()) text << e.units;

            const std::string line = ass_escape(trim_copy(text.str()));
            if (line.empty()) continue;

            const int px = (int)(e.l * cfg.video_w + 0.5f);
            const int py = (int)(e.t * cfg.video_h + 0.5f);

            layout_out << "Dialogue: 0," << start << "," << end
                       << ",Default,,0,0,0,,{\\pos(" << px << "," << py << ")}" << line << "\n";
        }
        layout_out.flush();
    }

    void write_debug_snapshot(const std::string& start, const std::string& end) {
        if (!debug_out.is_open()) return;

        std::vector<std::string> rows;
        rows.reserve(debug_channels.size());
        for (const DebugChannel& c : debug_channels) {
            float raw = 0.f;
            uint32_t age = 0;
            char buf[128];
            if (storage->get_value((uint8_t)c.channel, &raw, &age)) {
                std::snprintf(buf, sizeof(buf), "%3d %-24s %10.2f  %ums",
                              c.channel, c.label.c_str(), raw, age);
            } else {
                std::snprintf(buf, sizeof(buf), "%3d %-24s %10s",
                              c.channel, c.label.c_str(), "-");
            }
            rows.push_back(buf);
        }

        const int left = std::max(10, cfg.video_w / 80);
        const int top = std::max(10, cfg.video_h / 80);
        const int base_row_h = std::max(16, cfg.video_h / 54);
        // +10% до міжрядкового, щоб рядки не злипались.
        const int row_h = std::max(18, (base_row_h * 11 + 9) / 10);
        const int col_gap = std::max(24, cfg.video_w / 100);

        int columns = 3;
        const int usable_w = std::max(1, cfg.video_w - left * 2);
        while (columns > 1) {
            const int candidate = (usable_w - (columns - 1) * col_gap) / columns;
            if (candidate >= 220) break;
            --columns;
        }
        const int col_w = std::max(1, (usable_w - (columns - 1) * col_gap) / columns);
        const int y_start = top + row_h * 2;
        const int rows_per_col = std::max(1, (cfg.video_h - y_start - row_h) / row_h);
        const int max_rows = rows_per_col * columns;

        if ((int)rows.size() > max_rows) {
            rows.resize(max_rows);
            if (!rows.empty()) rows.back() = "...";
        }

        std::ostringstream head;
        head << "DEBUG OSD TABLE | channel | value | age | cols=" << columns
             << " | lines=" << rows.size();
        debug_out << "Dialogue: 0," << start << "," << end
                  << ",Default,,0,0,0,,{\\pos(" << left << "," << top << ")}"
                  << ass_escape(head.str()) << "\n";

        for (size_t i = 0; i < rows.size(); ++i) {
            const int col = (int)i / rows_per_col;
            const int row = (int)i % rows_per_col;
            if (col >= columns) break;
            const int x = left + col * (col_w + col_gap);
            const int y = y_start + row * row_h;
            debug_out << "Dialogue: 0," << start << "," << end
                      << ",Default,,0,0,0,,{\\pos(" << x << "," << y << ")}"
                      << ass_escape(rows[i]) << "\n";
        }
        debug_out.flush();
    }

    void snapshot() {
        const double start_s = std::chrono::duration<double>(cursor).count();
        cursor += std::chrono::milliseconds(cfg.snapshot_ms);
        const double end_s = std::chrono::duration<double>(cursor).count();

        const std::string start = format_ass_time(start_s);
        const std::string end = format_ass_time(end_s);

        write_layout_snapshot(start, end);
        write_debug_snapshot(start, end);

        std::lock_guard<std::mutex> lk(st_mtx);
        st.snapshots++;
    }

    void poll_once() {
        // НОСІЙ — через той самий клас, що й рекордери. Стан беремо
        // знімком: state() не блокується на вводі-виводі навіть тоді,
        // коли флешку висмикнули посеред роботи.
        const auto ds = drive->state();
        if (!ds.usable() || (cur_generation != 0 && ds.generation != cur_generation)) {
            // Носія немає або він УЖЕ ІНШИЙ. Далі не пишемо: файл, який
            // ми тримаємо відкритим, лежить на тому, що зникло.
            if (layout_out.is_open() || debug_out.is_open()) close_output();
            was_active = false;
            cur_generation = 0;
            return;
        }

        const auto rs = rec->stats();

        // НОВИЙ ФАЙЛ — теж привід перевідкритись, не лише старт запису:
        // рекордер сам ротує файли за розміром і перезапускається на
        // відновленні сигналу. Без цієї перевірки другий і подальші
        // файли лишились би без субтитрів.
        if (rs.active && (!was_active || rs.file != cur_video)) {
            if (was_active) close_output();
            open_for(rs.file);
            cur_generation = ds.generation;
        } else if (!rs.active && was_active) {
            close_output();
            cur_generation = 0;
        }
        was_active = rs.active;

        if (!rs.active) return;
        if (!layout_out.is_open() && !debug_out.is_open()) return;

        const auto now = std::chrono::steady_clock::now();
        if (now < next_at) return;
        next_at += std::chrono::milliseconds(cfg.snapshot_ms);
        // Якщо потік проспав кілька періодів (носій гальмував), не
        // намагаємось надолужити чергою знімків — просто йдемо далі від
        // теперішнього моменту.
        if (next_at < now) next_at = now + std::chrono::milliseconds(cfg.snapshot_ms);

        snapshot();
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            poll_once();
            std::unique_lock<std::mutex> lk(wake_mtx);
            wake_cv.wait_for(lk, std::chrono::milliseconds(50),
                             [this] { return !running.load(std::memory_order_relaxed); });
        }
        close_output();
    }
};

// ---------------------------------------------------------------------

SubtitleWriter::SubtitleWriter(Config cfg) : impl_(new Impl(std::move(cfg))) {}

SubtitleWriter::~SubtitleWriter() { stop(); }

bool SubtitleWriter::init() {
    const bool ok = impl_->load_layout();
    if (impl_->cfg.write_debug) impl_->load_debug_channels();
    return ok;
}

bool SubtitleWriter::start(const record::Recorder& rec,
                           const record::Storage& drive,
                           VtTelemetryStorage& storage) {
    if (impl_->running.load()) return true;
    impl_->rec = &rec;
    impl_->drive = &drive;
    impl_->storage = &storage;
    impl_->running.store(true);
    impl_->th = std::thread([this] { impl_->loop(); });
    std::fprintf(stderr, "[субтитри] піднято: знімок раз на %d мс, PlayRes %dx%d\n",
                 impl_->cfg.snapshot_ms, impl_->cfg.video_w, impl_->cfg.video_h);
    return true;
}

void SubtitleWriter::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->wake_cv.notify_all();
    if (impl_->th.joinable()) impl_->th.join();
}

SubtitleStats SubtitleWriter::stats() const {
    std::lock_guard<std::mutex> lk(impl_->st_mtx);
    return impl_->st;
}

} // namespace vrx::osd
