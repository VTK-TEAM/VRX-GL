// vrx_rtsp — RTSP-видача потоків станції на виділений інтерфейс для
// зовнішнього центрального комп'ютера.
//
// НАВІЩО. Центральний комп'ютер уміє працювати з енкодером Unisheen
// BM1000H "як є": той за замовчуванням нічого не шле сам, а тримає RTSP-
// сервер на 192.168.1.168:554, з якого потік забирають. Ми повторюємо саме
// це, а не вигадуємо свій протокол, — тоді на тому боці нічого
// переналаштовувати не треба.
//
//     /0  основний канал: H.265 + звук Opus   (UDP 5600 + 5601)
//     /1  додатковий канал (sub): MJPEG      (UDP 5001)
//     /2  локальний канал, плата захвату     (loopback-група релею, 5002)
//
// БЕЗ ПЕРЕКОДУВАННЯ. Пакети лише депакуються й пакуються знову: RTSP-
// серверу потрібні власні пакувальники (pay0, pay1), а свій SSRC і
// нумерація на кожну сесію — це саме те, що вони дають. Декодування тут
// немає, тож ціна — копіювання байтів.
//
// ТРИ АДРЕСИ, А НЕ ОДНА З ПЕРЕМИКАННЯМ. Кодек в RTSP оголошується один раз,
// у SDP при підключенні, і всередині живої сесії його не змінити. Основний
// канал — H.265, додатковий і локальний — MJPEG. Тож кожне джерело має свою
// адресу, а вибирає центральний комп'ютер.
//
// "СПИТЬ", ПОКИ НЕ ПОПРОСЯТЬ. Конвеєр будується на першому DESCRIBE і
// розбирається, коли піде останній клієнт. Потік, який ніхто не забирає,
// не тримає ні сокета, ні процесора.
//
// ЧОМУ ОКРЕМИЙ ПРОЦЕС. libgstrtspserver ставиться з apt, а станції в полі
// оновлюються лише пакетом із флешки, який apt-пакетів не ставить. Якби цим
// займалась сама станція, то на старому образі без бібліотеки вона б не
// запустилась узагалі. Окремо — падає лише цей процес, а показ і запис
// живуть. Запускає й піднімає його наглядач.
//
// ПЛАШКА "НЕМАЄ СИГНАЛУ". Джерело, якого немає, НЕ зникає з RTSP: замість
// нього йде заставка з назвою джерела й годинником, що йде. Годинник тут —
// суть, а не прикраса: він каже центральному комп'ютеру, що ЗВ'ЯЗОК ЗІ
// СТАНЦІЄЮ Є, просто немає камери. Застигла картинка означала б, що зник
// зв'язок. Так само поводиться й BM1000H: без HDMI вона кодує заставку.
//
// Без плашки було б гірше, ніж здається: RTSP-сервер пише SDP з caps
// пакувальника, а caps з'являються лише з першим кадром. Немає камери —
// DESCRIBE висить до таймауту клієнта. Заставка дає caps завжди.
//
// Перемикач — input-selector між живою гілкою й заставкою. H.265
// перемикається лише на КЛЮЧОВОМУ кадрі в обидва боки: дельта-кадр без
// опори декодер показав би сміттям. MJPEG і звук — будь-коли, там кожен
// кадр самодостатній.
//
// ЧОМУ МОЖНА СЛУХАТИ ТІ САМІ ПОРТИ, ЩО Й СТАНЦІЯ. Камера шле бродкастом, і
// ядро віддає копію кожному сокету на порту. При юнікасті це зламалося б:
// пакет дістався б рівно одному слухачеві.
//
// ЧОМУ SO_BINDTODEVICE, А НЕ МАРШРУТИЗАЦІЯ. Адреса 192.168.1.168 — з тієї
// самої підмережі, що й камери на end1. Звичайна таблиця маршрутів обирає
// інтерфейс лише за адресою ПРИЗНАЧЕННЯ, тож відповідь центральному
// комп'ютеру пішла б через end1, у мережу камер, а не в його кабель.
// Маршрутизація за джерелом (ip rule) розв'язала б це, але в ядрі станції
// CONFIG_IP_MULTIPLE_TABLES вимкнено: `ip rule` відповідає "Operation not
// supported". Тому кожен наш сокет прив'язано до пристрою: прив'язаний
// сокет шле лише через нього, хоч що каже таблиця.
//
// Використання:  vrx_rtsp [адреса [порт]]     типово 192.168.1.168 554

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video.h>
#include <glib-unix.h>

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static GMainLoop* g_loop = NULL;
static int g_exit_code = 0;

