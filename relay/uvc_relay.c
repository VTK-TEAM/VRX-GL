/* uvc_relay.c — автономний релей: USB-камера (V4L2/MJPEG) -> UDP.
 *
 * ОКРЕМА програма, навмисно. Володіє /dev/videoN сама (V4L2 single-consumer),
 * а станція читає готовий UDP-потік як звичайну мережеву камеру — і показ, і
 * запис. Тобто третій канал на станції це просто "другий потік, інший порт";
 * увесь клопіт із залізом (перепідключення, зависання) живе тут, окремо, і
 * впасти може, не зачепивши станцію.
 *
 * Адаптовано з польового uvc_capture_relay.c (waybeam_venc/OpenIPC): чистий
 * V4L2 + POSIX-сокети, нуль SDK. ПРИБРАНО одну річ — аварійний unbind/rebind
 * USB-хоста: на цій платі шина спільна й на ній ще й ЗАПИС, тож ребайнд
 * контролера вбив би й інші пристрої. Лишається все решта: конект/реконект,
 * детектор зависання, бекоф.
 *
 * Збірка (окремо від CMake теж працює):
 *   gcc -O2 -Wall -Wextra -pthread -o uvc_relay relay/uvc_relay.c
 *
 * Запуск:
 *   uvc_relay /dev/video0 640 480 30 udp://239.255.77.1:5002
 *
 * Призначення udp://HOST:PORT:
 *   - мультикаст 224.0.0.0..239.255.255.255 -> шлемо в loopback (iface=lo,
 *     ttl=0), тобто НЕ виходить за плату, і копію отримують ВСІ приймачі
 *     (показ + рекордер + його проба); саме тому мультикаст, а не юнікаст,
 *     який ядро розкидало б між сокетами;
 *   - бродкаст x.x.x.255 -> SO_BROADCAST (як роблять реальні камери; вийде
 *     в мережу);
 *   - юнікаст -> просто одному приймачу.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_UDP_PAYLOAD 65507u   /* 65535 - 8 (UDP) - 20 (IP) */
#define BUF_COUNT 4u
#define RECONNECT_MS 1000
#define RECONNECT_SLICE_MS 100
#define POLL_MS 500
#define STALL_TIMEOUTS 2         /* 2 x POLL_MS = 1000 мс без кадру -> зрив */
#define STAT_EVERY_S 5
#define LOG_EVERY_N 10           /* рідше спамити "немає пристрою" в лог */

typedef struct { void *start; size_t length; } MmapBuf;

typedef struct {
    /* налаштування */
    char device[64];
    char dst_host[128];
    uint16_t dst_port;
    uint32_t req_w, req_h, req_fps;

    /* стан пристрою */
    int fd;
    uint32_t buf_count;
    MmapBuf bufs[BUF_COUNT];
    uint32_t w, h, fps;

    /* вихідний сокет */
    int sock;
    struct sockaddr_in dst;

    /* життя */
    volatile sig_atomic_t running;

    /* лічильники */
    uint64_t frames_sent, bytes_sent, send_errors;
    uint64_t oversize_drops, reconnects;
} Relay;

static Relay g_relay;

static void on_signal(int sig) { (void)sig; g_relay.running = 0; }

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Переривана пауза: спимо ms по RECONNECT_SLICE_MS, щоб на сигнал вийти
 * швидко, а не пересиджувати повну секунду бекофу. */
static void nap(int ms) {
    int slept = 0;
    while (g_relay.running && slept < ms) {
        struct timespec t = {0, (long)RECONNECT_SLICE_MS * 1000000L};
        nanosleep(&t, NULL);
        slept += RECONNECT_SLICE_MS;
    }
}

/* ── сокет ─────────────────────────────────────────────────────────────── */

static int is_multicast(const char *ip) {
    struct in_addr a;
    if (inet_aton(ip, &a) == 0) return 0;
    uint32_t h = ntohl(a.s_addr);
    return (h >= 0xE0000000u && h <= 0xEFFFFFFFu);   /* 224..239 */
}

static int is_broadcast(const char *ip) {
    return strcmp(ip, "255.255.255.255") == 0 ||
           (strlen(ip) > 4 && strcmp(ip + strlen(ip) - 4, ".255") == 0);
}

