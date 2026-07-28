#pragma once

// Другий канал: MJPEG.
//
// Камера шле СИРИЙ JPEG по UDP, БЕЗ RTP-обгортки — перевірено на
// реальному потоці ще в старому VRX; ядро прозоро збирає IP-фрагменти
// на MTU, від нас для цього не потрібно нічого.

#include "video_source.hpp"

namespace vrx::source {

class MjpegSource : public VideoSource {
public:
    using VideoSource::VideoSource;

protected:
    // Обгортки немає — описувати нічого. Порожньо означає "без
    // caps-фільтра на udpsrc взагалі".
    std::string caps() const override { return {}; }

    std::string parse_chain() const override {
        // jpegparse, а НЕ identity: він розбирає межі кадрів по маркерах
        // SOI/EOI і виставляє коректні image/jpeg. Без цих caps далі
        // нічого не негоціюється, а муксер у гілці запису мовчки не
        // пише жодного кадру.
        return "jpegparse";
    }

    std::string decoder() const override {
        // mppjpegdec (апаратний), а не jpegdec (софтверний): софтверний
        // віддає звичайну системну пам'ять, а не dmabuf, і кожен кадр
        // мовчки гинув би на імпорті в текстуру.
        return "mppjpegdec";
    }
};

} // namespace vrx::source
