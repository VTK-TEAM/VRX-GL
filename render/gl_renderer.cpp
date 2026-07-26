#include "gl_renderer.hpp"
#include "gl_quad.hpp"

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <algorithm>
#include <thread>
#include <vector>

namespace vrx::render {
namespace {

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

PFNEGLCREATEIMAGEKHRPROC                            eglCreateImageKHR_ = nullptr;
PFNEGLDESTROYIMAGEKHRPROC                           eglDestroyImageKHR_ = nullptr;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC       glEGLImageTargetRenderbufferStorageOES_ = nullptr;
PFNEGLCREATESYNCKHRPROC                             eglCreateSyncKHR_ = nullptr;
PFNEGLDESTROYSYNCKHRPROC                            eglDestroySyncKHR_ = nullptr;
PFNEGLCLIENTWAITSYNCKHRPROC                         eglClientWaitSyncKHR_ = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC                 glEGLImageTargetTexture2DOES_ = nullptr;

} // namespace

// ---------------------------------------------------------------------

struct GlRenderer::Impl {
    Config cfg;
    display::DisplayManager* dpy = nullptr;
    display::LayerInfo info{};

    int drm_fd = -1;
    struct gbm_device* gbm = nullptr;

    EGLDisplay egl = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig  egl_cfg = nullptr;
    bool have_fence = false;

    // Один буфер кільця: пам'ять + як її бачить GL + як її бачить дисплей.
    struct Buffer {
        struct gbm_bo* bo = nullptr;
        int fd = -1;
        uint32_t stride = 0;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint rb = 0;          // renderbuffer поверх тієї ж пам'яті
        GLuint fbo = 0;

        // Маркер зайнятості: копія лежить у Frame::keepalive, тож поки
        // дисплей показує кадр, лічильник > 1. Це і є "чи можна малювати".
        std::shared_ptr<int> busy = std::make_shared<int>(0);
        bool free_now() const { return busy.use_count() == 1; }
    };
    std::vector<Buffer> bufs;

    std::thread thread;
    std::atomic<bool> running{false};

    // Прокидання на звільнення буфера. Сигналить present-колбек дисплея,
    // тобто цикл спить рівно доти, доки нема куди малювати.
    std::mutex wake_mtx;
    std::condition_variable wake_cv;
    bool woken = false;

    mutable std::mutex stats_mtx;
    RenderStats st{};

    // Зареєстровані джерела. Список беремо зрізом на кожен кадр, щоб
    // додавання/видалення на ходу не вимагало тримати лок під час
    // малювання.
    mutable std::mutex src_mtx;
    std::vector<std::shared_ptr<source::FrameSource>> sources;

    // Дві програми, бо sampler2D і samplerExternalOES — різні типи GLSL
    // і в одну не компілюються. Перемикань за кадр буде одне, не по
    // одному на прямокутник: відео знизу, OSD зверху, тож групуються самі.
    GLuint prog_ext = 0;
    GLuint vbo = 0;
    DmabufTextureCache textures;

    // --- налаштування EGL ---

