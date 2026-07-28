// stb_image_impl.cpp
// Єдине місце в бінарнику VRX, де реально компілюється тіло stb_image
// (STB_IMAGE_IMPLEMENTATION можна визначити рівно ОДИН раз на весь
// бінарник, інакше duplicate symbol на лінку). osd_renderer.cpp просто
// #include "stb_image.h" без define — використовує символи звідси.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
