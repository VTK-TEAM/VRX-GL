#include "osd.hpp"

#include "json.hpp"
#include "stb_image.h"
#include "telemetry/vt_telemetry_fetch.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace vrx::osd {
namespace {

// Ті самі константи, що в білдері атласу. Розмір кодується в старших
// бітах коду: гліф 'p' розміру 2 має код 0x2070. Іконки живуть окремим
// діапазоном, їхній "код" — хеш імені.
constexpr uint32_t SIZE_STEP = 0x1000u;
constexpr uint32_t CUSTOM_ICON_BASE = 0xE000u;
constexpr uint32_t ICON_HASH_SPACE = 0x1000u;

uint32_t simple_name_hash(const std::string& name) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : name) {
        hash ^= static_cast<unsigned char>(std::tolower(c));
        hash *= 16777619u;
    }
    return hash % ICON_HASH_SPACE;
}

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

} // namespace

// ---------------------------------------------------------------------

struct Osd::Impl {
    Config cfg;

    // Кадр, у частках якого віддаються квади. Ставить рендерер.
    std::atomic<int> frame_w{0};
    std::atomic<int> frame_h{0};

    // Текстури: [0] завжди атлас, далі окремі картинки барів і горизонту.
    std::vector<render::OverlayImage> images;
    std::unordered_map<std::string, int> image_index;   // шлях -> індекс
    int atlas_w = 0, atlas_h = 0;

    std::vector<OsdGlyphInfo> glyphs;
    std::vector<OsdElementConfig> elements;

    // Потік 1: приймач телеметрії. Свій сокет, свій потік — усе всередині.
    VtTelemetryStorage storage;
    VtTelemetryListener listener;

    // Потік 2: збирач списку квадів.
    std::thread builder;
    std::atomic<bool> running{false};
    std::mutex wake_mtx;
    std::condition_variable wake_cv;

    // Опублікований знімок. Пишe збирач, читає рендерер.
    mutable std::mutex out_mtx;
    render::DrawList ready;
    bool ready_fresh = false;
    uint64_t serial = 0;

    mutable std::mutex st_mtx;
    OsdStats st{};

    explicit Impl(Config c) : cfg(std::move(c)) {}

    // --- завантаження ---

    bool load_image(const std::string& path, int* out_index) {
        auto it = image_index.find(path);
        if (it != image_index.end()) {
            if (out_index) *out_index = it->second;
            return true;
        }

        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
        if (!px) {
            std::fprintf(stderr, "[osd] не завантажилось зображення %s (%s)\n",
                         path.c_str(), stbi_failure_reason());
            return false;
        }

        render::OverlayImage img;
        img.id = path;
        img.width = w;
        img.height = h;
        img.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);

