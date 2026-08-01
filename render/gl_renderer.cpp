#include "gl_renderer.hpp"
#include "gl_quad.hpp"
#include "phase_meter.hpp"
#include "../diag/path_meter.hpp"

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <atomic>
#include <chrono>
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

int64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
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
    display::Display* dpy = nullptr;
    display::OutputState info{};

    struct gbm_device* gbm = nullptr;   // належить дисплею, ми лише беремо для EGL

    EGLDisplay egl = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLConfig  egl_cfg = nullptr;
    bool have_fence = false;

    // GL-ОБГОРТКА НАД БУФЕРОМ ДИСПЛЕЯ.
    //
    // Самими буферами володіє дисплей — він знає вимоги до сканування і
    // він же єдиний знає, який із них зараз читає сканер. Але писати в
    // них має GL, а для цього над тим самим dmabuf потрібні EGLImage і
    // FBO, які можна створити лише в потоці з поточним контекстом, тобто
    // тут.
    //
    // Це КЕШ, а не джерело правди: заповнюється на перших кадрах і
    // застигає, ключ — дескриптор dmabuf. Правда про стан буферів
    // лишається одна, у дисплея.
    struct GlTarget {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint rb = 0;          // renderbuffer поверх тієї ж пам'яті
        GLuint fbo = 0;
    };
    std::map<int, GlTarget> gl_targets;

    std::thread thread;
    std::atomic<bool> running{false};

    // Прокидання на звільнення буфера. Сигналить present-колбек дисплея,
    // тобто цикл спить рівно доти, доки нема куди малювати.
    std::mutex wake_mtx;
    std::condition_variable wake_cv;
    bool woken = false;

    // Час підтвердження останнього показу і період розгортки — з них
    // рахується момент, коли питати джерела.
    std::atomic<int64_t> last_present_ns{0};
    int64_t period_ns = 0;

    // produced_ns кадру, який щойно віддали на показ. Наступне
    // підтвердження flip'а стосується саме його — звідси й затримка.
    std::atomic<int64_t> inflight_produced_ns{0};
    int64_t prev_pres_produced = 0;   // лише з потоку подій DRM
    int64_t last_late_produced = 0;   // лише з потоку рендерера

    // Адаптивний зсув опиту від початку періоду, мс.
    //
    // Фіксований момент опиту не працює в принципі: джерело 60.00, показ
    // 59.789, тож фаза приходу кадру вільно пливе по всьому періоду
    // (повний оберт ~4.7 с). Коли прихід трохи раніше опиту — все добре,
    // коли трохи пізніше — повтор і дроп.
    //
    // Тому зсув СЛІДУЄ за фазою, керуючись спостережуваним сигналом:
    // стався повтор -> опитали зарано, посуваємось пізніше; повторів
    // немає -> повільно повертаємось до меншої затримки.
    //
    // Теоретична межа так — 0.21 пропуску/с (сама різниця частот), бо
    // раз на оберт фази прихід неминуче потрапляє в зону, куди опит уже
    // не встигає (там треба ще звести й закомітити).
    double poll_offset_ms = 8.0;
    int64_t last_shown_produced_ns = 0;

    // Вимір фази — окремим модулем: до малювання він стосунку не має,
    // а власного стану має на три механізми (кругове середнє, регресія
    // дрейфу, згладжений розкид).
    PhaseMeter phase;

    // Штучна затримка утримання цілі, лише для перевірки гарячої заміни.
    int test_hold_ms = getenv("VRX_TEST_HOLD_MS") ? atoi(getenv("VRX_TEST_HOLD_MS")) : 0;

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

    // Друга програма — для звичайних текстур (атлас OSD, картинки барів
    // і горизонту). Окрема, бо sampler2D і samplerExternalOES це різні
    // типи GLSL і в одну програму не компілюються.
    GLuint prog_tex = 0;

    // Зареєстровані оверлеї. Кожен тримає власні текстури й останній
    // знімок квадів — знімок лишається між кадрами, бо оверлей оновлює
    // його рідше, ніж іде показ.
    struct OverlaySlot {
        std::shared_ptr<Overlay> ovl;
        std::vector<GLuint> textures;
        DrawList list;
        bool have = false;
    };
    mutable std::mutex ovl_mtx;
    std::vector<OverlaySlot> overlays;
    GLuint ovl_vbo = 0;
    std::vector<GLfloat> ovl_verts;
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

    // Обгортка для цього буфера: береться з кешу або створюється.
    GLuint fbo_for(const display::Target& t) {
        auto it = gl_targets.find(t.frame.fd[0]);
        if (it != gl_targets.end()) return it->second.fbo;

        GlTarget g;
        EGLint attr[] = {
            EGL_WIDTH, t.frame.width,
            EGL_HEIGHT, t.frame.height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)t.frame.fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT, t.frame.fd[0],
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)t.frame.stride[0],
            EGL_NONE
        };
        g.image = eglCreateImageKHR_(egl, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
        if (g.image == EGL_NO_IMAGE_KHR) {
            std::fprintf(stderr, "[gl] eglCreateImageKHR для цілі провалився\n");
            return 0;
        }

        // Renderbuffer поверх ТІЄЇ САМОЇ пам'яті — після цього glClear і
        // glDraw пишуть прямо в буфер, який покаже дисплей.
        glGenRenderbuffers(1, &g.rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g.rb);
        glEGLImageTargetRenderbufferStorageOES_(GL_RENDERBUFFER, g.image);

        glGenFramebuffers(1, &g.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, g.rb);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[gl] FBO неповний (0x%04x)\n", st);
            return 0;
        }

        gl_targets[t.frame.fd[0]] = g;
        return g.fbo;
    }

    // Викидається цілком при зміні конфігурації виводу: буфери під цими
    // обгортками дисплей уже знищив.
    void drop_gl_targets() {
        for (auto& kv : gl_targets) {
            GlTarget& g = kv.second;
            if (g.fbo) glDeleteFramebuffers(1, &g.fbo);
            if (g.rb) glDeleteRenderbuffers(1, &g.rb);
            if (g.image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_) {
                eglDestroyImageKHR_(egl, g.image);
            }
        }
        gl_targets.clear();
    }


    // Створюється в робочому потоці: GL-об'єкти потребують поточного
    // контексту.
    bool create_gl_objects() {
        prog_ext = link_program(kQuadVS, kExternalFS);
        if (!prog_ext) return false;
        prog_tex = link_program(kQuadVS, kTexture2dFS);
        if (!prog_tex) return false;
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ovl_vbo);
        textures.init(egl, eglCreateImageKHR_, eglDestroyImageKHR_,
                      glEGLImageTargetTexture2DOES_);
        return true;
    }

    void destroy_gl_objects() {
        textures.clear();
        {
            std::lock_guard<std::mutex> lk(ovl_mtx);
            for (OverlaySlot& s : overlays) {
                for (GLuint t : s.textures) if (t) glDeleteTextures(1, &t);
                s.textures.clear();
            }
        }
        if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
        if (ovl_vbo) { glDeleteBuffers(1, &ovl_vbo); ovl_vbo = 0; }
        if (prog_ext) { glDeleteProgram(prog_ext); prog_ext = 0; }
        if (prog_tex) { glDeleteProgram(prog_tex); prog_tex = 0; }
    }

    // Малює один кадр: тло плюс усі джерела, що дали кадр.
    // Три вертикальні смуги чистого R, G, B зліва направо.
    //
    // НАВІЩО. Перевірити, що порядок каналів у XRGB8888 саме такий, як ми
    // думаємо. Помилка тут майже не помітна на службовій графіці — очі не
    // чіпляються за трохи інші відтінки, — але зіпсує колір усього відео,
    // і шукати її потім довго: підозра піде на декодер, на формат кадру,
    // на що завгодно, тільки не на порядок байтів.
    //
    // Робиться ножицями й очищенням, БЕЗ шейдера й текстури. Це навмисно:
    // так із перевірки викинуто все, що могло б само внести помилку. Якщо
    // ліва смуга не червона — винен формат буфера або сканування, і
    // більше нічого.
    void draw_colortest() {
        const int w = info.width, h = info.height;
        const int third = w / 3;
        const struct { int x, w; float r, g, b; } bars[3] = {
            {0,           third,         1.f, 0.f, 0.f},
            {third,       third,         0.f, 1.f, 0.f},
            {third * 2,   w - third * 2, 0.f, 0.f, 1.f},
        };

        glEnable(GL_SCISSOR_TEST);
        for (const auto& b : bars) {
            glScissor(b.x, 0, b.w, h);
            glClearColor(b.r, b.g, b.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
    }

    // Що і де малювати цього кадру.
    struct Item {
        layout::Placement p;
        source::SourceFrame f;
    };

#if VRX_MEASURE
    // Що саме зараз їде на екран, по каналах. Заповнює потік малювання,
    // читає потік подій дисплея в обробнику розгортки — звідти й лок.
    std::mutex pm_mtx;
    std::vector<std::pair<int, int64_t>> pm_inflight;
#endif

    // Забирає кадри в усіх джерел і вкладає їх у порядок малювання.
    //
    // Повертає false, якщо показувати нема чого. Заглушку в такому разі
    // не малюємо: програма одна, а дрони різні, і будь-яка заставка
    // припускала б знання про конкретну конфігурацію, якого в програми
    // немає.
    bool collect_items(std::vector<Item>& items, int64_t& primary_produced) {
        std::vector<std::shared_ptr<source::FrameSource>> snap;
        {
            std::lock_guard<std::mutex> lk(src_mtx);
            snap = sources;
        }
        if (snap.empty()) return false;

        const float screen_aspect = float(info.width) / float(info.height);
        items.reserve(snap.size());
#if VRX_MEASURE
        std::vector<std::pair<int, int64_t>> pm_now;
#endif
        primary_produced = 0;

        bool primary = true;
        for (auto& src : snap) {
            const bool is_primary = primary;
            primary = false;

            source::SourceFrame f;
            if (!src->acquire(f)) continue;
            if (!f.valid() || !f.where.enabled) continue;
            if (f.image.fd[0] < 0) continue;      // кадру як буфера немає

            const float a = f.aspect();
            if (a <= 0.0f) continue;

            // ФАЗУ МІРЯЄМО ЛИШЕ ПО ПЕРШОМУ ДЖЕРЕЛУ.
            //
            // Синхронізуватись можна рівно з одним передавачем: розгортка
            // одна, а кожна камера має власний кварц. Другий канал живе
            // як виходить — його фаза нікому не підпорядкована.
            //
            // Раніше тут стояв МАКСИМУМ produced_ns по всіх джерелах. З
            // однією камерою це збігалося з "по першому", тож і
            // працювало. З двома максимум стрибав би між ними, і петля
            // вела б то одну камеру, то другу.
            if (is_primary) primary_produced = f.produced_ns;

#if VRX_MEASURE
            pm_now.push_back({VRX_PM_CHANNEL(src->name()), f.produced_ns});
#endif

            items.push_back({layout::fit_source(f.where, a, screen_aspect), f});
        }
        if (items.empty()) return false;

#if VRX_MEASURE
        {
            std::lock_guard<std::mutex> lk(pm_mtx);
            pm_inflight.swap(pm_now);
        }
#endif

        // Порядок за z: менше — далі. Джерел одиниці, тож stable_sort
        // дешевший за будь-яку хитрість і зберігає порядок реєстрації
        // для однакових z.
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.p.z < b.p.z; });
        return true;
    }

    void draw_sources(const std::vector<Item>& items) {
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

    // КАДР, ЩО ЗАПІЗНИВСЯ НА ОПИТ. Не втрачений і не повторений — просто
    // поїде на екран на розгортку пізніше, ніж міг би.
    //
    // Затримка як функція фази — пила з єдиним обривом рівно в точці
    // опиту: кадр, що встиг, чекає (період − фаза), а кадр, що спізнився
    // на мікросекунду, чекає ЦІЛИЙ зайвий період. По жодному наявному
    // лічильнику цього не видно: кадр цілий, черга здорова, крок зйомки в
    // нормі — усі показання ідеальні, а затримка стрибнула на 17 мс.
    //
    // Саме цей перехід і є те, що петля фази тримає під контролем, тож
    // лічильник — пряма міра її роботи, а не непряма.
    //
    // Арифметика по модулю періоду тут не формальність: кадр, знятий у
    // попередньому періоді ПІСЛЯ його опиту, приходить сюди вже після
    // наступної розгортки, тобто produced < vblank. Обгортка робить із
    // цього велику фазу — тобто рівно "спізнився", як і має бути.
    void count_late(int64_t produced, int64_t vblank) {
        if (produced <= 0 || vblank <= 0 || period_ns <= 0) return;
        if (produced == last_late_produced) return;   // той самий кадр
        last_late_produced = produced;

        int64_t rel = (produced - vblank) % period_ns;
        if (rel < 0) rel += period_ns;

        std::lock_guard<std::mutex> lk(stats_mtx);
        st.taken_new++;
        if (rel / 1e6 > poll_offset_ms) st.late++;
    }

    void draw_frame() {
        if (cfg.colortest) { draw_colortest(); return; }

        // Чистимо ЗАВЖДИ. Під кадром, що не покрив увесь екран, інакше
        // лишиться вміст ПОЗАминулого кадру — буферів два, вони
        // чергуються. Плюс на тайловому GPU (Mali) очищення на початку
        // проходу дозволяє не завантажувати попередній вміст у тайлову
        // пам'ять, тобто виходить швидше, ніж без нього.
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // ВІДСУТНІСТЬ КАДРІВ — ЦЕ НОРМАЛЬНИЙ СТАН, А НЕ ПРИЧИНА НЕ
        // МАЛЮВАТИ КАДР.
        //
        // Тут стояв ранній вихід: немає жодного джерела з кадром —
        // виходимо з draw_frame(). Разом із відео зникав і ОСД, бо
        // draw_overlays() лишався за цим виходом.
        //
        // На екрані це виглядало так: пропала камера — і телеметрія
        // пропала теж. Тобто рівно в момент, коли пілоту треба бачити
        // висоту, напругу й стан лінка, екран гасне повністю. Причому
        // дані для ОСД нікуди не діваються: телеметрія йде своїм каналом,
        // збирач далі публікує квади, малювати їх ніхто не приходить.
        //
        // Джерела й оверлеї незалежні за задумом — рендерер знає лише
        // інтерфейси, — і єдине, що їх пов'язувало, це був цей `return`.
        std::vector<Item> items;
        int64_t primary_produced = 0;
        collect_items(items, primary_produced);

        const int64_t vbl = last_present_ns.load(std::memory_order_relaxed);
        phase.sample(primary_produced, vbl, period_ns);
        count_late(primary_produced, vbl);

        // Наступне підтвердження flip'а стосуватиметься саме цього кадру —
        // звідси й наскрізна затримка. НУЛЬ означає "кадру немає", і
        // обробник розгортки на нього не реагує.
        //
        // Раніше при відсутності кадру сюди не доходило взагалі, і в полі
        // лишалась мітка ОСТАННЬОГО показаного кадру. Обробник розгортки
        // чесно рахував по ній крок зйомки — виходило 0 мс, тобто
        // "повтор", і так на кожній розгортці, поки камери немає. За
        // прогін із камерою, вимкненою чверть часу, це дало 4432 фальшиві
        // повтори: показувати в ті моменти було нічого.
        inflight_produced_ns.store(primary_produced, std::memory_order_relaxed);

        // Порожній список — draw_sources() просто нічого не намалює, і на
        // екрані лишиться чистий фон під ОСД. Заглушки тут свідомо немає:
        // програма одна, а дрони різні, і рамка чи напис припускали б
        // знання про конкретну конфігурацію.
        draw_sources(items);
        draw_overlays();
    }

    // Оверлеї малюються ПІСЛЯ джерел, тобто поверх відео, і завжди зі
    // змішуванням: гліф має напівпрозорі краї (згладжування зроблене ще
    // в атласі), і без альфи він вийде з рваною облямівкою.
    //
    // Змішування вмикається тільки тут. Для відео воно шкідливе: кадр
    // непрозорий, а зайвий blend — це зайве читання цілі, тобто та сама
    // смуга пам'яті, заради якої вся ця архітектура й затіяна.
    void draw_overlays() {
        std::lock_guard<std::mutex> lk(ovl_mtx);
        if (overlays.empty() || !prog_tex) return;

        bool blend_on = false;

        for (OverlaySlot& slot : overlays) {
            // Знімок. Немає нового — малюємо попередній: OSD оновлюється
            // разів у двадцять на секунду, показ іде шістдесят.
            if (slot.ovl->acquire(slot.list)) slot.have = true;
            if (!slot.have || slot.list.quads.empty()) continue;

            ensure_overlay_textures(slot);

            if (!blend_on) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glUseProgram(prog_tex);
                glActiveTexture(GL_TEXTURE0);
                glUniform1i(glGetUniformLocation(prog_tex, "uTex"), 0);
                blend_on = true;
            }

            // Квади йдуть пачками з однією текстурою: підряд лежать усі
            // гліфи одного напису, і всі вони з атласу. Тому просто
            // ріжемо список на відрізки з однаковим image і малюємо
            // кожен одним викликом — двісті гліфів дають одну-дві пачки
            // замість двохсот перемикань стану.
            size_t i = 0;
            while (i < slot.list.quads.size()) {
                const int img = slot.list.quads[i].image;
                size_t j = i;
                while (j < slot.list.quads.size() && slot.list.quads[j].image == img) ++j;

                if (img >= 0 && img < (int)slot.textures.size() && slot.textures[img]) {
                    build_overlay_batch(slot.list, i, j);
                    glBindTexture(GL_TEXTURE_2D, slot.textures[img]);
                    glBindBuffer(GL_ARRAY_BUFFER, ovl_vbo);
                    glBufferData(GL_ARRAY_BUFFER,
                                 (GLsizeiptr)(ovl_verts.size() * sizeof(GLfloat)),
                                 ovl_verts.data(), GL_STREAM_DRAW);
                    glEnableVertexAttribArray(0);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                          4 * sizeof(GLfloat), (void*)0);
                    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                          4 * sizeof(GLfloat),
                                          (void*)(2 * sizeof(GLfloat)));
                    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(ovl_verts.size() / 4));
                }
                i = j;
            }
        }

        if (blend_on) glDisable(GL_BLEND);
    }

    // Вершини для відрізка квадів [from, to). Два трикутники на квад,
    // порядок вершин у квада: ЛВ, ПВ, ЛН, ПН.
    //
    // Частки екрана переводяться в NDC тут, а не в оверлеї: оверлей не
    // повинен знати ні про GL, ні про те, що вісь Y у ньому дивиться вгору.
    void build_overlay_batch(const DrawList& dl, size_t from, size_t to) {
        ovl_verts.clear();
        ovl_verts.reserve((to - from) * 6 * 4);

        // ВІСЬ Y ПЕРЕВЕРНУТА, і це не помилка оверлея, а властивість
        // цього тракту виводу. Перевірено на екрані трьома кроками:
        // з природним відображенням (1-2y) уся картинка виходила
        // дзеркальною; після перевороту лише текстурних координат гліфи
        // стали правильними, але опинилися не на своїх місцях — тобто
        // дзеркальний саме ВИВІД, а не вміст.
        //
        // Тому переворот тут один і стосується позиції. Оверлей лишається
        // в координатах картинки (Y вниз, v вниз) і про GL не знає нічого.
        //
        // Відео цього не помічає з тієї ж причини, з якої помилку було
        // важко впіймати: воно малюється одним прямокутником на весь
        // екран і перевертає свої текстурні координати (див.
        // draw_textured_quad), тож два дзеркала гасять одне одного.
        auto push = [this](const OverlayQuad& q, int k) {
            ovl_verts.push_back(2.0f * q.x[k] - 1.0f);
            ovl_verts.push_back(2.0f * q.y[k] - 1.0f);
            ovl_verts.push_back(q.u[k]);
            ovl_verts.push_back(q.v[k]);
        };

        for (size_t i = from; i < to; ++i) {
            const OverlayQuad& q = dl.quads[i];
            push(q, 0); push(q, 1); push(q, 2);
            push(q, 2); push(q, 1); push(q, 3);
        }
    }

    // Текстури оверлея вантажаться один раз: атлас і картинки не
    // змінюються за час роботи.
    void ensure_overlay_textures(OverlaySlot& slot) {
        const auto& imgs = slot.ovl->images();
        if (slot.textures.size() == imgs.size()) return;

        slot.textures.assign(imgs.size(), 0);
        for (size_t i = 0; i < imgs.size(); ++i) {
            const OverlayImage& im = imgs[i];
            if (im.width <= 0 || im.height <= 0 || im.rgba.empty()) continue;

            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            // Лінійна фільтрація: гліфи в атласі побудовані під свій
            // піксельний розмір і малюються 1:1, але екран може бути
            // іншої роздільності, і тоді найближчий сусід дав би драбинку.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, im.width, im.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
            slot.textures[i] = tex;
            std::fprintf(stderr, "[gl] текстура оверлея %s: %dx%d\n",
                         im.id.c_str(), im.width, im.height);
        }
    }

    void wake() {
        {
            std::lock_guard<std::mutex> lk(wake_mtx);
            woken = true;
        }
        wake_cv.notify_one();
    }

    // РОЗГОРТКА СТАЛАСЯ. Кличе дисплей зі свого потоку подій.
    //
    // Означає дві речі одночасно: попередній буфер звільнився, і ось
    // точна мітка часу самої розгортки. Обидві потрібні, і обидві
    // стаються в один момент.
    //
    // Раніше це була лямбда на сорок рядків усередині init() — там її не
    // було видно, хоча вона рахує дві метрики тракту.
    void on_flip(int64_t t) {
        last_present_ns.store(t, std::memory_order_relaxed);

        VRX_PM_FLIP(t);
#if VRX_MEASURE
        {
            std::lock_guard<std::mutex> lk(pm_mtx);
            for (const auto& e : pm_inflight) VRX_PM_SHOWN(e.first, e.second, t);
        }
#endif

        const int64_t produced = inflight_produced_ns.load(std::memory_order_relaxed);
        if (produced > 0) {
            std::lock_guard<std::mutex> lk(stats_mtx);

            if (t > produced) {
                const double ms = (t - produced) / 1e6;
                st.latency_avg_ms = st.latency_avg_ms == 0.0
                                          ? ms : st.latency_avg_ms * 0.95 + ms * 0.05;
                if (ms > st.latency_max_ms) st.latency_max_ms = ms;
            }

            // КРОК ЗЙОМКИ між сусідніми показаними кадрами.
            //
            // Лічильники повторів і пропусків міряють НАШУ роботу з
            // чергою і мовчать, поки ми чесно беремо по кадру на
            // розгортку. Але рух на екрані задає не це, а те, з яким
            // інтервалом ці кадри були ЗНЯТІ. Показ рівномірний по
            // сітці розгортки; якщо крок зйомки при цьому стрибає
            // (17, 33, 17, 0...), рух смикається, і жоден наявний
            // лічильник цього не побачить.
            if (prev_pres_produced > 0 && period_ns > 0) {
                const double step = (produced - prev_pres_produced) / 1e6;
                const double p = period_ns / 1e6;
                if (step <= 0.0)          st.step_repeat++;
                else if (step < p * 0.5)  st.step_short++;
                else if (step < p * 1.5)  st.step_ok++;
                else                      st.step_gap++;
                if (step > 0.0) {
                    if (st.step_min_ms == 0.0 || step < st.step_min_ms)
                        st.step_min_ms = step;
                    if (step > st.step_max_ms) st.step_max_ms = step;
                }
            }
            prev_pres_produced = produced;
        }
        wake();
    }

    // Чекає на розгортку. Прокидається колбек дисплея — саме тоді
    // звільняється буфер. Таймаут страхує від загубленої події.
    void wait_flip(int ms) {
        std::unique_lock<std::mutex> lk(wake_mtx);
        wake_cv.wait_for(lk, std::chrono::milliseconds(ms),
                         [this] { return woken || !running.load(); });
        woken = false;
    }

    // Спить до моменту, коли треба питати кадр.
    //
    // ЧОМУ НЕ ОДРАЗУ ПІСЛЯ FLIP'А. Кадр від борту завершується не на
    // початку свого періоду, а через 5-11 мс: кодер кодує, шейпер пакетує
    // (заміряно на живому лінку — передача кадру 5.0/7.2/10.6 мс за
    // p50/p90/max). Якщо питати одразу після flip'а, ми систематично
    // застаємо момент, коли свіжий кадр ЩЕ В ДОРОЗІ, показуємо старий, а
    // той, що прийшов через 3 мс, чекає цілу розгортку.
    //
    // Симптом — мікрофризи: частина кадрів показується двічі, частина
    // викидається. По лічильниках виглядає як "дроп 12/с, повтор 12/с"
    // при живих 60 к/с з обох боків.
    //
    // Питаємо натомість ПЕРЕД дедлайном: настільки пізно, наскільки
    // встигаємо звести й закомітити. Тоді кадр, готовий у будь-який
    // момент періоду, потрапляє на найближчу розгортку. Заразом падає
    // затримка: обраний кадр показується не через період, а через
    // час_зведення.
    void sleep_until_poll_deadline() {
        const int64_t last = last_present_ns.load(std::memory_order_relaxed);
        if (last == 0 || period_ns <= 0) return;   // ще не показували

        // Запас: зведення плаває, а коміт має встигнути до того, як VOP2
        // засуне конфігурацію. Півтори тривалості зведення плюс 1.5 мс —
        // якщо не встигнемо, кадр поїде на наступну розгортку, тобто
        // вийде рівно та проблема, яку лікуємо.
        double draw_ms;
        {
            std::lock_guard<std::mutex> lk(stats_mtx);
            draw_ms = st.avg_draw_ms > 0.0 ? st.avg_draw_ms : 3.0;
        }
        const int64_t reserve_ns = (int64_t)(draw_ms * 1.3e6) + 1000000;

        // Пізніше цієї межі опитувати не можна: не встигнемо звести й
        // закомітити до того, як VOP2 засуне конфігурацію.
        // СТАЛИЙ пізній зсув, без підстроювання.
        //
        // Спроба вести зсув контролером (повтор -> пізніше, інакше ->
        // раніше) дала 1.5 повтору/с замість 5, але почала ПОЛЮВАТИ:
        // кожен рух зсуву — це зайвий перетин фази приходу, тобто
        // контролер сам собі створював пропуски.
        //
        // При сталому зсуві повтор трапляється лише коли фаза приходу
        // перетинає точку опиту, а вона пливе повільно (оберт ~4.7 с
        // через різницю 60.00 проти 59.789). Двічі за оберт => ~0.43/с,
        // і це вже теоретична межа без буферизації.
        poll_offset_ms = (period_ns - reserve_ns) / 1e6;
        if (poll_offset_ms < 0.0) poll_offset_ms = 0.0;

        int64_t deadline = last + (int64_t)(poll_offset_ms * 1e6);
        const int64_t now = now_ns();
        if (deadline <= now) return;               // вже пізно, працюємо негайно

        // Обмеження зверху: не спати довше за період, навіть якщо
        // підтвердження flip'а раптом загубилося.
        int64_t wait_ns = deadline - now;
        if (wait_ns > period_ns) wait_ns = period_ns;

        std::unique_lock<std::mutex> lk(wake_mtx);
        wake_cv.wait_for(lk, std::chrono::nanoseconds(wait_ns),
                         [this] { return !running.load(std::memory_order_relaxed); });
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

        // Програми шейдерів від геометрії не залежать — робимо один раз.
        if (!create_gl_objects()) {
            running.store(false);
            return;
        }
        glDisable(GL_DEPTH_TEST);

        int frame_no = 0;
        double avg = 0.0;
        uint32_t my_generation = 0;

        while (running.load(std::memory_order_relaxed)) {
            // Буфер для цього кадру. Дисплей віддає лише той, який точно
            // не читається сканером, тож перевіряти щось самому не треба.
            display::Target target;
            if (!dpy->acquire(target)) {
                // Або виводу немає, або всі буфери зайняті. І те, і те
                // розв'яжеться розгорткою — на неї й чекаємо.
                wait_flip(100);
                continue;
            }

            // ЗАМІНА МОНІТОРА видно по номеру конфігурації в самій цілі.
            // Порівнювати ширину й висоту було б недостатньо: новий
            // монітор може мати ту саму роздільність, але це вже інший
            // тракт, і старі обгортки над буферами недійсні.
            if (target.generation != my_generation) {
                if (my_generation != 0) {
                    std::fprintf(stderr, "[gl] вивід змінився, перебудовую обгортки\n");
                }
                drop_gl_targets();
                // Попередні виміри стосуються іншого періоду розгортки —
                // тягнути їх у нову конфігурацію не можна.
                phase.reset();
                info = dpy->state();
                my_generation = target.generation;
                period_ns = info.frame_time_ns();
                glViewport(0, 0, info.width, info.height);

                // Оверлеї рахують розміри гліфів у частках кадру, тож
                // нову геометрію треба сказати їм ТУТ. Інакше після
                // перепідключення монітора з іншою роздільністю OSD
                // лишився б із масштабом від попереднього — і це було б
                // не падіння, а тихо неправильний розмір тексту.
                {
                    std::lock_guard<std::mutex> lk(ovl_mtx);
                    for (OverlaySlot& sl : overlays) {
                        sl.ovl->set_frame_size(info.width, info.height);
                    }
                }

                std::fprintf(stderr, "[gl] %dx%d | період %.2f мс | fence: %s\n",
                             info.width, info.height, period_ns / 1e6,
                             have_fence ? "є" : "немає");
            }

            const GLuint fbo = fbo_for(target);
            if (!fbo) {
                // Обгортка не створилась — віддаємо буфер назад, інакше
                // він застрягне в Drawing назавжди.
                dpy->present(target);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            // ДІАГНОСТИКА: VRX_TEST_HOLD_MS змушує тримати ціль довше,
            // ніж триває звичайний кадр. Потрібно, щоб перевірити
            // відкладене звільнення буферів при розбиранні виводу: у
            // нормальному темпі стан Drawing надто короткий, щоб у нього
            // влучити ззовні.
            if (test_hold_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(test_hold_ms));
            }

            // Спимо до моменту опитування — щоб узяти найсвіжіший кадр,
            // а не найстаріший.
            sleep_until_poll_deadline();
            if (!running.load(std::memory_order_relaxed)) break;

            const double t0 = now_ms();

            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            draw_frame();
            frame_no++;

            // Чекаємо РЕАЛЬНОГО завершення GPU. Без цього на екран поїде
            // недомальований буфер: команди лише поставлені в чергу.
            // IN_FENCE_FD на цьому ядрі немає, тож чекаємо тут, у своєму
            // потоці — потік показу це не гальмує.
            if (have_fence && eglCreateSyncKHR_) {
                EGLSyncKHR sy = eglCreateSyncKHR_(egl, EGL_SYNC_FENCE_KHR, nullptr);
                glFlush();
                eglClientWaitSyncKHR_(egl, sy, 0, EGL_FOREVER_KHR);
                eglDestroySyncKHR_(egl, sy);
            } else {
                glFinish();
            }

            const double dt = now_ms() - t0;
            avg = avg == 0.0 ? dt : (avg * 0.95 + dt * 0.05);

            if (dpy->present(target)) {
                std::lock_guard<std::mutex> lk(stats_mtx);
                st.frames++;
                st.last_draw_ms = dt;
                st.avg_draw_ms = avg;
            }
        }

        drop_gl_targets();
        destroy_gl_objects();
        eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
};

