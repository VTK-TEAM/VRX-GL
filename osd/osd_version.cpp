// osd_version.cpp — єдине місце, що знає рядок версії збірки.
//
// version.h генерується scripts/gen_version.sh у каталог збірки на КОЖЕН
// білд (лічильник++), тож включати його в osd.cpp означало б щоразу
// перекомпільовувати весь OSD. Замість цього версію тримає цей крихітний
// файл: щобілд перекомпілюється лише він, а osd.cpp бере рядок через
// оголошення нижче.
#include "version.h"

namespace vrx::osd {

const char* build_version() { return VRX_VERSION; }

} // namespace vrx::osd
