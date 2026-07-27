#pragma once

// Рендерер: зводить шари на GPU у власний буфер і сам віддає його на екран.
//
// Працює у ВЛАСНОМУ потоці й керує собою сам. Ззовні — лише три виклики:
// init(), start(), stop(). Жодного "намалюй кадр" чи "покажи" з боку
// викликача немає й не буде: темп задає звільнення буферів, а про нього
// знає лише сам рендерер.
//
// Хто чим володіє:
//   - буфери виводу виділяє РЕНДЕРЕР (через GBM), бо саме він у них пише;
//   - дисплей їх лише імпортує як dmabuf і показує.
// Тому кільце буферів, EGL-контекст і GL-об'єкти живуть тут.
//
// Темп циклу: малювати можна тільки в буфер, який дисплей уже відпустив.
// Рендерер підписується на підтвердження flip'а і прокидається саме на
// нього — без опитування й без сну "на око".

#include "../display/display_manager.hpp"
#include "../source/frame_source.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace vrx::render {

struct RenderStats {
    uint64_t frames = 0;        // намальовано й віддано на показ
    uint64_t stalls = 0;        // цикл чекав вільного буфера
    double last_draw_ms = 0;    // тривалість проходу GPU (по fence)
    double avg_draw_ms = 0;

    // Наскрізна затримка: від виходу кадру з декодера до підтвердження
    // показу. Головна метрика тракту для FPV.
    double latency_avg_ms = 0;
    double latency_max_ms = 0;

    // Поточний зсув опиту від початку періоду — видно, як фаза пливе.
    double poll_offset_ms = 0;

    // ФАЗА: де в періоді розгортки опиняється прихід кадру з камери.
    // 0 — кадр приходить рівно на vblank (найгірше: показ аж через
    // період). Ближче до poll_offset — найкраще: показ одразу.
    // Затримка від приходу до показу = період - фаза.
    double phase_ms = 0;
    double phase_drift_ms_per_s = 0;
};

class GlRenderer {
public:
    struct Config {
        // Пристрій для виділення буферів. Саме card0, а не render-нода:
        // буфери мають бути придатні до сканування (GBM_BO_USE_SCANOUT),
        // а render-нода такі виділяти зазвичай не вміє.
        std::string card = "/dev/dri/card0";

        // Кільце буферів. Два — мінімум і оптимум за затримкою: у той,
        // що зараз сканується, малювати не можна. Третій додав би
        // стійкості до джитера ціною кадру затримки.
        int buffers = 2;

    };

    GlRenderer();
    ~GlRenderer();

    GlRenderer(const GlRenderer&) = delete;
    GlRenderer& operator=(const GlRenderer&) = delete;

    // Готує GBM і EGL, перевіряє наявність потрібних розширень. Розмір і
    // формат буферів беруться з display.layer().info() — тобто дисплей
    // має бути вже відкритий.
    //
    // GL-об'єкти тут НЕ створюються: контекст робиться поточним у
    // робочому потоці, а без нього створювати нічого не можна.
    //
    // Дві перевантаження замість типового аргументу: усередині
    // оголошення класу Config ще неповний, і GCC на "= Config{}"
    // спотикається (те саме в KmsDisplayManager).
    bool init(display::DisplayManager& display);
    bool init(display::DisplayManager& display, Config cfg);

    // Запускає робочий потік. З цього моменту картинка йде на екран.
    bool start();

    // Зупиняє потік і чекає його завершення. Безпечно викликати двічі.
    void stop();

    bool is_running() const;
    RenderStats stats() const;

    // Реєстрація джерел. Можна в будь-який момент, зокрема на ходу:
    // рендерер бере зріз списку на кожен кадр.
    //
    // Рендерер НЕ володіє життям джерела і не запускає його — за це
    // відповідає той, хто джерело створив. Тут лише shared_ptr, щоб
    // джерело не зникло посеред кадру.
    void add_source(std::shared_ptr<source::FrameSource> src);
    void remove_source(const std::shared_ptr<source::FrameSource>& src);
    int source_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::render
