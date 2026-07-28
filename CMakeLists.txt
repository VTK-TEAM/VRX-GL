cmake_minimum_required(VERSION 3.16)
project(VRX_GL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # для clangd/IntelliSense

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

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
    display/kms_display_manager.cpp
    source/video_source.cpp
    render/gl_renderer.cpp
    control/camera_api.cpp
    control/phase_controller.cpp
    diag/link_monitor.cpp
    record/storage.cpp
    record/recorder.cpp
)

target_include_directories(vrx_gl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
