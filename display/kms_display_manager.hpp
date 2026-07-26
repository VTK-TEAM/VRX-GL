#pragma once

// Реалізація DisplayManager поверх DRM/KMS atomic.
//
// Задача класу рівно одна: перекидати буфери на екран у правильні моменти
// і чесно тримати їхні стани. Він нічого не малює й нічого не зводить —
// готовий кадр приходить ззовні.
//
// Три стани кадру, які тут відстежуються:
//   pending   — покладений через submit(), ще не відданий залізу;
//   in_flight — commit зроблено, підтвердження flip'а ще не прийшло;
//   current   — фізично на екрані, контролер читає саме його.
//
// Правильне звільнення буферів тримається на цьому поділі: буфер вільний
// не після commit'а, а лише коли flip підтверджено і він перестав бути
// current. Помилка тут дає розриви кадру, які потім довго не зрозуміти.
//
// Заголовок свідомо не тягне <xf86drmMode.h> і <gbm.h> — уся ця машинерія
// схована в .cpp, щоб не текти в кожен файл, що включає дисплей.

#include "display_manager.hpp"

#include <memory>
#include <string>

namespace vrx::display {

class KmsDisplayManager final : public DisplayManager {
public:
    struct Config {
        // Пристрій. card0 майже завжди, але на платах із кількома DRM
        // (напр. окремий VOP і NPU) буває інакше.
        std::string card = "/dev/dri/card0";

        // Режим беремо той, що дисплей позначив PREFERRED. Якщо тут
        // задати ненульові значення — спробуємо знайти саме такий режим,
        // а не знайшовши, все одно візьмемо PREFERRED і скажемо про це.
        int want_width = 0;
        int want_height = 0;
        int want_refresh_mhz = 0;

        // Пінимо явно, а не лишаємо драйверу.
        //
        // 8 біт: 10-бітних даних у тракті немає взагалі (H.265 8 біт,
        // OSD 8 біт, буфер XRGB8888), тож 30bit гнав би доповнені нулями
        // 8 біт і вимагав у 1.25 раза ширшого лінка. На наземній станції
        // з довгим кабелем це рівно та різниця, через яку картинка
        // починає моргати. Виграшу нуль.
        //
        // RGB: буфер у нас RGB, і віддавати його як YCbCr означає дві
        // конверсії замість жодної. Драйвер за замовчуванням обирає за
        // EDID — тобто на іншому моніторі поведінка інша.
        int color_depth_bits = 8;
        ColorFormat color_format = ColorFormat::RGB;

        // Забрати DRM master. Потрібне для зміни режиму й ексклюзивне:
        // якщо його тримає інший процес (lightdm, друга копія), відкриття
        // провалиться з поясненням у лозі.
        bool become_master = true;
    };

    // Два конструктори замість одного з типовим аргументом: усередині
    // оголошення класу Config ще неповний, і GCC на "= {}" спотикається.
    KmsDisplayManager();
    explicit KmsDisplayManager(Config cfg);
    ~KmsDisplayManager() override;

    bool open() override;
    void close() override;
    bool is_open() const override;

    Layer& layer() override;

    bool present() override;
    void set_present_callback(PresentCallback cb) override;

    PresentStats stats() const override;
    const std::string& description() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vrx::display