        const int idx = (int)images.size();
        images.push_back(std::move(img));
        image_index[path] = idx;
        if (out_index) *out_index = idx;
        std::fprintf(stderr, "[osd] зображення %s: %dx%d\n", path.c_str(), w, h);
        return true;
    }

    bool load_atlas() {
        int idx = -1;
        if (!load_image(cfg.atlas_png, &idx)) return false;
        if (idx != 0) return false;              // атлас має бути першим
        atlas_w = images[0].width;
        atlas_h = images[0].height;
        std::fprintf(stderr, "[osd] атлас %dx%d\n", atlas_w, atlas_h);
        return true;
    }

    bool load_glyphs() {
        std::ifstream bin(cfg.glyph_bin, std::ios::binary);
        if (!bin.is_open()) {
            std::fprintf(stderr, "[osd] не відкрився %s\n", cfg.glyph_bin.c_str());
            return false;
        }
        uint32_t count = 0;
        bin.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!bin) {
            std::fprintf(stderr, "[osd] %s закороткий\n", cfg.glyph_bin.c_str());
            return false;
        }
        glyphs.resize(count);
        bin.read(reinterpret_cast<char*>(glyphs.data()),
                 static_cast<std::streamsize>(count) * sizeof(OsdGlyphInfo));
        if (!bin) {
            std::fprintf(stderr, "[osd] %s пошкоджений: заявлено %u гліфів, даних менше\n",
                         cfg.glyph_bin.c_str(), count);
            return false;
        }
        std::fprintf(stderr, "[osd] гліфів %u\n", count);
        return true;
    }

    bool load_layout() {
        std::ifstream f(cfg.config_json);
        if (!f.is_open()) {
            std::fprintf(stderr, "[osd] не відкрився %s\n", cfg.config_json.c_str());
            return false;
        }
        nlohmann::json j;
        try {
            f >> j;
            for (auto& item : j.at("elements").items()) {
                const auto& val = item.value();
                // Коментарі в конфізі — порожні об'єкти без L/T.
                if (!val.contains("L") && !val.contains("T")) continue;

                OsdElementConfig el;
                el.key = item.key();
                el.l = val.value("L", 0.f);
                el.t = val.value("T", 0.f);
                el.size_index = val.value("S", 0);
                el.label = val.value("LABEL", "");
                el.data_channel = val.value("DATACHANNEL", -1);
                el.units = val.value("UNITS", "");
                el.decimals = val.value("DECIMALS", 0);
                el.value_format = val.value("FORMAT", "");

                const std::string meta = val.value("META", "");
                if (meta == "RATE_HZ") el.channel_meta = OsdChannelMeta::RATE_HZ;
                else if (meta == "AGE_S") el.channel_meta = OsdChannelMeta::AGE_S;
                else el.channel_meta = OsdChannelMeta::NONE;

                // TYPE опційний: старі конфіги без нього поводяться як
                // раніше — є канал, значить VALUE, немає — LABEL.
                const std::string type = val.value("TYPE", "");
                if (type == "LABEL") el.type = OsdElementType::LABEL;
                else if (type == "VALUE") el.type = OsdElementType::VALUE;
                else if (type == "ENUM") el.type = OsdElementType::ENUM_SWITCH;
                else if (type == "BAR") el.type = OsdElementType::BAR;
                else if (type == "HORIZON") el.type = OsdElementType::HORIZON;
                else el.type = (el.data_channel < 0) ? OsdElementType::LABEL
                                                     : OsdElementType::VALUE;

                if (el.type == OsdElementType::ENUM_SWITCH) {
                    if (val.contains("CASES")) {
                        for (const auto& c : val.at("CASES")) {
                            OsdEnumCase ec;
                            const std::string op = c.value("OP", "EQ");
                            if (op == "GT") ec.op = OsdCompareOp::GT;
                            else if (op == "LT") ec.op = OsdCompareOp::LT;
                            else ec.op = OsdCompareOp::EQ;
                            ec.threshold = c.value("VALUE", 0.f);
                            ec.label = c.value("LABEL", "");
                            el.cases.push_back(std::move(ec));
                        }
                    }
                    el.enum_default = val.value("DEFAULT", "");
                } else if (el.type == OsdElementType::BAR) {
                    el.bar_min = val.value("MIN", 0.f);
                    el.bar_max = val.value("MAX", 1.f);
                    el.bar_w = val.value("BAR_W", 0.15f);
                    el.bar_h = val.value("BAR_H", 0.02f);
                    el.bar_empty_image = val.value("EMPTY_IMAGE", "");
                    el.bar_fill_image = val.value("FILL_IMAGE", "");
                    if (!el.bar_empty_image.empty()) load_image(el.bar_empty_image, nullptr);
                    if (!el.bar_fill_image.empty()) load_image(el.bar_fill_image, nullptr);
                    if (el.bar_empty_image.empty() || el.bar_fill_image.empty()) {
                        std::fprintf(stderr, "[osd] \"%s\": BAR без EMPTY_IMAGE/FILL_IMAGE"
                                     " — малюватись не буде\n", el.key.c_str());
                    }
                } else if (el.type == OsdElementType::HORIZON) {
                    el.horizon_base_image = val.value("BASE_IMAGE", "");
                    el.horizon_pointer_image = val.value("POINTER_IMAGE", "");
                    el.image_path = val.value("IMAGE", "");
                    bool any = false;
                    if (!el.horizon_base_image.empty())
                        any |= load_image(el.horizon_base_image, nullptr);
                    if (!el.horizon_pointer_image.empty())
                        any |= load_image(el.horizon_pointer_image, nullptr);
                    if (!el.image_path.empty())
                        any |= load_image(el.image_path, nullptr);
                    if (!any) {
                        std::fprintf(stderr, "[osd] \"%s\": HORIZON без зображень"
                                     " — малюватись не буде\n", el.key.c_str());
                    }
                }

                elements.push_back(std::move(el));
            }
        } catch (const nlohmann::json::exception& e) {
            std::fprintf(stderr, "[osd] помилка розбору %s: %s\n",
                         cfg.config_json.c_str(), e.what());
            return false;
        }
        std::fprintf(stderr, "[osd] елементів layout %zu\n", elements.size());
        return true;
    }

    // --- пошук гліфа ---

    const OsdGlyphInfo* find_glyph(uint32_t full_code) const {
        for (const auto& g : glyphs) {
            if (g.unicode_char == full_code) return &g;
        }
        return nullptr;
    }

    // --- видача квадів ---
    //
    // Прямокутник у частках екрана з прямокутником в атласі. Без
    // обертання — усі текстові квади осьові.
    static void push_rect(render::DrawList& dl, int image,
                          float x, float y, float w, float h,
                          float u0, float v0, float u1, float v1) {
        render::OverlayQuad q;
        q.image = image;
        q.x[0] = x;     q.y[0] = y;        // лівий верхній
        q.x[1] = x + w; q.y[1] = y;        // правий верхній
        q.x[2] = x;     q.y[2] = y + h;    // лівий нижній
        q.x[3] = x + w; q.y[3] = y + h;    // правий нижній
        // Координати ПРИРОДНІ для картинки: верхня вершина бере верхню
        // межу клітинки. Що вісь виводу перевернута — знає рендерер, і
        // виправляє це в одному місці, для позицій. Тут про GL не знають.
        q.u[0] = u0; q.v[0] = v0;
        q.u[1] = u1; q.v[1] = v0;
        q.u[2] = u0; q.v[2] = v1;
        q.u[3] = u1; q.v[3] = v1;
        dl.quads.push_back(q);
    }

    // Гліф. РОЗМІР — його власний, у пікселях атласа.
    //
    // g.width/g.height — це розмір клітинки в пікселях, поділений на
    // розмір атласа. Перевірено на самих даних: g.width*atlas_w дає рівно
    // цілі числа (14, 21, 28, 34, 42 для п'яти розмірів), тобто це і є
    // піксельний розмір, а нормалізація — лише спосіб зберігання. Саме
    // це мав на увазі коментар "для рендеру 1:1" у osd_types.h.
    //
    // Трактувати g.width як частку ШИРИНИ ЕКРАНА (як довелося зробити в
    // CPU-версії VRX, бо там не було чим перевірити) не можна: пропорції
    // атласа й екрана різні, і гліфи вийшли б сплюснуті.
    float emit_glyph(render::DrawList& dl, const OsdGlyphInfo& g,
                     float x, float y, int fw, int fh) const {
        const float w = g.width * atlas_w / fw;
        const float h = g.height * atlas_h / fh;
        push_rect(dl, 0, x, y, w, h,
                  g.left, g.top, g.left + g.width, g.top + g.height);
        return x + w;
    }

    float emit_text(render::DrawList& dl, const std::string& text,
                    float x, float y, int size_index, int fw, int fh) const {
        const uint32_t size_offset = static_cast<uint32_t>(size_index) * SIZE_STEP;
        size_t i = 0;
        while (i < text.size()) {
            uint32_t cp = 0;
            const unsigned char c1 = text[i];
            if (c1 < 0x80) { cp = c1; i += 1; }
            else if ((c1 & 0xE0) == 0xC0 && i + 1 < text.size()) {
                cp = ((c1 & 0x1F) << 6) | (text[i + 1] & 0x3F); i += 2;
            } else if ((c1 & 0xF0) == 0xE0 && i + 2 < text.size()) {
                cp = ((c1 & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
                i += 3;
            } else if (i + 3 < text.size()) {
                cp = ((c1 & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) |
                     ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
                i += 4;
            } else {
                break;   // обрізаний UTF-8 наприкінці
            }
            const OsdGlyphInfo* g = find_glyph(cp + size_offset);
            if (!g) continue;
            x = emit_glyph(dl, *g, x, y, fw, fh);
        }
        return x;
    }

    float emit_icon(render::DrawList& dl, const std::string& icon,
                    float x, float y, int size_index, int fw, int fh) const {
        const uint32_t code = CUSTOM_ICON_BASE + simple_name_hash(icon) +
                              static_cast<uint32_t>(size_index) * SIZE_STEP;
        const OsdGlyphInfo* g = find_glyph(code);
        if (!g) return x;
        return emit_glyph(dl, *g, x, y, fw, fh);
    }

    // Рядок розбирається на сегменти "текст" і "<icon>", які йдуть
    // поспіль. Тобто іконка й текст можуть бути в одному лейблі:
    // "<pilot>dd", "Заряд <battery_low> критичний".
    float emit_label_or_icon(render::DrawList& dl, const std::string& text,
                             float x, float y, int size_index, int fw, int fh) const {
        size_t i = 0;
        const size_t n = text.size();
        while (i < n) {
            const size_t lt = text.find('<', i);
            if (lt == std::string::npos) {
                x = emit_text(dl, text.substr(i), x, y, size_index, fw, fh);
                break;
            }
            if (lt > i) x = emit_text(dl, text.substr(i, lt - i), x, y, size_index, fw, fh);

            const size_t gt = text.find('>', lt + 1);
            if (gt == std::string::npos) {
                // Немає закриваючої — малюємо решту буквально, щоб мовчки
                // не загубити хвіст рядка.
                x = emit_text(dl, text.substr(lt), x, y, size_index, fw, fh);
                break;
            }
            if (gt == lt + 1) {
                x = emit_text(dl, text.substr(lt, gt - lt + 1), x, y, size_index, fw, fh);
            } else {
                x = emit_icon(dl, text.substr(lt + 1, gt - lt - 1), x, y, size_index, fw, fh);
            }
            i = gt + 1;
        }
        return x;
    }

    // Значення в рядок. Звичайний випадок — число з заданою кількістю
    // знаків; окремі канали потребують іншого подання.
    static std::string format_value(const OsdElementConfig& el, float value) {
        char buf[32];

        if (el.value_format == "MM:SS") {
            // Канал віддає секунди. Від'ємних тут не буває, але якщо
            // прийдуть — не показуємо "-1:-5", а тримаємо нуль.
            long total = (long)(value + 0.5f);
            if (total < 0) total = 0;

            // Нулі попереду навмисно: ширина рядка стала, і цифри не
            // сіпають сусідні елементи щохвилини.
            std::snprintf(buf, sizeof(buf), "%02ld:%02ld", total / 60, total % 60);
            return buf;
        }

        std::snprintf(buf, sizeof(buf), "%.*f", el.decimals, value);
        return buf;
    }

    bool fetch_value(const OsdElementConfig& el, float* out) {
        return vt_fetch_channel_value(storage, el.data_channel,
                                      el.channel_meta == OsdChannelMeta::AGE_S,
                                      el.channel_meta == OsdChannelMeta::RATE_HZ,
                                      out);
    }

    // Перший CASE, чия умова підтвердилась, перемагає — семантика
    // switch/case, а не "взяти найкращий".
    std::string eval_enum(const OsdElementConfig& el, float value) const {
        constexpr float EQ_EPS = 0.0001f;   // телеметрія float, точна рівність не спрацює
        for (const auto& c : el.cases) {
            bool matched = false;
            switch (c.op) {
                case OsdCompareOp::EQ: matched = std::fabs(value - c.threshold) < EQ_EPS; break;
                case OsdCompareOp::GT: matched = (value > c.threshold); break;
                case OsdCompareOp::LT: matched = (value < c.threshold); break;
            }
            if (matched) return c.label;
        }
        return el.enum_default;
    }

    // Бар: фон на всю ширину, заповнення поверх, обрізане зліва направо.
    // У CPU-версії обрізання робилось попіксельно; тут це просто підрізана
    // права межа квада разом із UV — картинка не розтягується, а саме
    // обрізається, як і було задумано.
    void emit_bar(render::DrawList& dl, const OsdElementConfig& el, float value,
                  int fw, int fh) const {
        (void)fw; (void)fh;
        if (el.bar_w <= 0.f || el.bar_h <= 0.f) return;

        const float range = el.bar_max - el.bar_min;
        float frac = (range != 0.f) ? (value - el.bar_min) / range : 0.f;
        if (frac < 0.f) frac = 0.f;
        if (frac > 1.f) frac = 1.f;

        auto empty = image_index.find(el.bar_empty_image);
        if (empty != image_index.end()) {
            push_rect(dl, empty->second, el.l, el.t, el.bar_w, el.bar_h, 0.f, 0.f, 1.f, 1.f);
        }
        auto fill = image_index.find(el.bar_fill_image);
        if (fill != image_index.end() && frac > 0.f) {
            push_rect(dl, fill->second, el.l, el.t, el.bar_w * frac, el.bar_h,
                      0.f, 0.f, frac, 1.f);
        }
    }

    // Горизонт: статичний шар плюс обертовий, спільний центр екрана,
    // висота 100%, ширина за пропорцією картинки.
    //
    // У CPU-версії обертання рахувалося попіксельно оберненим перетворенням
    // (вкладені цикли по всьому прямокутнику). Тут достатньо повернути
    // чотири вершини — решту робить растеризатор.
    void emit_horizon(render::DrawList& dl, const OsdElementConfig& el, float angle_deg,
                      int fw, int fh) const {
        const int* base = nullptr;
        const int* ptr = nullptr;
        auto it_base = image_index.find(el.horizon_base_image);
        auto it_ptr = image_index.find(el.horizon_pointer_image);
        auto it_legacy = image_index.find(el.image_path);
        if (it_base != image_index.end()) base = &it_base->second;
        if (it_ptr != image_index.end()) ptr = &it_ptr->second;
        const int* legacy = (it_legacy != image_index.end()) ? &it_legacy->second : nullptr;

        const int* static_idx = base ? base : legacy;
        const int* rot_idx = ptr ? ptr : legacy;
        if (!static_idx || !rot_idx) return;

        // Висота — весь екран, ширина за пропорцією картинки. Обидва шари
        // мають ОДИН центр, інакше вони роз'їдуться.
        const auto& si = images[*static_idx];
        if (si.width <= 0 || si.height <= 0) return;

        const float h = 1.0f;
        const float w = (float(si.width) / si.height) * fh / float(fw);
        const float cx = 0.5f, cy = 0.5f;

        push_rect(dl, *static_idx, cx - w * 0.5f, cy - h * 0.5f, w, h, 0.f, 0.f, 1.f, 1.f);

        // Знак від'ємний: при крені борта лінія горизонту має лишатись
        // горизонтальною — стандартна конвенція авіагоризонту.
        const float a = -angle_deg * 3.14159265358979f / 180.0f;
        const float ca = std::cos(a), sa = std::sin(a);

        // Обертаємо в ПІКСЕЛЯХ, інакше неквадратний кадр перетворив би
        // поворот на скіс.
        const float hw = w * 0.5f * fw, hh = h * 0.5f * fh;
        const float corner_x[4] = { -hw, +hw, -hw, +hw };
        const float corner_y[4] = { -hh, -hh, +hh, +hh };

        render::OverlayQuad q;
        q.image = *rot_idx;
        for (int i = 0; i < 4; ++i) {
            const float rx = corner_x[i] * ca - corner_y[i] * sa;
            const float ry = corner_x[i] * sa + corner_y[i] * ca;
            q.x[i] = cx + rx / fw;
            q.y[i] = cy + ry / fh;
        }
        q.u[0] = 0.f; q.v[0] = 0.f;
        q.u[1] = 1.f; q.v[1] = 0.f;
        q.u[2] = 0.f; q.v[2] = 1.f;
        q.u[3] = 1.f; q.v[3] = 1.f;
        dl.quads.push_back(q);
    }

    // --- збірка одного знімка ---

    void build() {
        const int fw = frame_w.load(std::memory_order_relaxed);
        const int fh = frame_h.load(std::memory_order_relaxed);
        if (fw <= 0 || fh <= 0) return;      // рендерер ще не сказав геометрію

        const int64_t t0 = now_ns();

        render::DrawList dl;
        dl.quads.reserve(512);

        for (const auto& el : elements) {
            float x = el.l;
            const float y = el.t;

            switch (el.type) {
            case OsdElementType::LABEL:
                emit_label_or_icon(dl, el.label, x, y, el.size_index, fw, fh);
                break;

            case OsdElementType::VALUE: {
                x = emit_label_or_icon(dl, el.label, x, y, el.size_index, fw, fh);
                if (el.data_channel >= 0) {
                    std::string value_str = "--";
                    float value = 0.f;
                    if (fetch_value(el, &value)) {
                        value_str = format_value(el, value);
                    }
                    x = emit_label_or_icon(dl, value_str, x, y, el.size_index, fw, fh);
                    if (!el.units.empty()) {
                        emit_label_or_icon(dl, el.units, x, y, el.size_index, fw, fh);
                    }
                }
                break;
            }

            case OsdElementType::ENUM_SWITCH: {
                x = emit_label_or_icon(dl, el.label, x, y, el.size_index, fw, fh);
                if (el.data_channel >= 0) {
                    float value = 0.f;
                    const bool have = fetch_value(el, &value);
                    const std::string out = have ? eval_enum(el, value) : el.enum_default;
                    emit_label_or_icon(dl, out, x, y, el.size_index, fw, fh);
                }
                break;
            }

            case OsdElementType::BAR:
                if (el.data_channel >= 0) {
                    float value = el.bar_min;    // немає даних -> порожній бар
                    fetch_value(el, &value);
                    emit_bar(dl, el, value, fw, fh);
                }
                break;

            case OsdElementType::HORIZON:
                if (el.data_channel >= 0) {
                    float deg = 0.f;             // немає даних -> рівний горизонт
                    fetch_value(el, &deg);
                    emit_horizon(dl, el, deg, fw, fh);
                }
                break;
            }
        }

        const double ms = (now_ns() - t0) / 1e6;

        {
            std::lock_guard<std::mutex> lk(out_mtx);
            dl.serial = ++serial;
            ready = std::move(dl);
            ready_fresh = true;
        }
        {
            std::lock_guard<std::mutex> lk(st_mtx);
            st.builds++;
            st.quads = ready.quads.size();
            st.build_ms = st.build_ms == 0.0 ? ms : st.build_ms * 0.9 + ms * 0.1;
            st.packets = listener.packet_count();
            st.crc_fails = listener.crc_fail_count();
        }
    }

    void loop() {
        while (running.load(std::memory_order_relaxed)) {
            build();
            std::unique_lock<std::mutex> lk(wake_mtx);
            wake_cv.wait_for(lk, std::chrono::milliseconds(cfg.rebuild_ms),
                             [this] { return !running.load(std::memory_order_relaxed); });
        }
    }
};

// ---------------------------------------------------------------------

Osd::Osd(Config cfg) : impl_(new Impl(std::move(cfg))) {}

Osd::~Osd() { stop(); }

bool Osd::init() {
    if (!impl_->load_atlas()) return false;
    if (!impl_->load_glyphs()) return false;
    if (!impl_->load_layout()) return false;
    return true;
}

bool Osd::start() {
    if (impl_->running.load()) return true;

    // Потік 1: приймач телеметрії. Піднімає власний сокет і власний потік.
    if (!impl_->listener.start(impl_->storage, impl_->cfg.telemetry_port)) {
        std::fprintf(stderr, "[osd] приймач телеметрії не піднявся (порт %u)\n",
                     impl_->cfg.telemetry_port);
        return false;
    }

    // Потік 2: збирач списку квадів.
    impl_->running.store(true);
    impl_->builder = std::thread([this] { impl_->loop(); });

    std::fprintf(stderr, "[osd] піднято: телеметрія на порту %u, перезбирання раз на %d мс\n",
                 impl_->cfg.telemetry_port, impl_->cfg.rebuild_ms);
    return true;
}

void Osd::stop() {
    if (impl_->running.exchange(false)) {
        impl_->wake_cv.notify_all();
        if (impl_->builder.joinable()) impl_->builder.join();
    }
    impl_->listener.stop();
}

void Osd::set_frame_size(int width, int height) {
    impl_->frame_w.store(width, std::memory_order_relaxed);
    impl_->frame_h.store(height, std::memory_order_relaxed);
}

const std::vector<render::OverlayImage>& Osd::images() const {
    return impl_->images;
}

bool Osd::acquire(render::DrawList& out) {
    std::lock_guard<std::mutex> lk(impl_->out_mtx);
    if (!impl_->ready_fresh) return false;      // нового немає — хай малює попередній
    out = impl_->ready;
    impl_->ready_fresh = false;
    return true;
}

VtTelemetryStorage& Osd::storage() { return impl_->storage; }

OsdStats Osd::stats() const {
    std::lock_guard<std::mutex> lk(impl_->st_mtx);
    return impl_->st;
}

} // namespace vrx::osd
