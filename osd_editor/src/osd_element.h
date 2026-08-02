// osd_element.h — обгортка над ОДНИМ елементом з "elements" у
// osd_config.json.
//
// ДИЗАЙН-РІШЕННЯ: тримаємо nlohmann::json ЯК Є (не розпаковуємо у
// жорсткий C++ struct), і лише читаємо/пишемо конкретні поля через
// геттери/сеттери. Це гарантує, що БУДЬ-ЯКЕ поле, якого редактор не
// знає (майбутнє розширення схеми, коментарі-об'єкти в JSON) —
// НЕ губиться при збереженні. Той самий принцип round-trip safety, що
// й у прошивковому коді (osd_source.cpp:init() теж просто читає
// значення value(), не валідує суворо схему).
//
// КАНАЛ (DATACHANNEL) — НАВМИСНО без сеттера. За задумом Олега канали
// жорстко прив'язані до об'єкта з каталогу (osd_catalog.h) і НЕ
// редагуються після додавання елемента — щоб не можна було випадково
// "переприв'язати" елемент на канал, якому не призначений (напр.
// прогрес-бар батареї на канал GPS).
//
// Сирий числовий id (VT_telemetry_index_e, OSD/vt_telemetry_index.h) —
// той самий формат, що й у рушії VRX. -1 = немає каналу (LABEL-елемент).
#pragma once

#include "json.hpp"
#include "osd_schema.h"
#include <string>
#include <vector>

namespace osdedit {

struct EnumCaseView {
    CompareOp op = CompareOp::EQ;
    float threshold = 0.f;
    std::string label;
};

class OsdElement {
public:
    // Порожній елемент (для create_from_template).
    OsdElement() = default;

    // Обгортає ІСНУЮЧИЙ json-об'єкт елемента (напр. завантажений з файлу).
    static OsdElement wrap(std::string key, nlohmann::json json_value) {
        OsdElement el;
        el.key_ = std::move(key);
        el.json_ = std::move(json_value);
        return el;
    }

    // Створює НОВИЙ елемент з шаблону каталогу (глибока копія), з
    // новим унікальним ключем і позицією (l, t) за замовчуванням.
    static OsdElement create_from_template(const std::string& key,
                                            const nlohmann::json& template_json,
                                            float l, float t) {
        OsdElement el;
        el.key_ = key;
        el.json_ = template_json; // deep copy (nlohmann::json copy-семантика)
        el.json_["L"] = l;
        el.json_["T"] = t;
        return el;
    }

    const std::string& key() const { return key_; }
    const nlohmann::json& raw() const { return json_; }

    ElementType type() const {
        const std::string type_str = json_.value("TYPE", std::string(""));
        if (!type_str.empty()) {
            return element_type_from_json(type_str);
        }
        // Повна сумісність із VRX runtime (OsdSource::init):
        // старі конфіги без TYPE трактуються як VALUE, якщо є DATACHANNEL.
        return data_channel() < 0 ? ElementType::LABEL : ElementType::VALUE;
    }
    void set_type(ElementType t) { json_["TYPE"] = element_type_to_json(t); }

    float l() const { return json_.value("L", 0.f); }
    float t() const { return json_.value("T", 0.f); }
    void set_position(float l, float t) {
        // Затискаємо в межах екрана — не дозволяємо перетягнути елемент
        // за край видимої області, де його буде неможливо знову схопити
        // мишкою для повернення назад.
        if (l < 0.f) l = 0.f;
        if (l > 1.f) l = 1.f;
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        json_["L"] = l;
        json_["T"] = t;
    }

    int size_index() const { return json_.value("S", 0); }
    void set_size_index(int s) {
        if (s < 0) s = 0;
        if (s >= NUM_SIZES) s = NUM_SIZES - 1;
        json_["S"] = s;
    }

    std::string label() const { return json_.value("LABEL", std::string("")); }
    void set_label(const std::string& v) { json_["LABEL"] = v; }

