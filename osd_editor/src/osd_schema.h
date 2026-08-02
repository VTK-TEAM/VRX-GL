// osd_schema.h — типи елементів OSD-layout. НАВМИСНО дублює enum'и з
// прошивкового osd_types.h (а не #include того файлу напряму), бо
// редактор — окремий десктопний застосунок без залежності від
// DRM/GStreamer заголовків прошивки. Значення (LABEL=0, VALUE=1, ...)
// МУСЯТЬ збігатися з osd_types.h — якщо там зміниться порядок/додасться
// новий тип, синхронізувати тут вручну.
#pragma once

#include <string>
#include <vector>
#include <array>

namespace osdedit {

enum class ElementType : int {
    LABEL = 0,
    VALUE = 1,
    ENUM_SWITCH = 2,
    BAR = 3,
    HORIZON = 4,
};

enum class CompareOp : int { EQ = 0, GT = 1, LT = 2 };

inline const char* element_type_to_json(ElementType t) {
    switch (t) {
        case ElementType::LABEL: return "LABEL";
        case ElementType::VALUE: return "VALUE";
        case ElementType::ENUM_SWITCH: return "ENUM";
        case ElementType::BAR: return "BAR";
        case ElementType::HORIZON: return "HORIZON";
    }
    return "LABEL";
}

inline ElementType element_type_from_json(const std::string& s) {
    if (s == "VALUE") return ElementType::VALUE;
    if (s == "ENUM") return ElementType::ENUM_SWITCH;
    if (s == "BAR") return ElementType::BAR;
    if (s == "HORIZON") return ElementType::HORIZON;
    return ElementType::LABEL;
}

inline const char* element_type_display_name(ElementType t) {
    switch (t) {
        case ElementType::LABEL: return "Лейбл";
        case ElementType::VALUE: return "Текстовий вивід";
        case ElementType::ENUM_SWITCH: return "Перемикач (enum)";
        case ElementType::BAR: return "Прогрес-бар";
        case ElementType::HORIZON: return "Горизонт";
    }
    return "?";
}

inline const char* compare_op_to_json(CompareOp op) {
    switch (op) {
        case CompareOp::EQ: return "EQ";
        case CompareOp::GT: return "GT";
        case CompareOp::LT: return "LT";
    }
    return "EQ";
}

inline CompareOp compare_op_from_json(const std::string& s) {
    if (s == "GT") return CompareOp::GT;
    if (s == "LT") return CompareOp::LT;
    return CompareOp::EQ;
}

inline const char* compare_op_symbol(CompareOp op) {
    switch (op) {
        case CompareOp::EQ: return "=";
        case CompareOp::GT: return ">";
        case CompareOp::LT: return "<";
    }
    return "=";
}

// Кількість розмірів, підтримуваних атласом (узгоджено з
// osd_atlas_builder.cpp: XS/SMALL/MID/BIG/XL, size_index 0..4).
constexpr int NUM_SIZES = 5;
constexpr std::array<const char*, NUM_SIZES> SIZE_NAMES = {
    "XS", "S", "M", "B", "XL"
};

// Один запис доступної іконки атласу (з osd_icon_names.json, генерує
// osd_atlas_builder поруч з atlas.png/osd_glyph_info.bin). Спільний тип
// між IconCatalog (osd_icon_catalog.h, завантаження файлу) і
// OnScreenKeyboard (widgets.h, ICONS-режим клавіатури) — тут, а не в
// жодному з двох, щоб не плодити дублікат структури чи зайву залежність
// одного модуля від іншого.
struct IconKeyInfo {
    std::string name;   // "battery" — показується як підпис під іконкою
    std::string token;  // "<battery>" — вставляється в текст при кліку
};

} // namespace osdedit
