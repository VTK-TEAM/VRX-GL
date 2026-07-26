#pragma once

// Малювання текстурованого прямокутника і кеш текстур із dmabuf.
//
// Включати ЛИШЕ з gl_renderer.cpp: тягне за собою EGL і GLES, і нема
// причини нести їх у решту проєкту.

#include "../display/display_manager.hpp"
#include "../layout/layout.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <cstdio>
#include <map>
#include <vector>

namespace vrx::render {

// ---------------------------------------------------------------------
// Шейдери
// ---------------------------------------------------------------------

inline GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] компіляція шейдера: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

inline GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUV");
    glLinkProgram(p);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gl] лінкування програми: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// Позиція приходить уже в NDC, множень у шейдері немає — прямокутник
// один на кадр, рахувати його на CPU дешевше й зрозуміліше.
inline const char* kQuadVS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

// Зовнішня текстура: кадр із декодера. Конверсію YUV->RGB робить
// апаратний семплер, безкоштовно — саме тому NV12 виходить ДЕШЕВШИМ за
// RGB, попри "зайвий" крок.
inline const char* kExternalFS =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform samplerExternalOES uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

// Звичайна текстура: атлас OSD. Окрема програма, бо sampler2D і
// samplerExternalOES — РІЗНІ типи GLSL і в одну програму не компілюються.
inline const char* kTexture2dFS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";

// ---------------------------------------------------------------------
// Кеш текстур із dmabuf
// ---------------------------------------------------------------------

class DmabufTextureCache {
public:
    void init(EGLDisplay egl,
              PFNEGLCREATEIMAGEKHRPROC create,
              PFNEGLDESTROYIMAGEKHRPROC destroy,
              PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind) {
        egl_ = egl; create_ = create; destroy_ = destroy; bind_ = bind;
    }

    // Текстура для кадру. Створює один раз на буфер, далі віддає готову:
    // джерело крутить сталий пул, тож кеш заповнюється на перших кадрах
    // і застигає.
    GLuint get(const display::Frame& f) {
        auto it = cache_.find(f.fd[0]);
        if (it != cache_.end()) {
            // ЗВІРКА ОБОВ'ЯЗКОВА. При зміні роздільності джерело звільняє
            // старий пул і виділяє новий, а ядро цілком може видати новим
            // буферам ТІ САМІ номери дескрипторів. Без звірки ми віддали б
            // стару текстуру на новий буфер — застигла картинка або сміття,
            // і жодного повідомлення в лозі.
            if (same(it->second, f)) return it->second.tex;
            drop(it);
        }
        return create_entry(f);
    }

    void clear() {
        for (auto& kv : cache_) release(kv.second);
        cache_.clear();
    }

    int size() const { return (int)cache_.size(); }

private:
    struct Entry {
        GLuint tex = 0;
        EGLImageKHR img = EGL_NO_IMAGE_KHR;
        int w = 0, h = 0;
        uint32_t fourcc = 0, stride = 0;
    };

    static bool same(const Entry& e, const display::Frame& f) {
        return e.w == f.width && e.h == f.height &&
               e.fourcc == f.fourcc && e.stride == f.stride[0];
    }

    void release(Entry& e) {
        if (e.tex) glDeleteTextures(1, &e.tex);
        if (e.img != EGL_NO_IMAGE_KHR && destroy_) destroy_(egl_, e.img);
        e = Entry{};
    }

    void drop(std::map<int, Entry>::iterator it) {
        release(it->second);
        cache_.erase(it);
    }

    GLuint create_entry(const display::Frame& f) {
        std::vector<EGLint> a = {
            EGL_WIDTH,  f.width,
            EGL_HEIGHT, f.height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)f.fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT, f.fd[0],
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)f.offset[0],
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)f.stride[0],
        };
        if (f.n_planes > 1) {
            a.insert(a.end(), {
                EGL_DMA_BUF_PLANE1_FD_EXT, f.fd[1] >= 0 ? f.fd[1] : f.fd[0],
                EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)f.offset[1],
                EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)f.stride[1],
            });
        }
        a.push_back(EGL_NONE);

        Entry e;
        e.img = create_(egl_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a.data());
        if (e.img == EGL_NO_IMAGE_KHR) {
            std::fprintf(stderr, "[gl] eglCreateImageKHR для кадру %dx%d 0x%08x провалився\n",
                         f.width, f.height, f.fourcc);
            return 0;
        }

        glGenTextures(1, &e.tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, e.tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        bind_(GL_TEXTURE_EXTERNAL_OES, e.img);

        e.w = f.width; e.h = f.height;
        e.fourcc = f.fourcc; e.stride = f.stride[0];

        GLuint tex = e.tex;
        cache_[f.fd[0]] = e;
        return tex;
    }

    EGLDisplay egl_ = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC create_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC bind_ = nullptr;
    std::map<int, Entry> cache_;
};

// ---------------------------------------------------------------------
// Прямокутник
// ---------------------------------------------------------------------

// Малює текстуру в заданому місці.
//
//   p   — уже пройдене через fit_source(), у частках 0..1 від ВЕРХНЬОГО
//         лівого кута екрана
//   vis — видима частина кадру (crop) у пікселях буфера
//
// Два перетворення, кожне з яких легко наплутати:
//
//   позиція: частки з верхнього лівого -> NDC (-1..1) з НИЖНЬОГО лівого
//   UV:      піксели crop -> 0..1, з переворотом по Y
//
// Переворот потрібен, бо в GL початок текстури внизу, а і в PNG, і в
// кадрі декодера — вгорі. Забутий переворот дає картинку догори дриґом.
inline void draw_textured_quad(GLuint vbo,
                               const layout::Placement& p,
                               const display::Rect& vis,
                               int buf_w, int buf_h) {
    const float x0 = 2.0f * p.x - 1.0f;
    const float x1 = 2.0f * (p.x + p.w) - 1.0f;
    const float y0 = 1.0f - 2.0f * p.y;                 // верх
    const float y1 = 1.0f - 2.0f * (p.y + p.h);         // низ

    const float u0 = float(vis.x) / buf_w;
    const float u1 = float(vis.x + vis.w) / buf_w;

    // Перевірено на екрані: для текстури з dmabuf координата v=0 лежить
    // ЗНИЗУ, а перший рядок буфера — зверху. Тобто верху екрана
    // відповідає БІЛЬШЕ значення v, а не менше.
    //
    // Спершу я зробив навпаки, і картинка вийшла догори дриґом: у
    // тестовому візерунку червоний росте вправо, зелений вниз, тож
    // правий верхній кут мав бути червоним, а виявився знизу.
    const float v_top    = 1.0f - float(vis.y) / buf_h;
    const float v_bottom = 1.0f - float(vis.y + vis.h) / buf_h;

    const GLfloat verts[] = {
        // позиція        UV
        x0, y0,      u0, v_top,
        x1, y0,      u1, v_top,
        x0, y1,      u0, v_bottom,
        x1, y1,      u1, v_bottom,
    };

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          (void*)(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

} // namespace vrx::render