    bool setup_egl() {
        auto get_dpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
        egl = get_dpy ? get_dpy(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                      : eglGetDisplay((EGLNativeDisplayType)gbm);
        if (egl == EGL_NO_DISPLAY) {
            std::fprintf(stderr, "[gl] eglGetPlatformDisplay провалився\n");
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(egl, &major, &minor)) {
            std::fprintf(stderr, "[gl] eglInitialize провалився\n");
            return false;
        }

        const char* ext = eglQueryString(egl, EGL_EXTENSIONS);
        bool dmabuf = ext && std::strstr(ext, "EGL_EXT_image_dma_buf_import");
        have_fence  = ext && std::strstr(ext, "EGL_KHR_fence_sync");
        if (!dmabuf) {
            // Без цього вся архітектура не працює: ні вивести буфер у
            // дисплей, ні потім прийняти кадр від декодера без копії.
            std::fprintf(stderr,
                "[gl] немає EGL_EXT_image_dma_buf_import — zero-copy неможливий\n");
            return false;
        }

        eglBindAPI(EGL_OPENGL_ES_API);

        EGLint cfg_attr[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
            EGL_NONE
        };
        EGLint n = 0;
        if (!eglChooseConfig(egl, cfg_attr, &egl_cfg, 1, &n) || n < 1) {
            std::fprintf(stderr, "[gl] eglChooseConfig провалився\n");
            return false;
        }

        EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        ctx = eglCreateContext(egl, egl_cfg, EGL_NO_CONTEXT, ctx_attr);
        if (ctx == EGL_NO_CONTEXT) {
            std::fprintf(stderr, "[gl] eglCreateContext провалився\n");
            return false;
        }

        eglCreateImageKHR_  = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        glEGLImageTargetRenderbufferStorageOES_ =
            (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
            eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
        glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
        eglCreateSyncKHR_     = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
        eglDestroySyncKHR_    = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        eglClientWaitSyncKHR_ = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");

        if (!eglCreateImageKHR_ || !glEGLImageTargetRenderbufferStorageOES_) {
            std::fprintf(stderr, "[gl] бракує EGLImage-функцій\n");
            return false;
        }
        return true;
    }

    // --- буфери (потребують поточного контексту) ---

    bool create_buffers() {
        bufs.resize(cfg.buffers);
        for (int i = 0; i < cfg.buffers; ++i) {
            Buffer& b = bufs[i];

            // SCANOUT обов'язковий: без нього буфер намалюється, але
            // показати його не вийде — у нього інші вимоги до
            // вирівнювання й розміщення.
            b.bo = gbm_bo_create(gbm, info.width, info.height, info.fourcc,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
            if (!b.bo) {
                std::fprintf(stderr,
                    "[gl] gbm_bo_create(%dx%d, 0x%08x, SCANOUT|RENDERING) провалився\n",
                    info.width, info.height, info.fourcc);
                return false;
            }
            b.fd = gbm_bo_get_fd(b.bo);
            b.stride = gbm_bo_get_stride(b.bo);

            EGLint attr[] = {
                EGL_WIDTH, info.width,
                EGL_HEIGHT, info.height,
                EGL_LINUX_DRM_FOURCC_EXT, (EGLint)info.fourcc,
                EGL_DMA_BUF_PLANE0_FD_EXT, b.fd,
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)b.stride,
                EGL_NONE
            };
            b.image = eglCreateImageKHR_(egl, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
            if (b.image == EGL_NO_IMAGE_KHR) {
                std::fprintf(stderr, "[gl] eglCreateImageKHR для буфера %d провалився\n", i);
                return false;
            }

            // Renderbuffer поверх ТІЄЇ САМОЇ пам'яті — після цього
            // glClear/glDraw пишуть прямо в буфер, який покаже дисплей.
            glGenRenderbuffers(1, &b.rb);
            glBindRenderbuffer(GL_RENDERBUFFER, b.rb);
            glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, b.image);

            glGenFramebuffers(1, &b.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_RENDERBUFFER, b.rb);
            GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (s != GL_FRAMEBUFFER_COMPLETE) {
                std::fprintf(stderr, "[gl] FBO %d неповний (0x%04x)\n", i, s);
                return false;
            }
        }
        return true;
    }

    void destroy_buffers() {
        for (Buffer& b : bufs) {
            if (b.fbo) glDeleteFramebuffers(1, &b.fbo);
            if (b.rb) glDeleteRenderbuffers(1, &b.rb);
            if (b.image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_) {
                eglDestroyImageKHR_(egl, b.image);
            }
            if (b.fd >= 0) ::close(b.fd);
            if (b.bo) gbm_bo_destroy(b.bo);
        }
        bufs.clear();
    }

    display::Frame frame_for(const Buffer& b) const {
        display::Frame f;
        f.fourcc = info.fourcc;
        f.modifier = DRM_FORMAT_MOD_LINEAR;
        f.width = info.width;
        f.height = info.height;
        f.n_planes = 1;
        f.fd[0] = b.fd;
        f.stride[0] = b.stride;
        f.offset[0] = 0;
        f.keepalive = b.busy;
        return f;
    }

    // Створюється в робочому потоці: GL-об'єкти потребують поточного
    // контексту.
    bool create_gl_objects() {
        prog_ext = link_program(kQuadVS, kExternalFS);
        if (!prog_ext) return false;
        glGenBuffers(1, &vbo);
        textures.init(egl, eglCreateImageKHR_, eglDestroyImageKHR_,
                      glEGLImageTargetTexture2DOES_);
        return true;
    }

    void destroy_gl_objects() {
        textures.clear();
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (prog_ext) { glDeleteProgram(prog_ext); prog_ext = 0; }
    }

    // Малює один кадр: тло плюс усі джерела, що дали кадр.
    void draw_frame() {
        // Чистимо ЗАВЖДИ. Під кадром, що не покрив увесь екран, інакше
        // лишиться вміст ПОЗАминулого кадру — буферів два, вони
        // чергуються. Плюс на тайловому GPU (Mali) очищення на початку
        // проходу дозволяє не завантажувати попередній вміст у тайлову
        // пам'ять, тобто виходить швидше, ніж без нього.
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::vector<std::shared_ptr<source::FrameSource>> snap;
        {
            std::lock_guard<std::mutex> lk(src_mtx);
            snap = sources;
        }
        if (snap.empty()) return;

        const float screen_aspect = float(info.width) / float(info.height);

        struct Item {
            layout::Placement p;
            source::SourceFrame f;
        };
        std::vector<Item> items;
        items.reserve(snap.size());

        for (auto& src : snap) {
            source::SourceFrame f;
            // Джерела немає або сигналу немає — не малюємо нічого.
            // Програма одна, а дрони різні: заглушка припускала б знання
            // про конкретну конфігурацію, якого в програми немає.
            if (!src->acquire(f)) continue;
            if (!f.valid() || !f.where.enabled) continue;
            if (f.image.fd[0] < 0) continue;      // кадру як буфера немає

            const float a = f.aspect();
            if (a <= 0.0f) continue;

            items.push_back({layout::fit_source(f.where, a, screen_aspect), f});
        }
        if (items.empty()) return;

        // Порядок за z: менше — далі. Джерел одиниці, тож stable_sort
        // дешевший за будь-яку хитрість і зберігає порядок реєстрації
        // для однакових z.
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.p.z < b.p.z; });

        glUseProgram(prog_ext);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(prog_ext, "uTex"), 0);