// Пристрій, на якому живе наша адреса, і його індекс на момент старту.
static char g_ifname[IFNAMSIZ] = "";
static unsigned g_ifindex = 0;
static const char* g_addr = NULL;

// Шукає інтерфейс, що тримає адресу addr. Ім'я USB-адаптера складене з його
// MAC (enx...) і міняється разом з адаптером, тож зашивати його не можна —
// беремо той, на якому адреса є зараз.
static int find_iface(const char* addr, char* out, size_t n) {
    struct in_addr want;
    if (inet_pton(AF_INET, addr, &want) != 1) return 0;
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0) return 0;
    int ok = 0;
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (((struct sockaddr_in*)p->ifa_addr)->sin_addr.s_addr == want.s_addr) {
            g_strlcpy(out, p->ifa_name, n);
            ok = 1;
            break;
        }
    }
    freeifaddrs(ifa);
    return ok;
}

static void bind_to_iface(GSocket* s, const char* what) {
    if (!s || !g_ifname[0]) return;
    if (setsockopt(g_socket_get_fd(s), SOL_SOCKET, SO_BINDTODEVICE,
                   g_ifname, (socklen_t)strlen(g_ifname) + 1) != 0)
        fprintf(stderr, "[rtsp] прив'язка %s до %s: %s\n", what, g_ifname, strerror(errno));
}

// RTP і RTCP по UDP сервер відкриває на SETUP, а дані йдуть після PLAY.
// Перед PLAY і прив'язуємо: інакше перші пакети встигли б піти через end1.
// По TCP (interleaved) окремих сокетів немає — дані йдуть тим самим
// з'єднанням, що вже прив'язане через сокет, який слухає.
static GstRTSPStatusCode on_pre_play(GstRTSPClient* client, GstRTSPContext* ctx, gpointer data) {
    (void)client; (void)data;
    GstRTSPMedia* media = ctx->sessmedia ? gst_rtsp_session_media_get_media(ctx->sessmedia)
                                         : ctx->media;
    if (!media) return GST_RTSP_STS_OK;
    const guint n = gst_rtsp_media_n_streams(media);
    for (guint i = 0; i < n; ++i) {
        GstRTSPStream* st = gst_rtsp_media_get_stream(media, i);
        GSocket* rtp = gst_rtsp_stream_get_rtp_socket(st, G_SOCKET_FAMILY_IPV4);
        GSocket* rtcp = gst_rtsp_stream_get_rtcp_socket(st, G_SOCKET_FAMILY_IPV4);
        bind_to_iface(rtp, "RTP");
        bind_to_iface(rtcp, "RTCP");
        if (rtp) g_object_unref(rtp);
        if (rtcp) g_object_unref(rtcp);
    }
    return GST_RTSP_STS_OK;
}

// ---------------------------------------------------------------------------
// Перемикач "живе / плашка"
// ---------------------------------------------------------------------------

// Скільки тиші вважати зниклим сигналом. Камера дає кадр кожні 33 мс, тож
// 1.5 с — це вже не мережева гикавка, а відсутність джерела.
#define SILENCE_US (1500 * G_TIME_SPAN_MILLISECOND)

enum { LIVE = 0, STUB = 1 };