static int open_socket(Relay *r) {
    r->sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (r->sock < 0) { perror("socket"); return -1; }

    if (is_multicast(r->dst_host)) {
        /* Тримаємо на loopback: вихід через lo + ttl=0 => НЕ покидає плату,
         * але локальні приймачі, що приєднались до групи, копію отримують. */
        struct in_addr iface;
        iface.s_addr = inet_addr("127.0.0.1");
        setsockopt(r->sock, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
        unsigned char ttl = 0;
        setsockopt(r->sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        unsigned char loop = 1;
        setsockopt(r->sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    } else if (is_broadcast(r->dst_host)) {
        int one = 1;
        setsockopt(r->sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }

    memset(&r->dst, 0, sizeof(r->dst));
    r->dst.sin_family = AF_INET;
    r->dst.sin_port = htons(r->dst_port);
    if (inet_aton(r->dst_host, &r->dst.sin_addr) == 0) {
        fprintf(stderr, "[relay] погана IP-адреса: %s\n", r->dst_host);
        close(r->sock); r->sock = -1;
        return -1;
    }
    return 0;
}

/* ── V4L2 ──────────────────────────────────────────────────────────────── */

static int xioctl(int fd, unsigned long req, void *arg) {
    int rc;
    do { rc = ioctl(fd, req, arg); } while (rc < 0 && errno == EINTR);
    return rc;
}

static void v4l2_disconnect(Relay *r) {
    if (r->fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(r->fd, VIDIOC_STREAMOFF, &t);
    }
    for (uint32_t i = 0; i < r->buf_count; i++) {
        if (r->bufs[i].start && r->bufs[i].start != MAP_FAILED)
            munmap(r->bufs[i].start, r->bufs[i].length);
        r->bufs[i].start = NULL;
        r->bufs[i].length = 0;
    }
    r->buf_count = 0;
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
}

static int v4l2_connect(Relay *r) {
    r->fd = open(r->device, O_RDWR | O_CLOEXEC);
    if (r->fd < 0) return -1;

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = r->req_w;
    fmt.fmt.pix.height = r->req_h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (xioctl(r->fd, VIDIOC_S_FMT, &fmt) < 0) { v4l2_disconnect(r); return -1; }
    r->w = fmt.fmt.pix.width;
    r->h = fmt.fmt.pix.height;
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "[relay] пристрій не дав MJPEG\n");
        v4l2_disconnect(r); return -1;
    }

    /* Частота — не фатально, якщо драйвер відмовить. */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = r->req_fps ? r->req_fps : 30;
    xioctl(r->fd, VIDIOC_S_PARM, &parm);
    r->fps = parm.parm.capture.timeperframe.denominator;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(r->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        v4l2_disconnect(r); return -1;
    }

    for (uint32_t i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(r->fd, VIDIOC_QUERYBUF, &b) < 0) { v4l2_disconnect(r); return -1; }
        r->bufs[i].length = b.length;
        r->bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, r->fd, b.m.offset);
        if (r->bufs[i].start == MAP_FAILED) { v4l2_disconnect(r); return -1; }
        r->buf_count = i + 1;
        if (xioctl(r->fd, VIDIOC_QBUF, &b) < 0) { v4l2_disconnect(r); return -1; }
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(r->fd, VIDIOC_STREAMON, &t) < 0) { v4l2_disconnect(r); return -1; }

    fprintf(stderr, "[relay] підключено %s: %ux%u@%u MJPEG -> %s:%u\n",
            r->device, r->w, r->h, r->fps, r->dst_host, r->dst_port);
    return 0;
}

/* Один цикл захвату, поки не зрив. Повертає при зриві (пристрій треба
 * перепідключати). */
