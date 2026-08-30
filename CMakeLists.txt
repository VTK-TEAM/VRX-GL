cmake_minimum_required(VERSION 3.16)
project(VRX_GL CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd/IntelliSense

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# --- Ліцензія: перевірка підпису Ed25519 ---
# Станції потрібне лише verify; підпис робить вендор окремо (tools/).
# libcrypto тут уже є як залежність GStreamer/системи.
find_package(OpenSSL REQUIRED)

# --- Вивід: DRM/KMS + GBM + EGL/GLES2 ---
# Композиція шарів робиться на GPU в один буфер, який далі сканується
# одним плейном. GBM потрібен для виділення буферів, придатних і для
# рендера, і для сканування; EGL — щоб імпортувати dmabuf від декодера
# як зовнішню текстуру без копії.
pkg_check_modules(DRM    REQUIRED libdrm)
pkg_check_modules(GBM    REQUIRED gbm)
pkg_check_modules(EGL    REQUIRED egl)
pkg_check_modules(GLES2  REQUIRED glesv2)

# --- Прийом і декод: GStreamer ---
pkg_check_modules(GST REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-allocators-1.0
)

add_executable(vrx_gl
    main.cpp
    display/display.cpp
    source/video_source.cpp
    render/gl_renderer.cpp
    control/camera_api.cpp
    control/phase_controller.cpp
    ui/pointer.cpp
    ui/screen_ui.cpp
    ui/screen_presets.cpp
    diag/link_monitor.cpp
    record/storage.cpp
    record/mkv_seek.cpp
    record/recorder.cpp
    osd/osd.cpp
    osd/osd_version.cpp
    osd/subtitle_writer.cpp
    osd/local_channels.cpp
    osd/stb_image_impl.cpp
    osd/telemetry/vt_telemetry_listener.cpp
    license/sd_identity.cpp
    license/ed25519.cpp
    license/license_store.cpp
    license/license_gate.cpp
)

target_include_directories(vrx_gl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}          # тут лежить згенерований version.h
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
)

target_link_libraries(vrx_gl PRIVATE
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${GST_LIBRARIES}
    OpenSSL::Crypto
    Threads::Threads
)

target_link_directories(vrx_gl PRIVATE
    ${DRM_LIBRARY_DIRS}
    ${GBM_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLES2_LIBRARY_DIRS}
    ${GST_LIBRARY_DIRS}
)

target_compile_options(vrx_gl PRIVATE
    -Wall -Wextra
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${EGL_CFLAGS_OTHER}
    ${GLES2_CFLAGS_OTHER}
    ${GST_CFLAGS_OTHER}
)

# РЕЖИМ ОДНОГО КАНАЛУ — компіляційний, а не рантаймний. Той самий бінарник
# vrx_gl, лише обрізаний: вторинні канали (PiP, захват), їхні рекордери й
# система екранів-пресетів НЕ збираються й увімкнути їх нема як. Це збірка
# під простий приймач — конфігурується один раз на девайс, і оновлення
# лишається таким же, поки не переконфігурувати.
#
#   cmake -B build -DVRX_SINGLE_CHANNEL=ON      # однканальний девайс
#   cmake -B build -DVRX_SINGLE_CHANNEL=OFF     # назад у повний
#
# Оголошено ТУТ, до цілі версії: генератор дописує 'S' у кінець версії під
# однканальну збірку (V01.2767.0003S), тож мусить знати режим.
option(VRX_SINGLE_CHANNEL "Однканальний режим (лише основний потік)" OFF)

# ВЕРСІЯ ЗБІРКИ — Vyy.xxxx.bbbb(S), генерується на КОЖЕН білд.
#   yy   стала (version/major.txt), xxxx днів від 2019-01-05,
#   bbbb лічильник білдів (version/build_counter.txt, ++ щобілд),
#   S    лише під VRX_SINGLE_CHANNEL — однканальна збірка.
# Пише build/version.h (для бінарника) і version/version.txt (для
# оновлювача). Ціль ALL і без OUTPUT — тобто завжди «застаріла» й біжить
# щобілд; add_dependencies гарантує, що вона відпрацює до компіляції
# osd_version.cpp, єдиного, хто version.h включає.
add_custom_target(vrx_version ALL
    BYPRODUCTS ${CMAKE_BINARY_DIR}/version.h
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/gen_version.sh
            ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/version.h $<BOOL:${VRX_SINGLE_CHANNEL}>
    COMMENT "Версія збірки (Vyy.xxxx.bbbb)"
    VERBATIM)
add_dependencies(vrx_gl vrx_version)

if(VRX_SINGLE_CHANNEL)
    target_compile_definitions(vrx_gl PRIVATE VRX_SINGLE_CHANNEL=1)
    message(STATUS "VRX: ОДНОКАНАЛЬНИЙ режим збірки — лише основний потік")
endif()

# Наглядач: вирішує, що зараз на екрані — станція чи редактор.
# Екрана сам не тримає, тож може бути єдиним, що стоїть в
# автозапуску. Протокол зі станцією — код виходу.
add_executable(vrx_supervisor supervisor/supervisor.c)

# Релей захвату: окрема автономна програма. Володіє USB-камерою (V4L2) і
# жене MJPEG бродкастом; станція читає його як звичайну мережеву камеру.
# Чистий V4L2 + сокети, без жодних залежностей проєкту.
add_executable(uvc_relay relay/uvc_relay.c)
# ---------------------------------------------------------------------
# Редактор розкладки OSD
#
# Переніс із VRX як є. Він працює З ФАЙЛАМИ ОСНОВНОГО ПРОЄКТУ —
# osd_config.json, atlas.png, osd_glyph_info.bin, osd_icon_names.json, — і
# читає ті самі заголовки телеметрії, що й станція. Саме тому він тут, а
# не окремим репозиторієм: розійтись їм не можна.
#
# Збирається окремою ціллю: станції SDL не потрібен, а редактору не
# потрібні ні DRM, ні GStreamer.
pkg_check_modules(SDL2 sdl2)
pkg_check_modules(SDL2_IMAGE SDL2_image)
pkg_check_modules(SDL2_TTF SDL2_ttf)

if(SDL2_FOUND AND SDL2_IMAGE_FOUND AND SDL2_TTF_FOUND)
    add_executable(osd_editor
        osd_editor/src/main.cpp
        osd_editor/src/editor_app.cpp
        osd_editor/src/edit_dialog.cpp
        osd_editor/src/widgets.cpp
        osd_editor/src/canvas_renderer.cpp
        osd/telemetry/vt_telemetry_listener.cpp
    )
    target_include_directories(osd_editor PRIVATE
        ${CMAKE_SOURCE_DIR}/osd/telemetry
        ${CMAKE_SOURCE_DIR}/osd
        ${SDL2_INCLUDE_DIRS}
        ${SDL2_IMAGE_INCLUDE_DIRS}
        ${SDL2_TTF_INCLUDE_DIRS}
    )
    target_link_libraries(osd_editor PRIVATE
        ${SDL2_LIBRARIES}
        ${SDL2_IMAGE_LIBRARIES}
        ${SDL2_TTF_LIBRARIES}
        Threads::Threads
    )
    target_compile_options(osd_editor PRIVATE -Wall -Wextra)
else()
    message(STATUS "SDL2 не знайдено — osd_editor не збирається (станції він не потрібен)")
endif()