// ---------------------------------------------------------------------

GlRenderer::GlRenderer() : impl_(std::make_unique<Impl>()) {}
GlRenderer::~GlRenderer() { stop(); }

bool GlRenderer::init(display::Display& display) {
    return init(display, Config{});
}

bool GlRenderer::init(display::Display& display, Config cfg) {
    Impl& d = *impl_;
    d.cfg = std::move(cfg);
    d.dpy = &display;
    d.info = display.state();

    // GBM-пристрій БЕРЕТЬСЯ В ДИСПЛЕЯ, а не створюється свій.
    //
    // Раніше рендерер відкривав ту саму карту другим дескриптором і
    // робив на ній власний GBM — лише щоб виділяти буфери, які дисплей
    // потім імпортував назад через dmabuf. Ця подорож існувала тільки
    // тому, що володіння буферами було розділене навпіл. Тепер пристрій
    // один, а EGL просто піднімається поверх нього.
    d.gbm = (struct gbm_device*)display.native_handle();
    if (!d.gbm) {
        std::fprintf(stderr, "[gl] дисплей не дав GBM-пристрій\n");
        return false;
    }
    if (!d.setup_egl()) return false;

    // Прокидаємось на підтвердженні показу: саме тоді звільняється буфер.
    // Опитування тут було б і марною роботою, і зайвою затримкою.
    display.set_flip_callback([&d](int64_t t) { d.on_flip(t); });

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
    // GBM і карту не чіпаємо: ними володіє дисплей.
    d.gbm = nullptr;
}

void GlRenderer::add_source(std::shared_ptr<source::FrameSource> src) {
    if (!src) return;
    std::lock_guard<std::mutex> lk(impl_->src_mtx);
    impl_->sources.push_back(std::move(src));
}

void GlRenderer::add_overlay(std::shared_ptr<Overlay> ovl) {
    if (!ovl) return;
    ovl->set_frame_size(impl_->info.width, impl_->info.height);
    std::lock_guard<std::mutex> lk(impl_->ovl_mtx);
    Impl::OverlaySlot slot;
    slot.ovl = std::move(ovl);
    impl_->overlays.push_back(std::move(slot));
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
    RenderStats s = impl_->st;
    s.poll_offset_ms = impl_->poll_offset_ms;
    const PhaseMeter::Snapshot ph = impl_->phase.read();
    s.phase_ms = ph.phase_ms;
    s.phase_drift_ms_per_s = ph.drift_ms_per_s;
    // 0 = вікно ще не закрилося. Контролер на цьому бере
    // максимальний запас, а не мінімальний.
    s.phase_jitter_ms = ph.valid ? ph.jitter_ms : 0.0;
    s.phase_noise_ms = ph.noise_ms;
    return s;
}

} // namespace vrx::render