        for (const Item& it : items) {
            // Розмір звіряється ЩОКАДРУ, а не за таймером: це кілька
            // порівнянь усередині кеша, тоді як раз на секунду означало б
            // до 60 кадрів із неправильною геометрією. Привід реальний —
            // електронна стабілізація ріже роздільність кропом на ходу.
            GLuint tex = textures.get(it.f.image);
            if (!tex) continue;

            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            draw_textured_quad(vbo, it.p, it.f.image.visible(),
                               it.f.image.width, it.f.image.height);
        }
    }

    void wake() {
        {
            std::lock_guard<std::mutex> lk(wake_mtx);
            woken = true;
        }
        wake_cv.notify_one();
    }

    int wait_free_buffer() {
        for (;;) {
            for (size_t i = 0; i < bufs.size(); ++i) {
                if (bufs[i].free_now()) return (int)i;
            }
            if (!running.load(std::memory_order_relaxed)) return -1;

            {
                std::lock_guard<std::mutex> lk(stats_mtx);
                st.stalls++;
            }
            std::unique_lock<std::mutex> lk(wake_mtx);
            // Таймаут — страховка: якщо підтвердження flip'а раптом не
            // прийде, цикл не повисне назавжди, а перевірить сам.
            wake_cv.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return woken || !running.load(); });
            woken = false;
        }
    }

    void loop() {
        if (!eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            std::fprintf(stderr, "[gl] eglMakeCurrent у робочому потоці провалився\n");
            running.store(false);
            return;
        }

        std::fprintf(stderr, "[gl] %s | %s\n",
                     (const char*)glGetString(GL_RENDERER),
                     (const char*)glGetString(GL_VERSION));

        if (!create_buffers() || !create_gl_objects()) {
            running.store(false);
            return;
        }
        std::fprintf(stderr, "[gl] %d буфер(и) %dx%d, крок %u | fence: %s\n",
                     (int)bufs.size(), info.width, info.height,
                     bufs.empty() ? 0 : bufs[0].stride, have_fence ? "є" : "немає");

        glViewport(0, 0, info.width, info.height);
        glDisable(GL_DEPTH_TEST);

        int frame_no = 0;
        double avg = 0.0;

        while (running.load(std::memory_order_relaxed)) {
            int idx = wait_free_buffer();
            if (idx < 0) break;

            Buffer& b = bufs[idx];
            double t0 = now_ms();

            glBindFramebuffer(GL_FRAMEBUFFER, b.fbo);
            draw_frame();
            frame_no++;

            // Чекаємо РЕАЛЬНОГО завершення GPU. Без цього на екран поїде
            // недомальований буфер: команди лише поставлені в чергу.
            // IN_FENCE_FD на цьому ядрі немає, тож чекаємо тут, у своєму
            // потоці — потік показу це не гальмує.
            if (have_fence && eglCreateSyncKHR_) {
                EGLSyncKHR s = eglCreateSyncKHR_(egl, EGL_SYNC_FENCE_KHR, nullptr);
                glFlush();
                eglClientWaitSyncKHR_(egl, s, 0, EGL_FOREVER_KHR);
                eglDestroySyncKHR_(egl, s);
            } else {
                glFinish();
            }

            double dt = now_ms() - t0;
            avg = avg == 0.0 ? dt : (avg * 0.95 + dt * 0.05);

            if (dpy->layer().submit(frame_for(b))) {
                dpy->present();
                std::lock_guard<std::mutex> lk(stats_mtx);
                st.frames++;
                st.last_draw_ms = dt;
                st.avg_draw_ms = avg;
            }
        }

        destroy_gl_objects();
        destroy_buffers();
        eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
};

