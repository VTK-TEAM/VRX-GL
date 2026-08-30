// osd_document.h — робочий документ (той самий файл, що читає
// прошивка). Тримає елементи в порядку завантаження + окремо
// "коментар"-записи (napr. "// === GROUP ===": {}), щоб при збереженні
// не загубити структуру/групування, яке Олег уже навів у файлі руками.
#pragma once

#include "json.hpp"
#include "osd_element.h"
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace osdedit {

class OsdDocument {
public:
    // Завантажує osd_config.json. Кидає std::runtime_error з людяним
    // повідомленням при помилці — виклик коду вирішує, як показати це
    // користувачу (тут немає доступу до UI).
    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Не вдалося відкрити файл: " + path);
        }
        nlohmann::json root;
        try {
            f >> root;
        } catch (const std::exception& e) {
            throw std::runtime_error("Помилка парсингу JSON (" + path + "): " + e.what());
        }

        elements_.clear();
        comment_entries_.clear();
        path_ = path;

        if (!root.contains("elements")) {
            throw std::runtime_error("У файлі немає кореневого ключа \"elements\": " + path);
        }

        for (auto& item : root.at("elements").items()) {
            OsdElement el = OsdElement::wrap(item.key(), item.value());
            if (el.is_comment_entry()) {
                comment_entries_.emplace_back(item.key(), item.value());
            } else {
                elements_.push_back(std::move(el));
            }
        }
    }

    // Записує елементи (+ коментар-записи, які лишились незмінними) у
    // ТОЙ САМИЙ шлях, з якого завантажили — або за явно заданим path.
    void save(const std::string& path) const {
        nlohmann::json root;
        nlohmann::json elements_obj = nlohmann::json::object();

        for (const auto& [key, val] : comment_entries_) {
            elements_obj[key] = val;
        }
        for (const auto& el : elements_) {
            elements_obj[el.key()] = el.raw();
        }
        root["elements"] = elements_obj;

        std::ofstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Не вдалося відкрити файл для запису: " + path);
        }
        f << root.dump(2);
    }

    void save() const {
        if (path_.empty()) {
            throw std::runtime_error("Документ ще не завантажений з файлу — нема куди зберігати");
        }
        save(path_);
    }

    std::vector<OsdElement>& elements() { return elements_; }
    const std::vector<OsdElement>& elements() const { return elements_; }

    const std::string& path() const { return path_; }

    // Куди зберігати. Потрібне, коли документ узяли КОПІЄЮ іншого:
    // вміст той самий, а файл має бути свій.
    void set_path(const std::string& p) { path_ = p; }

    // Унікальний ключ для НОВОГО елемента: base_key, а якщо зайнятий —
    // base_key_2, base_key_3, ... Прості інкрементальні суфікси
    // достатні тут — колізії малоймовірні і легко видимі користувачу.
    std::string make_unique_key(const std::string& base_key) const {
        if (!key_exists(base_key)) return base_key;
        for (int i = 2; i < 10000; ++i) {
            std::string candidate = base_key + "_" + std::to_string(i);
            if (!key_exists(candidate)) return candidate;
        }
        return base_key + "_x"; // практично недосяжно, але для повноти
    }

    void add(OsdElement el) { elements_.push_back(std::move(el)); }

    void remove_by_key(const std::string& key) {
        elements_.erase(
            std::remove_if(elements_.begin(), elements_.end(),
                            [&](const OsdElement& e) { return e.key() == key; }),
            elements_.end());
    }

    OsdElement* find_by_key(const std::string& key) {
        for (auto& e : elements_) {
            if (e.key() == key) return &e;
        }
        return nullptr;
    }

private:
    bool key_exists(const std::string& key) const {
        for (const auto& e : elements_) {
            if (e.key() == key) return true;
        }
        for (const auto& [ck, cv] : comment_entries_) {
            (void)cv;
            if (ck == key) return true;
        }
        return false;
    }

    std::string path_;
    std::vector<OsdElement> elements_;
    std::vector<std::pair<std::string, nlohmann::json>> comment_entries_;
};

} // namespace osdedit