typedef struct {
    const char* name;      // для журналу
    GstElement* sel;       // input-selector
    GstPad* pad[2];        // його входи: [LIVE], [STUB]
    gboolean keyed;        // перемикати лише на ключовому кадрі (H.265)
    gint want;             // куди треба перейти (атомарно)
    gint active;           // де стоїть зараз (атомарно)
    gint64 last_live;      // монотонний час останнього живого кадру (__atomic)
} Switch;

typedef struct { Switch* sw; gint idx; } ProbeCtx;

// Кадр перед перемикачем. Саме тут, а не в таймері, і робиться перемикання:
// активним пад стає ПЕРЕД тим, як цей кадр у нього зайде, тож першим
// пройде саме ключовий кадр, на якому перемкнулись.
static GstPadProbeReturn on_branch_buffer(GstPad* pad, GstPadProbeInfo* info, gpointer data) {
    (void)pad;
    ProbeCtx* c = data;
    Switch* sw = c->sw;
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);

    if (c->idx == LIVE) {
        // 64-бітний час пишеться з потоку гілки, а читається з головного
        // циклу. У GLib немає атомарних gint64, тож — вбудовані GCC.
        __atomic_store_n(&sw->last_live, g_get_monotonic_time(), __ATOMIC_RELAXED);
        g_atomic_int_set(&sw->want, LIVE);
    }

    if (g_atomic_int_get(&sw->want) != c->idx || g_atomic_int_get(&sw->active) == c->idx)
        return GST_PAD_PROBE_OK;
    if (sw->keyed && GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT))
        return GST_PAD_PROBE_OK;                   // чекаємо ключового

    g_object_set(sw->sel, "active-pad", sw->pad[c->idx], NULL);
    g_atomic_int_set(&sw->active, c->idx);
    fprintf(stderr, "[rtsp] %s: %s\n", sw->name,
            c->idx == LIVE ? "сигнал є, віддаю живе" : "сигналу немає, віддаю плашку");
    return GST_PAD_PROBE_OK;
}

// Раз на 250 мс: чи не замовкло живе джерело.
static gboolean check_silence(gpointer data) {
    GstRTSPMedia* media = data;
    for (int k = 0; k < 2; ++k) {
        Switch* sw = g_object_get_data(G_OBJECT(media), k ? "vrx-sw1" : "vrx-sw0");
        if (!sw || g_atomic_int_get(&sw->want) != LIVE) continue;
        const gint64 last = __atomic_load_n(&sw->last_live, __ATOMIC_RELAXED);
        if (g_get_monotonic_time() - last < SILENCE_US) continue;
        g_atomic_int_set(&sw->want, STUB);
        // Заставка теж H.265, і її кодер іде своїм GOP. Просимо IDR одразу,
        // щоб не чекати до наступного ключового.
        if (sw->keyed)
            gst_pad_push_event(sw->pad[STUB],
                gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE, TRUE, 0));
    }
    return G_SOURCE_CONTINUE;
}

static void free_switch(gpointer p) {
    Switch* sw = p;
    for (int i = 0; i < 2; ++i) if (sw->pad[i]) gst_object_unref(sw->pad[i]);
    if (sw->sel) gst_object_unref(sw->sel);
    g_free(sw);
}

// Знаходить у конвеєрі перемикач і дві гілки, ставить зонди. Імена задані
// в рядках запуску: <sel>, <sel>_live, <sel>_stub.
static void setup_switch(GstRTSPMedia* media, GstElement* bin, const char* sel_name,
                         const char* key, const char* name, gboolean keyed) {
    GstElement* sel = gst_bin_get_by_name(GST_BIN(bin), sel_name);
    if (!sel) return;
    Switch* sw = g_new0(Switch, 1);
    sw->name = name;
    sw->sel = sel;
    sw->keyed = keyed;
    sw->pad[LIVE] = gst_element_get_static_pad(sel, "sink_0");
    sw->pad[STUB] = gst_element_get_static_pad(sel, "sink_1");
    // Стартуємо з плашки: живе джерело ще треба дочекатись.
    g_object_set(sel, "active-pad", sw->pad[STUB], NULL);
    sw->active = STUB;
    sw->want = STUB;

    const char* br[2] = { "_live", "_stub" };
    for (int i = 0; i < 2; ++i) {
        gchar* en = g_strconcat(sel_name, br[i], NULL);
        GstElement* e = gst_bin_get_by_name(GST_BIN(bin), en);
        g_free(en);
        if (!e) continue;
        GstPad* src = gst_element_get_static_pad(e, "src");
        ProbeCtx* c = g_new0(ProbeCtx, 1);
        c->sw = sw;
        c->idx = i;
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, on_branch_buffer, c, g_free);
        gst_object_unref(src);
        gst_object_unref(e);
    }
    g_object_set_data_full(G_OBJECT(media), key, sw, free_switch);
}

