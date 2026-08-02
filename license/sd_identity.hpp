// sd_identity.hpp — читання й розбір CID мікроСД, з якої стартує станція.
//
// CID — це 16 байт (32 hex-символи), які кожна нормальна картка несе в
// собі з заводу; формат стандартизований SD Card Association. Дешеві
// підробки або віддають сміття, або діляться одним CID на всю партію —
// і те, й інше ця прив'язка відсікає.
//
// Ядро вже розбирає CID у /sys/block/mmcblkN/device/{manfid,name,serial,...}.
// Ми читаємо і сирий cid, і ці розібрані поля, та звіряємо одне з одним:
// розбіжність означає, що CID підроблено на льоту, і це вже сигнал.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrx::license {

struct SdIdentity {
    bool present = false;       // картку знайдено й cid прочитано
    bool consistent = true;     // сирий CID збігся з розібраними полями /sys
    std::string sysfs_path;     // /sys/block/mmcblkN
    std::string cid_hex;        // 32 hex, як у /sys/.../cid

    uint8_t     manufacturer_id = 0;  // CID[0:1]
    uint16_t    oem_id = 0;           // CID[1:3]
    std::string product_name;         // CID[3:8]  -> 5 ASCII ("SD64G")
    uint8_t     product_revision = 0; // CID[8]
    uint32_t    serial = 0;           // CID[9:13]
    uint16_t    mfg_date_raw = 0;     // CID[13:15] (рік/місяць у форматі CID)

    // Канонічне ПОВІДОМЛЕННЯ, яке підписує вендор і перевіряє станція:
    // префікс домену + manufacturer_id + product_name + serial. Саме воно
    // друкується `-sdinfo` як hex і саме воно передається на підпис.
    std::vector<uint8_t> license_message() const;

    // Явно "сміттєвий" serial дешевих підробок (усі нулі / усі 0xFF).
    // Такі відкидаємо ще до перевірки підпису: інакше одна активація
    // відчинила б усю партію карток, що ділять один фейковий CID.
    bool serial_looks_fake() const;

    // Читабельний рядок для людини (-sdinfo та логів).
    std::string describe() const;
};

// Знаходить картку, з якої змонтовано '/', і читає її CID. Якщо корінь не
// на мікроСД (eMMC/NVMe) або CID недоступний — present=false.
SdIdentity read_sd_identity();

} // namespace vrx::license
