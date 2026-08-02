// osd_icon_catalog.h — завантажує osd_icon_names.json, який тепер генерує
// osd_atlas_builder поруч з atlas.png/osd_glyph_info.bin (той самий
// принцип відносних шляхів — файл шукається в CWD, звідки запущено
// редактор). Список іконок звідси живить ICONS-режим OnScreenKeyboard
// (widgets.h) — щоб вставляти "<ім'я>" кліком по прев'ю іконки, а не
// набирати вручну по літерах і сподіватись, що не помилився в назві.
//
// НАВМИСНО НЕ кидає виключення при відсутності файлу (на відміну від
// OsdDocument/OsdCatalog) — іконки корисні, але не критичні: старіші
// збірки osd_atlas_builder могли ще не генерувати цей файл, і редактор
// має продовжити працювати без нього (ICONS-режим клавіатури просто
// буде порожній, з підказкою прямо в UI — див. OnScreenKeyboard::draw()).
#pragma once

#include "json.hpp"
#include "osd_schema.h"
#include <string>
#include <vector>
#include <fstream>

namespace osdedit {

class IconCatalog {
public:
    bool load(const std::string& path, std::string* err) {
        entries_.clear();
        std::ifstream f(path);
        if (!f.is_open()) {
            if (err) *err = "не вдалося відкрити " + path;
            return false;
        }
        nlohmann::json root;
        try {
            f >> root;
        } catch (const std::exception& e) {
            if (err) *err = std::string("помилка парсингу: ") + e.what();
            return false;
        }
        if (!root.contains("icons")) {
            if (err) *err = "немає кореневого ключа \"icons\" у " + path;
            return false;
        }
        for (const auto& item : root.at("icons")) {
            IconKeyInfo e;
            e.name = item.value("name", std::string(""));
            if (e.name.empty()) continue;
            e.token = item.value("token", "<" + e.name + ">");
            entries_.push_back(std::move(e));
        }
        return true;
    }

    const std::vector<IconKeyInfo>& entries() const { return entries_; }

private:
    std::vector<IconKeyInfo> entries_;
};

} // namespace osdedit