// ---- ДІАГНОСТИКА ЗАТРИМКИ (VRX_RTSP_TRACE=1). Де саме сидить кадр: між
// входом у перемикач і виходом із пакувальника, чи вже після нього.
#define TR_N 256
typedef struct { GstClockTime pts[TR_N]; gint64 t[TR_N]; guint i; double sum; guint n; } Trace;
static Trace g_tr;
static GstPadProbeReturn tr_in(GstPad* p, GstPadProbeInfo* info, gpointer d) {
    (void)p; (void)d; GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
    guint k = g_tr.i++ % TR_N; g_tr.pts[k] = GST_BUFFER_PTS(b); g_tr.t[k] = g_get_monotonic_time();
    return GST_PAD_PROBE_OK;
}
static GstPadProbeReturn tr_out(GstPad* p, GstPadProbeInfo* info, gpointer d) {
    (void)p; (void)d; GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
    for (guint k = 0; k < TR_N; ++k) if (g_tr.t[k] && g_tr.pts[k] == GST_BUFFER_PTS(b)) {
        g_tr.sum += (g_get_monotonic_time() - g_tr.t[k]) / 1000.0; g_tr.n++; g_tr.t[k] = 0; break; }
    return GST_PAD_PROBE_OK;
}
static gboolean tr_report(gpointer data) {
    GstRTSPMedia* media = data; GstElement* e = gst_rtsp_media_get_element(media);
    GstQuery* q = gst_query_new_latency(); gboolean live = FALSE; GstClockTime mn = 0, mx = 0;
    GstElement* top = GST_ELEMENT(gst_object_get_parent(GST_OBJECT(e)));
    if (gst_element_query(top ? top : e, q)) gst_query_parse_latency(q, &live, &mn, &mx);
    fprintf(stderr, "[rtsp] ТРАСА: перемикач->пакувальник %.1f мс (кадрів %u) | затримка конвеєра min=%.1f мс\n",
            g_tr.n ? g_tr.sum / g_tr.n : -1.0, g_tr.n, mn / 1e6);
    g_tr.sum = 0; g_tr.n = 0;
    gst_query_unref(q); if (top) gst_object_unref(top); gst_object_unref(e);
    return G_SOURCE_CONTINUE;
}

static void on_media_unprepared(GstRTSPMedia* media, gpointer data) {
    (void)data;
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(media), "vrx-timer"));
    if (id) g_source_remove(id);
    g_object_set_data(G_OBJECT(media), "vrx-timer", NULL);
}

