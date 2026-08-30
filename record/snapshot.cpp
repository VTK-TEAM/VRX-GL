#include "record/snapshot.hpp"
#include "source/frame_source.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <drm_fourcc.h>

#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

namespace vrx::record {
namespace {

// DRM-код у назву формату GStreamer. Список короткий навмисно: це рівно
// те, що віддають наші декодери, і мовчазна підтримка чогось іще тут
// шкідливіша за чесну відмову.
const char* gst_format_of(uint32_t fourcc) {
    switch (fourcc) {
        case DRM_FORMAT_NV12:   return "NV12";
        case DRM_FORMAT_NV16:   return "NV16";
        case DRM_FORMAT_YUV420: return "I420";
        case DRM_FORMAT_YUYV:   return "YUY2";
        default: return nullptr;
    }
}

} // namespace

bool save_jpeg(const source::SourceFrame& f, const std::string& path, int quality) {
    const display::Frame& im = f.image;
    if (!f.valid() || im.fd[0] < 0) return false;

    const char* fmt = gst_format_of(im.fourcc);
    if (!fmt) {
        std::fprintf(stderr, "[знімок] невідомий формат кадру 0x%08x\n", im.fourcc);
        return false;
    }

    // Розмір беремо в самого dmabuf, а не рахуємо з кроків: вирівнювання
    // декодера тут своє, і будь-яка наша формула рано чи пізно розійдеться
    // з дійсністю.
    const off_t size = ::lseek(im.fd[0], 0, SEEK_END);
    if (size <= 0) return false;

    void* map = ::mmap(nullptr, (size_t)size, PROT_READ, MAP_SHARED, im.fd[0], 0);
    if (map == MAP_FAILED) {
        std::fprintf(stderr, "[знімок] не змапував кадр\n");
        return false;
    }

    char desc[512];
    std::snprintf(desc, sizeof(desc),
        "appsrc name=src ! videoconvert ! jpegenc quality=%d ! filesink location=\"%s\"",
        quality, path.c_str());

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe) {
        std::fprintf(stderr, "[знімок] пайплайн не зібрався: %s\n",
                     err && err->message ? err->message : "?");
        if (err) g_error_free(err);
        ::munmap(map, (size_t)size);
        return false;
    }

    GstElement* src = gst_bin_get_by_name(GST_BIN(pipe), "src");
    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, fmt,
        "width", G_TYPE_INT, im.width,
        "height", G_TYPE_INT, im.height,
        "framerate", GST_TYPE_FRACTION, 1, 1, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);

    // Буфер лише ОБГОРТАЄ відображену пам'ять — копії немає. Живий він
    // рівно до кінця цієї функції, а далі ми його самі й відпускаємо.
    GstBuffer* buf = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY, map, (size_t)size, 0, (size_t)size, nullptr, nullptr);

    // Кроки й зсуви площин — з кадру. Порахувати їх самим не можна:
    // stride майже ніколи не дорівнює ширині.
    GstVideoInfo vi;
    gst_video_info_set_format(&vi, gst_video_format_from_string(fmt), im.width, im.height);
    gsize offs[GST_VIDEO_MAX_PLANES] = {};
    gint strd[GST_VIDEO_MAX_PLANES] = {};
    for (int i = 0; i < im.n_planes && i < GST_VIDEO_MAX_PLANES; ++i) {
        offs[i] = im.offset[i];
        strd[i] = (gint)im.stride[i];
    }
    gst_buffer_add_video_meta_full(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                   GST_VIDEO_INFO_FORMAT(&vi), im.width, im.height,
                                   (guint)im.n_planes, offs, strd);

    gst_element_set_state(pipe, GST_STATE_PLAYING);
    const bool pushed = gst_app_src_push_buffer(GST_APP_SRC(src), buf) == GST_FLOW_OK;
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    // Чекаємо саме на кінець потоку: без цього filesink може не встигнути
    // дописати, і на носії лишиться обрізаний JPEG.
    bool ok = pushed;
    if (GstBus* bus = gst_element_get_bus(pipe)) {
        GstMessage* m = gst_bus_timed_pop_filtered(
            bus, 5 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!m || GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) ok = false;
        if (m) gst_message_unref(m);
        gst_object_unref(bus);
    }

    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(src);
    gst_caps_unref(caps);
    gst_object_unref(pipe);
    ::munmap(map, (size_t)size);
    return ok;
}

} // namespace vrx::record

namespace vrx::record {

int save_set(std::vector<std::pair<std::string, source::SourceFrame>> frames,
             const std::string& dir, int64_t wall_us) {
    if (frames.empty()) return 0;
    ::mkdir(dir.c_str(), 0755);

    const time_t sec = (time_t)(wall_us / 1000000);
    struct tm tm {};
    localtime_r(&sec, &tm);
    char stamp[40];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    const int ms = (int)(wall_us / 1000 % 1000);

    for (auto& f : frames) {
        char path[256];
        std::snprintf(path, sizeof(path), "%s/%s_%03d_%s.jpg",
                      dir.c_str(), stamp, ms, f.first.c_str());
        f.first = path;
    }

    // Кодування — десятки мілісекунд на кадр, тож окремий потік. Кадри
    // тримають свої буфери живими самі, тож віддати їх туди безпечно.
    const int n = (int)frames.size();
    std::thread([frames = std::move(frames)]() mutable {
        for (auto& f : frames) {
            const bool ok = save_jpeg(f.second, f.first);
            std::fprintf(stderr, "[знімок] %s%s\n", f.first.c_str(),
                         ok ? "" : " — НЕ ЗБЕРЕГЛОСЬ");
        }
    }).detach();
    return n;
}

} // namespace vrx::record