    // Канал — ЛИШЕ читання, немає set_data_channel() навмисно.
    int data_channel() const { return json_.value("DATACHANNEL", -1); }

    std::string units() const { return json_.value("UNITS", std::string("")); }
    void set_units(const std::string& v) { json_["UNITS"] = v; }

    // Знаків після коми для VALUE-виводу (те саме поле, що читає OsdSource
    // у рушії VRX). За замовчуванням 0 — більшість каналів цілі.
    // ФОРМАТ ЗНАЧЕННЯ — тільки читання.
    //
    // Ставиться заготовкою з каталогу й далі не міняється: це не смак, а
    // властивість каналу (секунди польоту, годинник, дата), і дозволити
    // її перемкнути означало б дозволити показати дату як хвилини.
    // Редактор його ЧИТАЄ, щоб превʼю збігалося зі станцією.
    std::string value_format() const { return json_.value("FORMAT", std::string("")); }

    int decimals() const { return json_.value("DECIMALS", 0); }
    void set_decimals(int v) {
        if (v < 0) v = 0;
        json_["DECIMALS"] = v;
    }

    std::string meta() const { return json_.value("META", std::string("")); }

    // --- ENUM_SWITCH ---
    std::vector<EnumCaseView> cases() const {
        std::vector<EnumCaseView> out;
        if (json_.contains("CASES")) {
            for (const auto& c : json_.at("CASES")) {
                EnumCaseView v;
                v.op = compare_op_from_json(c.value("OP", std::string("EQ")));
                v.threshold = c.value("VALUE", 0.f);
                v.label = c.value("LABEL", std::string(""));
                out.push_back(v);
            }
        }
        return out;
    }
    void set_cases(const std::vector<EnumCaseView>& cases) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : cases) {
            nlohmann::json jc;
            jc["OP"] = compare_op_to_json(c.op);
            jc["VALUE"] = c.threshold;
            jc["LABEL"] = c.label;
            arr.push_back(jc);
        }
        json_["CASES"] = arr;
    }
    std::string enum_default() const { return json_.value("DEFAULT", std::string("")); }
    void set_enum_default(const std::string& v) { json_["DEFAULT"] = v; }

    // --- BAR ---
    float bar_min() const { return json_.value("MIN", 0.f); }
    void set_bar_min(float v) { json_["MIN"] = v; }
    float bar_max() const { return json_.value("MAX", 1.f); }
    void set_bar_max(float v) { json_["MAX"] = v; }
    float bar_w() const { return json_.value("BAR_W", 0.15f); }
    void set_bar_w(float v) { json_["BAR_W"] = v; }
    float bar_h() const { return json_.value("BAR_H", 0.02f); }
    void set_bar_h(float v) { json_["BAR_H"] = v; }
    std::string bar_empty_image() const { return json_.value("EMPTY_IMAGE", std::string("")); }
    void set_bar_empty_image(const std::string& v) { json_["EMPTY_IMAGE"] = v; }
    std::string bar_fill_image() const { return json_.value("FILL_IMAGE", std::string("")); }
    void set_bar_fill_image(const std::string& v) { json_["FILL_IMAGE"] = v; }

    // --- HORIZON ---
    std::string horizon_base_image() const { return json_.value("BASE_IMAGE", std::string("")); }
    void set_horizon_base_image(const std::string& v) { json_["BASE_IMAGE"] = v; }
    std::string horizon_pointer_image() const { return json_.value("POINTER_IMAGE", std::string("")); }
    void set_horizon_pointer_image(const std::string& v) { json_["POINTER_IMAGE"] = v; }
    std::string image_path() const { return json_.value("IMAGE", std::string("")); }

    // Чи це "коментар"-запис ("// ...": {}), а не реальний елемент —
    // такі записи в osd_config.json не мають "L"/"T" і слугують лише
    // візуальним розділювачем груп у файлі.
    bool is_comment_entry() const { return !json_.contains("L") || !json_.contains("T"); }

private:
    std::string key_;
    nlohmann::json json_;
};

} // namespace osdedit