static void on_media_configure(GstRTSPMediaFactory* f, GstRTSPMedia* media, gpointer data) {
    (void)f;
    const char* mount = data;
    GstElement* bin = gst_rtsp_media_get_element(media);
    gchar* vname = g_strdup_printf("%s відео", mount);
    gchar* aname = g_strdup_printf("%s звук", mount);
    g_object_set_data_full(G_OBJECT(media), "vrx-n0", vname, g_free);
    g_object_set_data_full(G_OBJECT(media), "vrx-n1", aname, g_free);
    setup_switch(media, bin, "vsel", "vrx-sw0", vname, g_str_equal(mount, "/0"));
    setup_switch(media, bin, "asel", "vrx-sw1", aname, FALSE);
    gst_object_unref(bin);

    const guint id = g_timeout_add_full(G_PRIORITY_DEFAULT, 250, check_silence,
                                        g_object_ref(media), g_object_unref);
    g_object_set_data(G_OBJECT(media), "vrx-timer", GUINT_TO_POINTER(id));
    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), NULL);

    if (g_getenv("VRX_RTSP_TRACE") && g_str_equal(mount, "/0")) {
        GstElement* b2 = gst_rtsp_media_get_element(media);
        GstElement* lv = gst_bin_get_by_name(GST_BIN(b2), "vsel_live");
        GstElement* py = gst_bin_get_by_name(GST_BIN(b2), "pay0");
        GstPad* a = gst_element_get_static_pad(lv, "src"); GstPad* z = gst_element_get_static_pad(py, "src");
        gst_pad_add_probe(a, GST_PAD_PROBE_TYPE_BUFFER, tr_in, NULL, NULL);
        gst_pad_add_probe(z, GST_PAD_PROBE_TYPE_BUFFER, tr_out, NULL, NULL);
        gst_object_unref(a); gst_object_unref(z); gst_object_unref(lv); gst_object_unref(py); gst_object_unref(b2);
        g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 2, tr_report, g_object_ref(media), g_object_unref);
    }
}

// Гілка плашки: темне поле, назва виробника вгорі, "НЕМАЄ СИГНАЛУ", назва
// каналу, годинник.
// Розміри шрифтів рахуються від висоти кадру (зразок підбирався на 1080p).
// auto-resize=false обов'язково: інакше textoverlay сам масштабує шрифт
// відносно кадру 640 пікселів, і на 1080p літери виходять утричі більші.
#define OVL "auto-resize=false shaded-background=false draw-outline=false " \
            "draw-shadow=false wrap-mode=none halignment=center"
static gchar* plate(const char* label, const char* fmt, int w, int h) {
    return g_strdup_printf(
        "videotestsrc is-live=true pattern=solid-color foreground-color=0xff1c2026 "
        "! video/x-raw,format=%s,width=%d,height=%d,framerate=15/1 "
        "! textoverlay text=\"TOKKO SYSTEMS\" font-desc=\"DejaVu Sans Bold %d\" "
            "valignment=top ypad=%d color=0xff7f8c99 " OVL " "
        "! textoverlay text=\"НЕМАЄ СИГНАЛУ\" font-desc=\"DejaVu Sans Bold %d\" "
            "valignment=center deltay=%d color=0xffffffff " OVL " "
        "! textoverlay text=\"%s\" font-desc=\"DejaVu Sans %d\" "
            "valignment=center deltay=%d color=0xff9aa6b2 " OVL " "
        "! clockoverlay time-format=\"%%d.%%m.%%Y   %%H:%%M:%%S\" "
            "font-desc=\"DejaVu Sans Mono %d\" valignment=bottom ypad=%d "
            "color=0xff6f7b87 " OVL " ",
        fmt, w, h, h * 30 / 1080, h * 70 / 1080, h * 72 / 1080, -h * 40 / 1080, label, h * 32 / 1080, h * 60 / 1080,
        h * 24 / 1080, h * 60 / 1080);
}