// ---------------------------------------------------------------------

GlRenderer::GlRenderer() : impl_(std::make_unique<Impl>()) {}
GlRenderer::~GlRenderer() { stop(); }

bool GlRenderer::init(display::DisplayManager& display) {
    return init(display, Config{});
}

bool GlRenderer::init(display::DisplayManager& display, Config cfg) {
    Impl& d = *impl_;
    if (!display.is_open()) {
        std::fprintf(stderr, "[gl] дисплей не відкритий — нема з чого брати формат\n");
        return false;
    }
    d.cfg = std::move(cfg);
    d.dpy = &display;
    d.info = display.layer().info();

    d.drm_fd = ::open(d.cfg.card.c_str(), O_RDWR | O_CLOEXEC);
    if (d.drm_fd < 0) {
        std::fprintf(stderr, "[gl] open(%s): %s\n",
                     d.cfg.card.c_str(), std::strerror(errno));
        return false;
    }
    d.gbm = gbm_create_device(d.drm_fd);
    if (!d.gbm) {
        std::fprintf(stderr, "[gl] gbm_create_device провалився\n");
        return false;
    }
    if (!d.setup_egl()) return false;

    // Прокидаємось на підтвердженні показу: саме тоді звільняється буфер.
    // Опитування тут було б і марною роботою, і зайвою затримкою.
    display.set_present_callback([&d](int64_t) { d.wake(); });

    std::fprintf(stderr, "[gl] init: %dx%d fourcc=0x%08x, буферів %d\n",
                 d.info.width, d.info.height, d.info.fourcc, d.cfg.buffers);
    return true;
}

bool GlRenderer::start() {
    Impl& d = *impl_;
    if (d.running.load()) return true;
    if (d.egl == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "[gl] start() без успішного init()\n");
        return false;
    }
    d.running.store(true);
    d.thread = std::thread([&d] { d.loop(); });
    return true;
}

void GlRenderer::stop() {
    Impl& d = *impl_;
    if (d.running.exchange(false)) {
        d.wake();
        if (d.thread.joinable()) d.thread.join();
    }
    if (d.ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(d.egl, d.ctx);
        d.ctx = EGL_NO_CONTEXT;
    }
    if (d.egl != EGL_NO_DISPLAY) {
        eglTerminate(d.egl);
        d.egl = EGL_NO_DISPLAY;
    }
    if (d.gbm) { gbm_device_destroy(d.gbm); d.gbm = nullptr; }
    if (d.drm_fd >= 0) { ::close(d.drm_fd); d.drm_fd = -1; }
}

void GlRenderer::add_source(std::shared_ptr<source::FrameSource> src) {
    if (!src) return;
    std::lock_guard<std::mutex> lk(impl_->src_mtx);
    impl_->sources.push_back(std::move(src));
}

void GlRenderer::remove_source(const std::shared_ptr<source::FrameSource>& src) {
    std::lock_guard<std::mutex> lk(impl_->src_mtx);
    impl_->sources.erase(std::remove(impl_->sources.begin(), impl_->sources.end(), src),
                         impl_->sources.end());
}

int GlRenderer::source_count() const {
    std::lock_guard<std::mutex> lk(impl_->src_mtx);
    return (int)impl_->sources.size();
}

bool GlRenderer::is_running() const { return impl_->running.load(); }

RenderStats GlRenderer::stats() const {
    std::lock_guard<std::mutex> lk(impl_->stats_mtx);
    return impl_->st;
}

} // namespace vrx::render
