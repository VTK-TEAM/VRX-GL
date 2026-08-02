#include "license/sd_identity.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace vrx::license {
namespace {

// Префікс доменної відокремленості. Підписуємо не голі байти картки, а
// їх разом із цим тегом — щоб підпис від цієї схеми не можна було
// переплутати чи перевикористати в жодній іншій. Версія в кінці: якщо
// колись зміниться склад повідомлення, старі ліцензії просто перестануть
// проходити, а не почнуть означати щось інше.
constexpr char kDomainPrefix[] = "VRXGL-LIC-v1";

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    // прибрати кінцеві пробіли/переноси
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

bool hex_nibble(char c, uint8_t* out) {
    if (c >= '0' && c <= '9') { *out = c - '0'; return true; }
    c = std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') { *out = 10 + (c - 'a'); return true; }
    return false;
}

// "9f5449..." (32 hex) -> 16 байт. false, якщо довжина не та або не hex.
bool parse_hex(const std::string& hex, uint8_t* out, size_t out_len) {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        uint8_t hi, lo;
        if (!hex_nibble(hex[2 * i], &hi) || !hex_nibble(hex[2 * i + 1], &lo)) return false;
        out[i] = (hi << 4) | lo;
    }
    return true;
}

// Знайти пристрій-диск, на якому лежить корінь. Повертає "/sys/block/mmcblkN"
// або порожньо, якщо корінь не на mmc.
std::string find_boot_mmc_sysfs() {
    struct stat st{};
    if (stat("/", &st) != 0) return {};

    char link[64];
    std::snprintf(link, sizeof(link), "/sys/dev/block/%u:%u",
                  major(st.st_dev), minor(st.st_dev));

    char target[512];
    ssize_t n = ::readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return {};
    target[n] = '\0';

    // target виглядає як ".../block/mmcblk1/mmcblk1p1" (розділ) або
    // ".../block/mmcblk1" (цілий диск). Витягаємо ім'я вузла після "block/".
    std::string path(target);
    const std::string marker = "/block/";
    const size_t pos = path.find(marker);
    if (pos == std::string::npos) return {};
    std::string rest = path.substr(pos + marker.size());   // "mmcblk1/mmcblk1p1" або "mmcblk1"

    const size_t slash = rest.find('/');
    std::string disk = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    if (disk.compare(0, 6, "mmcblk") != 0) return {};        // корінь не на mmc

    return "/sys/block/" + disk;
}

} // namespace

std::vector<uint8_t> SdIdentity::license_message() const {
    std::vector<uint8_t> msg;
    // префікс без завершального '\0'
    for (const char* p = kDomainPrefix; *p; ++p) msg.push_back((uint8_t)*p);
    msg.push_back(manufacturer_id);
    for (char c : product_name) msg.push_back((uint8_t)c);
    // serial у мережевому порядку (старший байт першим) — фіксуємо порядок,
    // щоб hex у -sdinfo збігався з тим, що підписує вендор, незалежно від
    // порядку байтів машини.
    msg.push_back((uint8_t)((serial >> 24) & 0xff));
    msg.push_back((uint8_t)((serial >> 16) & 0xff));
    msg.push_back((uint8_t)((serial >> 8) & 0xff));
    msg.push_back((uint8_t)(serial & 0xff));
    return msg;
}

bool SdIdentity::serial_looks_fake() const {
    return serial == 0x00000000u || serial == 0xFFFFFFFFu;
}

std::string SdIdentity::describe() const {
    std::ostringstream os;
    if (!present) { os << "картку не знайдено (корінь не на мікроСД?)"; return os.str(); }
    os << "CID=" << cid_hex
       << " manfid=0x" << std::hex << (int)manufacturer_id
       << " name=" << product_name
       << " serial=0x";
    char b[16];
    std::snprintf(b, sizeof(b), "%08x", serial);
    os << b;
    if (!consistent) os << " [УВАГА: сирий CID не збігся з полями /sys]";
    if (serial_looks_fake()) os << " [serial схожий на підробку]";
    return os.str();
}

SdIdentity read_sd_identity() {
    SdIdentity id;

    id.sysfs_path = find_boot_mmc_sysfs();
    if (id.sysfs_path.empty()) return id;   // present=false

    const std::string cid = read_first_line(id.sysfs_path + "/device/cid");
    uint8_t raw[16];
    if (!parse_hex(cid, raw, 16)) return id; // present=false

    id.present = true;
    id.cid_hex = cid;

    // Розкладка CID (SD Association):
    //   [0]      manufacturer id
    //   [1..2]   oem id
    //   [3..7]   product name (5 ASCII)
    //   [8]      product revision
    //   [9..12]  serial number (32-біт)
    //   [13..14] manufacturing date
    //   [15]     crc
    id.manufacturer_id = raw[0];
    id.oem_id = (uint16_t)((raw[1] << 8) | raw[2]);
    for (int i = 3; i <= 7; ++i) {
        if (raw[i] >= 0x20 && raw[i] < 0x7f) id.product_name.push_back((char)raw[i]);
    }
    id.product_revision = raw[8];
    id.serial = ((uint32_t)raw[9] << 24) | ((uint32_t)raw[10] << 16) |
                ((uint32_t)raw[11] << 8) | (uint32_t)raw[12];
    id.mfg_date_raw = (uint16_t)((raw[13] << 8) | raw[14]);

    // Звірка з розібраними полями ядра. Ядро друкує manfid/serial як
    // "0x....", name — як ASCII. Будь-яка розбіжність — сигнал, що CID
    // підмінено (present лишаємо true, але consistent=false).
    auto read_hex_attr = [&](const char* a) -> long {
        std::string s = read_first_line(id.sysfs_path + "/device/" + a);
        if (s.rfind("0x", 0) == 0) return std::strtol(s.c_str(), nullptr, 16);
        if (!s.empty()) return std::strtol(s.c_str(), nullptr, 16);
        return -1;
    };
    const long k_manfid = read_hex_attr("manfid");
    const long k_serial = read_hex_attr("serial");
    const std::string k_name = read_first_line(id.sysfs_path + "/device/name");

    if (k_manfid >= 0 && (long)id.manufacturer_id != k_manfid) id.consistent = false;
    if (k_serial >= 0 && (long)id.serial != k_serial) id.consistent = false;
    if (!k_name.empty() && k_name != id.product_name) id.consistent = false;

    return id;
}

} // namespace vrx::license