// СТОРОЖ ІНТЕРФЕЙСУ. Прив'язка SO_BINDTODEVICE запам'ятовує ІНДЕКС
// пристрою. Висмикнули й встромили адаптер — він повертається з новим
// індексом, і старі сокети вже нікуди не ведуть, хоча процес живий і
// нічого не помічає. Тому раз на 2 с звіряємо: адреса на тому самому
// пристрої? Ні — виходимо, наглядач підніме нас знову, і нова спроба
// прив'яжеться вже до нового.
static gboolean check_iface(gpointer data) {
    (void)data;
    char now[IFNAMSIZ] = "";
    if (!find_iface(g_addr, now, sizeof(now)) || strcmp(now, g_ifname) != 0 ||
        if_nametoindex(now) != g_ifindex) {
        fprintf(stderr, "[rtsp] інтерфейс %s зник або змінився, перезапуск\n", g_ifname);
        g_exit_code = 2;
        g_main_loop_quit(g_loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_stop_signal(gpointer data) {
    (void)data;
    fprintf(stderr, "[rtsp] сигнал зупинки\n");
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static void on_client_closed(GstRTSPClient* client, gpointer ip) {
    (void)client;
    fprintf(stderr, "[rtsp] клієнт %s відключився\n", (const char*)ip);
    g_free(ip);
}

// Хто забирає потік. Для діагностики "центральний комп'ютер нічого не
// показує" це перше питання, і відповідати на нього треба з логу, а не
// tcpdump'ом.
static void on_client_connected(GstRTSPServer* srv, GstRTSPClient* client, gpointer data) {
    (void)srv; (void)data;
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(client);
    const gchar* ip = conn ? gst_rtsp_connection_get_ip(conn) : "?";
    fprintf(stderr, "[rtsp] підключився %s\n", ip);
    g_signal_connect(client, "closed", G_CALLBACK(on_client_closed), g_strdup(ip));
    g_signal_connect(client, "pre-play-request", G_CALLBACK(on_pre_play), NULL);
}

static void add_mount(GstRTSPMountPoints* mounts, const char* path, const char* launch) {
    GstRTSPMediaFactory* f = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(f, launch);
    g_signal_connect(f, "media-configure", G_CALLBACK(on_media_configure), (gpointer)path);
    // Один конвеєр на всіх клієнтів: другий глядач не відкриває ще один
    // сокет на тому самому порту.
    gst_rtsp_media_factory_set_shared(f, TRUE);
    gst_rtsp_mount_points_add_factory(mounts, path, f);   // забирає f
}

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    const char* addr = argc > 1 ? argv[1] : "192.168.1.168";
    g_addr = addr;
    const char* port = argc > 2 ? argv[2] : "554";

    GstRTSPServer* srv = gst_rtsp_server_new();
    gst_rtsp_server_set_address(srv, addr);
    gst_rtsp_server_set_service(srv, port);
    g_signal_connect(srv, "client-connected", G_CALLBACK(on_client_connected), NULL);

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(srv);

    // Типи навантаження — як у BM1000H: відео 96, звук 97. Вхідні типи
    // (97 і 98) — як шле камера, див. ВИМОГИ-РЕЛІЗУ.md, розділ "Мережа".
    //
    // config-interval=-1 на h265parse обох гілок: параметри кодека перед
    // КОЖНИМ ключовим кадром. Інакше клієнт, що підключився посеред
    // потоку, і клієнт після перемикання чекали б на них, а камера може
    // слати їх лише на старті сесії.
    //
    // sync-streams=false: неактивна гілка просто викидає кадри, а не чекає,
    // поки активна її наздожене.
    gchar* p0 = plate("ОСНОВНИЙ КАНАЛ", "NV12", 1920, 1080);
    gchar* l0 = g_strdup_printf(
        "( input-selector name=vsel sync-streams=false ! rtph265pay name=pay0 pt=96 "
        "  udpsrc port=5600 caps=\"application/x-rtp,media=video,clock-rate=90000,"
              "encoding-name=H265,payload=97\" "
        "    ! rtph265depay ! h265parse name=vsel_live config-interval=-1 ! vsel.sink_0 "
        "  %s ! mpph265enc gop=15 bps=400000 ! h265parse name=vsel_stub config-interval=-1 "
        "    ! vsel.sink_1 "
        "  input-selector name=asel sync-streams=false ! rtpopuspay name=pay1 pt=97 "
        "  udpsrc port=5601 caps=\"application/x-rtp,media=audio,clock-rate=48000,"
              "encoding-name=OPUS,payload=98\" "
        "    ! rtpopusdepay name=asel_live ! asel.sink_0 "
        "  audiotestsrc is-live=true wave=silence ! audio/x-raw,rate=48000,channels=1 "
        "    ! opusenc name=asel_stub ! asel.sink_1 )", p0);
    add_mount(mounts, "/0", l0);

    // Додатковий канал — сирий JPEG, без RTP. jpegparse знаходить межі кадрів
    // по маркерах SOI/EOI — той самий спосіб, що й у MjpegSource станції.
    //
    // caps=image/jpeg НА udpsrc ОБОВ'ЯЗКОВІ, хоч станція обходиться без
    // них: без caps jpegparse не стартує. Заміряно: без них ffprobe висне
    // на DESCRIBE до таймауту.
    gchar* p1 = plate("ДОДАТКОВИЙ КАНАЛ", "I420", 640, 480);
    gchar* l1 = g_strdup_printf(
        "( input-selector name=vsel sync-streams=false ! rtpjpegpay name=pay0 pt=26 "
        "  udpsrc port=5001 caps=image/jpeg ! jpegparse name=vsel_live ! vsel.sink_0 "
        "  %s ! jpegenc name=vsel_stub quality=80 ! vsel.sink_1 )", p1);
    add_mount(mounts, "/1", l1);

    // Захват приходить від релею в loopback-групу (main.cpp: kCapGroup,
    // kCapPort). Релей працює, лише коли наглядач знайшов камеру захвату.
    gchar* p2 = plate("ЛОКАЛЬНИЙ КАНАЛ", "I420", 640, 480);
    gchar* l2 = g_strdup_printf(
        "( input-selector name=vsel sync-streams=false ! rtpjpegpay name=pay0 pt=26 "
        "  udpsrc address=239.255.77.1 port=5002 multicast-iface=lo auto-multicast=true "
        "    caps=image/jpeg ! jpegparse name=vsel_live ! vsel.sink_0 "
        "  %s ! jpegenc name=vsel_stub quality=80 ! vsel.sink_1 )", p2);
    add_mount(mounts, "/2", l2);

    g_free(p0); g_free(p1); g_free(p2);
    g_free(l0); g_free(l1); g_free(l2);

    g_object_unref(mounts);

    // Немає адреси — найчастіше USB-адаптер не встромлено. Виходимо з
    // помилкою: наглядач спробує знову, і щойно адаптер з'явиться, наступна
    // спроба вдасться.
    if (!find_iface(addr, g_ifname, sizeof(g_ifname))) {
        fprintf(stderr, "[rtsp] адреси %s немає на жодному інтерфейсі\n", addr);
        g_object_unref(srv);
        return 1;
    }
    g_ifindex = if_nametoindex(g_ifname);

    // Сокет, що слухає, створюємо самі, а не через gst_rtsp_server_attach:
    // прив'язати до пристрою треба ДО прийому з'єднань. Прийняті з'єднання
    // прив'язку успадковують, тож і відповіді RTSP, і TCP-interleaved
    // підуть тим самим кабелем — починаючи з першого SYN-ACK.
    GError* err = NULL;
    GSocket* ls = gst_rtsp_server_create_socket(srv, NULL, &err);
    if (!ls) {
        fprintf(stderr, "[rtsp] не вдалося стати на %s:%s: %s\n",
                addr, port, err ? err->message : "?");
        g_clear_error(&err);
        g_object_unref(srv);
        return 1;
    }
    bind_to_iface(ls, "сокета, що слухає,");
    GSource* src = g_socket_create_source(ls, G_IO_IN, NULL);
    g_source_set_callback(src, G_SOURCE_FUNC(gst_rtsp_server_io_func),
                          g_object_ref(srv), (GDestroyNotify)g_object_unref);
    g_source_attach(src, NULL);
    g_source_unref(src);

    g_loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(2, check_iface, NULL);
    g_unix_signal_add(SIGTERM, on_stop_signal, NULL);
    g_unix_signal_add(SIGINT, on_stop_signal, NULL);

    fprintf(stderr, "[rtsp] слухаю rtsp://%s:%s на %s: /0 основний, /1 додатковий, /2 локальний\n",
            addr, port, g_ifname);
    g_main_loop_run(g_loop);

    g_main_loop_unref(g_loop);
    g_object_unref(ls);
    g_object_unref(srv);
    return g_exit_code;
}
