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

    // ЗАМІР ФАЗИ. Фаза — величина КУТОВА: 0.1 мс і 16.6 мс сусіди, а не
    // протилежності. Усереднювати її звичайним фільтром не можна, і це
    // не педантизм — на цьому вимір уже один раз збрехав.
    //
    // Було: ЕМА з α=0.02 по різниці "по найкоротшій дузі". Поки фаза
    // стоїть, воно працює. Але фаза ПОВЗЕ, і швидкість, яку такий фільтр
    // здатний відстежити, обмежена: за крок він може підтягнутися щонайб.
    // на α·півперіоду, тобто ~10 мс/с. При більшому дрейфі відставання
    // переростає півперіоду, різниця по колу перекидає знак, і фільтр
    // їде НАЗАД. На виході — рівна правдоподібна цифра (у нас −3.6 мс/с),
    // яка не міняється ні від чого. Контролер на такому вході сліпий.
    //
    // Стало: кругове середнє на вікні. Складаємо одиничні вектори кута
    // кожного кадру й беремо аргумент суми — операція, для якої обгортка
    // не існує в принципі. Довжина тієї ж суми дає розкид безкоштовно:
    // вектори збігаються -> |сума| ≈ n, розкид нуль; розкидані по колу ->
    // |сума| -> 0.
    double acc_sin = 0, acc_cos = 0;
    uint32_t acc_n = 0;
    int64_t acc_start_ns = 0;

    // Вікно усереднення. Компроміс: довше — точніше середнє, але фаза
    // встигає проповзти всередині вікна й роздути оцінку розкиду.
    // 250 мс = ~15 кадрів, і при дрейфі 6 мс/с розмиття 1.5 мс.
    static constexpr int64_t kPhaseWindowNs = 250000000LL;

    double phase_ms = 0;          // опубліковане середнє за останнє вікно
    double phase_jitter_ms = 0;   // кругове σ того ж вікна
    bool phase_valid = false;
    double phase_prev = -1;       // попереднє середнє, для розгортки
    int64_t phase_prev_ns = 0;
    double phase_drift = 0;

    // ДРЕЙФ — нахил розгорнутої фази на вікні, а не різниця двох сусідніх
    // середніх. Різниця через 250 мс ділить похибку середнього (~0.4 мс)
    // на надто короткий базис і дає ±2.3 мс/с шуму; частотна ланка
    // перетворює це на випадкове блукання тримінгу, яке потім доводиться
    // виправляти фазовій ланці. Регресія на 2 с зменшує шум на порядок.
    //
    // Фаза тут уже РОЗГОРНУТА (unwrapped): кожне нове середнє додається
    // до попереднього через різницю по найкоротшій дузі, тож усередині
    // кільця обгортки немає й нахил рахується звичайним МНК.
    static constexpr int kDriftPoints = 8;      // 8 x 250 мс = 2 с
    double un_phase[kDriftPoints] = {};
    double un_time[kDriftPoints] = {};
    int un_n = 0, un_head = 0;
    double un_acc = 0;                          // накопичена розгорнута фаза
    int64_t phase_last_produced = 0;   // щоб не міряти той самий кадр двічі

    // Сирі вікна фази в stderr — вмикається VRX_PHASE_DEBUG=1. Потрібне
    // саме тому, що зведені числа тут уже один раз збрехали, і без
    // сирого ряду відрізнити "вимір поганий" від "об'єкт такий" не можна.
    bool phase_debug = getenv("VRX_PHASE_DEBUG") != nullptr;
    // Скільки сирих зразків вивести. Обмежено навмисно: цей дамп потрібен
    // для розбору, а не для роботи, і 400 кадрів (7 с) вистачає, щоб
    // порахувати нахил фази з похибкою краще за 0.05 мс/с.
    int dbg_left = 400;

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

        // ФАЗУ І ЗАТРИМКУ МІРЯЄМО ЛИШЕ ПО ПЕРШОМУ ДЖЕРЕЛУ.
        //
        // Синхронізуватись можна рівно з одним передавачем: розгортка
        // одна, а кожна камера має власний кварц. Другий канал живе як
        // виходить — його фаза відносно нашого показу нікому не
        // підпорядкована.
        //
        // Раніше тут стояв МАКСИМУМ produced_ns по всіх джерелах. З
        // однією камерою це збігалося з "по першому", тож і працювало.
        // З двома максимум стрибав би між ними: у кожної своя фаза, і
        // петля вела б то одну камеру, то другу.
        int64_t newest_produced = 0;

        bool primary = true;
        for (auto& src : snap) {
            const bool is_primary = primary;
            primary = false;

            source::SourceFrame f;
            // Джерела немає або сигналу немає — не малюємо нічого.
            // Програма одна, а дрони різні: заглушка припускала б знання
            // про конкретну конфігурацію, якого в програми немає.
            if (!src->acquire(f)) continue;
            if (!f.valid() || !f.where.enabled) continue;
            if (f.image.fd[0] < 0) continue;      // кадру як буфера немає

            const float a = f.aspect();
            if (a <= 0.0f) continue;

            if (is_primary) newest_produced = f.produced_ns;
            items.push_back({layout::fit_source(f.where, a, screen_aspect), f});
        }
        if (items.empty()) return;

        // Порядок за z: менше — далі. Джерел одиниці, тож stable_sort
        // дешевший за будь-яку хитрість і зберігає порядок реєстрації
        // для однакових z.
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.p.z < b.p.z; });

        // Фаза: скільки минуло від останнього vblank до появи кадру.
        // Береться за модулем періоду, бо кадр міг прийти й на кілька
        // періодів раніше (тоді нас цікавить лише його позиція в сітці).
        //
        // Рахуємо ЛИШЕ по нових кадрах. Коли нового немає, джерело за
        // контрактом віддає попередній — з його старою міткою. Пустити
        // таку мітку у фільтр означало б виміряти той самий прихід
        // двічі, причому вдруге вже з іншого vblank'а: рівно та частина
        // вибірки, яка ЗМІЩЕНА в бік цілі, отримала б подвійну вагу.
        // Для контролера це систематична похибка, а не шум.
        if (newest_produced > 0 && newest_produced != phase_last_produced && period_ns > 0) {
            phase_last_produced = newest_produced;
            const int64_t last_v = last_present_ns.load(std::memory_order_relaxed);
            if (last_v > 0) {
                int64_t rel = (newest_produced - last_v) % period_ns;
                if (rel < 0) rel += period_ns;
                const double period_ms = period_ns / 1e6;
                const double th = 2.0 * M_PI * (rel / (double)period_ns);

                if (phase_debug && dbg_left > 0) {
                    dbg_left--;
                    std::fprintf(stderr, "RAW %.6f %.6f %.3f\n",
                                 newest_produced / 1e9, last_v / 1e9, rel / 1e6);
                }

                acc_sin += std::sin(th);
                acc_cos += std::cos(th);
                acc_n++;
                if (acc_start_ns == 0) acc_start_ns = newest_produced;

                // Вікно закрилося — публікуємо середнє й розкид.
                // Мінімум кадрів окремо від часу: на 30 к/с їх удвічі
                // менше, а на кількох штуках кругове σ ще безглузде.
                if (newest_produced - acc_start_ns >= kPhaseWindowNs && acc_n >= 8) {
                    const double n = (double)acc_n;
                    double mean = std::atan2(acc_sin / n, acc_cos / n);
                    if (mean < 0.0) mean += 2.0 * M_PI;
                    const double mean_ms = mean * period_ms / (2.0 * M_PI);

                    // Довжина середнього вектора: 1 — усі кадри в одну
                    // точку, 0 — рівномірно по колу. Стандартна кругова
                    // сигма: sqrt(-2 ln R), у радіанах.
                    const double R = std::hypot(acc_sin, acc_cos) / n;
                    const double sig_rad = R > 1e-6 ? std::sqrt(-2.0 * std::log(R < 1.0 ? R : 1.0))
                                                    : 2.0 * M_PI;
                    double sig_ms = sig_rad * period_ms / (2.0 * M_PI);
                    if (sig_ms > period_ms * 0.25) sig_ms = period_ms * 0.25;

                    // Дрейф — по двох сусідніх середніх. Тепер це можна:
                    // середні йдуть через 250 мс, і навіть 30 мс/с дає
                    // між ними 7.5 мс, тобто менше півперіоду. Обгортка
                    // однозначна.
                    // Розгортаємо: до накопиченої фази додаємо крок по
                    // найкоротшій дузі. Кроки йдуть через 250 мс, тож
                    // навіть 30 мс/с дають менше півперіоду — однозначно.
                    if (phase_prev >= 0) {
                        double dd = mean_ms - phase_prev;
                        if (dd >  period_ms * 0.5) dd -= period_ms;
                        if (dd < -period_ms * 0.5) dd += period_ms;
                        un_acc += dd;
                    }
                    phase_prev = mean_ms;
                    phase_prev_ns = newest_produced;

                    un_phase[un_head] = un_acc;
                    un_time[un_head] = newest_produced / 1e9;
                    un_head = (un_head + 1) % kDriftPoints;
                    if (un_n < kDriftPoints) un_n++;

                    double drift = phase_drift;
                    if (un_n >= 4) {
                        double sx = 0, sy = 0;
                        for (int k = 0; k < un_n; ++k) { sx += un_time[k]; sy += un_phase[k]; }
                        const double mx = sx / un_n, my = sy / un_n;
                        double num = 0, den = 0;
                        for (int k = 0; k < un_n; ++k) {
                            const double dx = un_time[k] - mx;
                            num += dx * (un_phase[k] - my);
                            den += dx * dx;
                        }
                        if (den > 1e-9) drift = num / den;
                    }

                    if (phase_debug) {
                        std::fprintf(stderr, "PH %.3f %.3f %u %.3f %.3f\n",
                                     newest_produced / 1e9, mean_ms, acc_n, sig_ms, drift);
                    }

                    {
                        std::lock_guard<std::mutex> lk(stats_mtx);
                        phase_ms = mean_ms;
                        // Розкид згладжуємо, а середнє — ні. Різниця не
                        // формальна: середнє кутове, і фільтрувати його
                        // звичайним ЕМА не можна; розкид же — звичайна
                        // додатна величина.
                        //
                        // Згладжувати ТРЕБА: по 15 кадрах вікна оцінка σ
                        // сама має похибку ~18%, а один викид роздуває її
                        // вдвічі. Запас 2σ цей скач подвоює, ціль повзе
                        // за ним на ±1.6 мс, і петля ганяється за власним
                        // шумом замість тримати фазу. Стала ~2.5 с.
                        phase_jitter_ms = phase_valid
                                        ? phase_jitter_ms * 0.9 + sig_ms * 0.1
                                        : sig_ms;
                        phase_drift = drift;
                        phase_valid = true;
                    }

                    acc_sin = acc_cos = 0.0;
                    acc_n = 0;
                    acc_start_ns = 0;
                }
            }
        }

        inflight_produced_ns.store(newest_produced, std::memory_order_relaxed);

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
    display.set_flip_callback([&d](int64_t t) {
        d.last_present_ns.store(t, std::memory_order_relaxed);

        const int64_t produced = d.inflight_produced_ns.load(std::memory_order_relaxed);
        if (produced > 0) {
            std::lock_guard<std::mutex> lk(d.stats_mtx);

            if (t > produced) {
                const double ms = (t - produced) / 1e6;
                d.st.latency_avg_ms = d.st.latency_avg_ms == 0.0
                                          ? ms : d.st.latency_avg_ms * 0.95 + ms * 0.05;
                if (ms > d.st.latency_max_ms) d.st.latency_max_ms = ms;
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
            if (d.prev_pres_produced > 0 && d.period_ns > 0) {
                const double step = (produced - d.prev_pres_produced) / 1e6;
                const double p = d.period_ns / 1e6;
                if (step <= 0.0)          d.st.step_repeat++;
                else if (step < p * 0.5)  d.st.step_short++;
                else if (step < p * 1.5)  d.st.step_ok++;
                else                      d.st.step_gap++;
                if (step > 0.0) {
                    if (d.st.step_min_ms == 0.0 || step < d.st.step_min_ms)
                        d.st.step_min_ms = step;
                    if (step > d.st.step_max_ms) d.st.step_max_ms = step;
                }
            }
            d.prev_pres_produced = produced;
        }
        d.wake();
    });

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
    s.phase_ms = impl_->phase_ms;
    s.phase_drift_ms_per_s = impl_->phase_drift;
    // 0 = вікно ще не закрилося. Контролер на цьому бере максимальний
    // запас, а не мінімальний.
    s.phase_jitter_ms = impl_->phase_valid ? impl_->phase_jitter_ms : 0.0;
    return s;
}

} // namespace vrx::render
