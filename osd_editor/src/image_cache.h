// image_cache.h — кеш SDL_Texture за шляхом до файлу, для BAR
// (EMPTY_IMAGE/FILL_IMAGE) і HORIZON (IMAGE) елементів. Якщо файл
// відсутній на диску редактора (звичайна ситуація — реальні ассети
// живуть на дроні за шляхами типу /opt/vrx/osd/...), кешується
// "відсутня" мітка і малюється явний плейсхолдер замість мовчазного
// пропуску — щоб було видно, що з елементом щось не так, ще на етапі
// редагування, а не вже в полі.
#pragma once

#include <SDL.h>
#include <SDL_image.h>
#include <string>
#include <unordered_map>

namespace osdedit {

class ImageCache {
public:
    explicit ImageCache(SDL_Renderer* renderer) : renderer_(renderer) {}

    ~ImageCache() { clear(); }

    // Повертає текстуру, або nullptr якщо файл не вдалося завантажити
    // (викликач сам малює плейсхолдер — це дозволяє йому знати розмір
    // плейсхолдера з контексту елемента, а не з кешу).
    SDL_Texture* get(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = cache_.find(path);
        if (it != cache_.end()) return it->second; // може бути nullptr — вже пробували, не вийшло

        SDL_Texture* tex = IMG_LoadTexture(renderer_, path.c_str());
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        }
        cache_[path] = tex; // кешуємо навіть nullptr, щоб не намагатись знову щокадру
        return tex;
    }

    void clear() {
        for (auto& [path, tex] : cache_) {
            (void)path;
            if (tex) SDL_DestroyTexture(tex);
        }
        cache_.clear();
    }

private:
    SDL_Renderer* renderer_;
    std::unordered_map<std::string, SDL_Texture*> cache_;
};

} // namespace osdedit
