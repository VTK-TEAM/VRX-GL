// osd_catalog.h — каталог "об'єктів для додавання" (кнопка "+" в куті
// редактора). ОКРЕМИЙ файл від osd_config.json — тут просто список
// шаблонів: людяна назва + повний JSON-шаблон елемента з УЖЕ ПРОШИТИМ
// каналом і дефолтними налаштуваннями. Редактор при додаванні бере
// шаблон, робить deep copy, підставляє позицію (де клікнули "+"/де
// поставили на канві) і згенерований унікальний ключ — сам канал
// (DATACHANNEL) в UI ніде не редагується (див. коментар в osd_element.h).
#pragma once

#include "json.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>

namespace osdedit {

struct CatalogEntry {
    std::string display_name;   // те, що бачить користувач у списку "+"
    std::string key_prefix;     // основа для ключа нового елемента
    nlohmann::json tpl;         // шаблон JSON (без L/T — підставляються при додаванні)
};

class OsdCatalog {
public:
    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Не вдалося відкрити каталог: " + path);
        }
        nlohmann::json root;
        try {
            f >> root;
        } catch (const std::exception& e) {
            throw std::runtime_error("Помилка парсингу каталогу (" + path + "): " + e.what());
        }
        entries_.clear();
        if (!root.contains("catalog")) {
            throw std::runtime_error("У каталозі немає кореневого ключа \"catalog\": " + path);
        }
        for (const auto& item : root.at("catalog")) {
            CatalogEntry e;
            e.display_name = item.value("display_name", std::string("(без назви)"));
            e.key_prefix = item.value("key_prefix", std::string("element"));
            if (!item.contains("template")) {
                throw std::runtime_error("Запис каталогу \"" + e.display_name + "\" без \"template\"");
            }
            e.tpl = item.at("template");
            entries_.push_back(std::move(e));
        }
    }

    const std::vector<CatalogEntry>& entries() const { return entries_; }

private:
    std::vector<CatalogEntry> entries_;
};

} // namespace osdedit
