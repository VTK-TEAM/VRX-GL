#pragma once

// Основний канал: H.265 в RTP.

#include "video_source.hpp"

namespace vrx::source {

class H265Source : public VideoSource {
public:
    using VideoSource::VideoSource;

protected:
    std::string caps() const override {
        return "application/x-rtp,media=video,clock-rate=90000,"
               "encoding-name=H265,payload=" + std::to_string(config().payload_type);
    }

    std::string parse_chain() const override {
        // config-interval=1 — VPS/SPS/PPS раз на секунду в потік. Без
        // цього декодер, підключений посеред трансляції, чекав би їх
        // до наступного IDR.
        return "rtph265depay ! h265parse config-interval=1";
    }

    std::string decoder() const override { return "mppvideodec"; }

    std::vector<std::string> decode_variants() const override {
        // Перший варіант — без нічого: зазвичай негоціація складається
        // сама. Другий форсує byte-stream явно, і потрібен лише там, де
        // не склалася.
        return {std::string(), "video/x-h265,stream-format=byte-stream,alignment=au"};
    }
};

} // namespace vrx::source