static void capture_loop(Relay *r) {
    int stall = 0;
    int64_t last_stat = now_ms();

    while (r->running) {
        struct pollfd pfd = { r->fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, POLL_MS);

        if (rc < 0) { if (errno == EINTR) continue; break; }

        if (rc == 0) {
            /* Таймаут: пристрій живий, а кадрів немає. Два поспіль (1 с) —
             * зависання прошивки, зовні непомітне; рвемо й перепідключаємось. */
            if (++stall >= STALL_TIMEOUTS) {
                fprintf(stderr, "[relay] зависання: %d мс без кадру, реконект\n",
                        STALL_TIMEOUTS * POLL_MS);
                return;
            }
            continue;
        }

        /* Пристрій сам сказав "я помер". */
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[relay] POLLERR/HUP — пристрій відвалився\n");
            return;
        }
        if (!(pfd.revents & POLLIN)) continue;
        stall = 0;

        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(r->fd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            fprintf(stderr, "[relay] DQBUF помилка: %s\n", strerror(errno));
            return;
        }

        if (b.bytesused > 0 && b.bytesused <= MAX_UDP_PAYLOAD && b.index < r->buf_count) {
            ssize_t n = sendto(r->sock, r->bufs[b.index].start, b.bytesused, 0,
                               (struct sockaddr *)&r->dst, sizeof(r->dst));
            if (n < 0) r->send_errors++;
            else { r->frames_sent++; r->bytes_sent += (uint64_t)n; }
        } else if (b.bytesused > MAX_UDP_PAYLOAD) {
            r->oversize_drops++;   /* кадр більший за датаграму — MJPEG такого не дає */
        }

        xioctl(r->fd, VIDIOC_QBUF, &b);   /* буфер назад драйверу */

        int64_t t = now_ms();
        if (t - last_stat >= STAT_EVERY_S * 1000) {
            last_stat = t;
            fprintf(stderr, "[relay] кадрів %llu | %.1f МБ | помилок send %llu"
                            " | завеликих %llu | реконектів %llu\n",
                    (unsigned long long)r->frames_sent, r->bytes_sent / 1e6,
                    (unsigned long long)r->send_errors,
                    (unsigned long long)r->oversize_drops,
                    (unsigned long long)r->reconnects);
        }
    }
}

/* ── розбір аргументів ─────────────────────────────────────────────────── */

static int parse_dst(Relay *r, const char *s) {
    /* очікуємо "udp://HOST:PORT" або "HOST:PORT" */
    const char *p = s;
    if (strncmp(p, "udp://", 6) == 0) p += 6;
    const char *colon = strrchr(p, ':');
    if (!colon) return -1;
    size_t hlen = (size_t)(colon - p);
    if (hlen == 0 || hlen >= sizeof(r->dst_host)) return -1;
    memcpy(r->dst_host, p, hlen);
    r->dst_host[hlen] = '\0';
    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return -1;
    r->dst_port = (uint16_t)port;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
            "usage: %s /dev/videoN WIDTH HEIGHT FPS udp://HOST:PORT\n"
            "  напр.: %s /dev/video0 640 480 30 udp://239.255.77.1:5002\n",
            argv[0], argv[0]);
        return 2;
    }

    Relay *r = &g_relay;
    memset(r, 0, sizeof(*r));
    r->fd = -1;
    r->sock = -1;
    r->running = 1;

    snprintf(r->device, sizeof(r->device), "%s", argv[1]);
    r->req_w = (uint32_t)atoi(argv[2]);
    r->req_h = (uint32_t)atoi(argv[3]);
    r->req_fps = (uint32_t)atoi(argv[4]);
    if (parse_dst(r, argv[5]) != 0) {
        fprintf(stderr, "[relay] погане призначення: %s\n", argv[5]);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (open_socket(r) != 0) return 1;

    fprintf(stderr, "[relay] старт: %s %ux%u@%u -> %s:%u (%s)\n",
            r->device, r->req_w, r->req_h, r->req_fps, r->dst_host, r->dst_port,
            is_multicast(r->dst_host) ? "loopback-мультикаст, не виходить за плату"
            : is_broadcast(r->dst_host) ? "бродкаст" : "юнікаст");

    /* СТАН-МАШИНА: підключено -> захват; не підключено -> ретраї з бекофом.
     * Здаватися не можна ніколи: краще вічно намагатися, ніж лишити відео
     * мертвим. USB-хост-ребайнд свідомо НЕ робимо — шина спільна з записом. */
    int retries = 0;
    while (r->running) {
        if (v4l2_connect(r) != 0) {
            v4l2_disconnect(r);
            if (retries == 0 || retries % LOG_EVERY_N == 0)
                fprintf(stderr, "[relay] немає %s, ретрай (%d)\n", r->device, retries);
            retries++;
            nap(RECONNECT_MS);
            continue;
        }
        retries = 0;
        capture_loop(r);           /* повертається лише при зриві */
        v4l2_disconnect(r);
        r->reconnects++;
        nap(RECONNECT_MS);
    }

    v4l2_disconnect(r);
    if (r->sock >= 0) close(r->sock);
    fprintf(stderr, "[relay] стоп. кадрів усього %llu\n",
            (unsigned long long)r->frames_sent);
    return 0;
}
