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
        //
        // Формат на стику з декодером пиниться ЯВНО. mppvideodec хоче
        // byte-stream (Annex B) з вирівнюванням по кадру, а h265parse
        // уміє віддавати і його, і hvc1 із довжинами перед NAL — яку
        // саме форму, вирішує негоціація. Покладатись на її результат
        // не варто: у старому VRX парсер "з'їжджав" у byte-stream,
        // щойно поруч з'являвся сусід-парсер, і це лікували милицями.
        // Один рядок caps робить вимогу явною й знімає питання.
        return "rtph265depay ! h265parse config-interval=1"
               " ! video/x-h265,stream-format=byte-stream,alignment=au";
    }

    std::string decoder() const override { return "mppvideodec"; }

};

} // namespace vrx::source
