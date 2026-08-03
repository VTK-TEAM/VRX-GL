// vrx_supervisor — хто вирішує, що зараз на екрані.
//
// НАВІЩО ОКРЕМА ПРОГРАМА. Станція, редактор розкладки і все, що з'явиться
// далі, не можуть працювати одночасно: DRM master ексклюзивний, і той,
// хто його тримає, тримає екран цілком. Отже потрібен хтось, хто екрана
// не тримає взагалі, а лише запускає по одному й чекає.
//
// Раніше це робив shell усередині run.sh. Поки сценарій був один
// ("підняти станцію"), цього вистачало; щойно з'являється другий, у
// скрипті починається розгалуження, яке ніхто не перечитає через місяць.
// Тут — звичайний автомат станів, і кожен новий режим це один case.
//
// ПРОТОКОЛ — КОД ВИХОДУ, і більше нічого. Ні сокетів, ні файлів-
// прапорців, ні сигналів. Станція, натомість щоб самій щось запускати,
// просто виходить із домовленим числом:
//
//     10           "хочу редактор розкладки" — піднімаємо його, чекаємо,
//                  повертаємо станцію
//     будь-що інше піднімаємо станцію знову
//
// ВИХОДУ "САМ ПО СОБІ" В НАГЛЯДАЧА НЕМАЄ. Станція завершилась із нулем,
// упала з сигналом, віддала невідомий код — реакція одна: підняти її
// назад. Причина проста: на станції немає ні термінала, ні робочого
// стола, і "нічого не показувати" там не є станом, з якого можна вийти.
// Єдиний спосіб зупинити наглядача — зупинити ЙОГО (systemctl stop).
//
// Причина саме така: поки процес станції живий, він тримає DRM master, і
// редактор однаково не отримав би екрана. Тобто передати керування можна
// РІВНО в момент виходу — і код виходу є природним способом сказати,
// кому саме.
//
// ЩО ЦЕ ДАЄ НА ГОЛІЙ СИСТЕМІ. Наглядач — єдине, що треба покласти в
// автозапуск (systemd-юніт). Він переживає і падіння станції, і вихід із
// редактора, і не залежить ні від графічної сесії, ні від термінала.

#define _GNU_SOURCE
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// --- домовлені коди виходу станції ---
#define EXIT_RUN_EDITOR 10

// --- пауза між перезапусками після збою: 1, 2, 4, 8 секунд ---
//
// Та сама логіка, що й у сторожів усередині станції: стала помилка (немає
// монітора, зайнятий порт, не збирається пайплайн) не виправляється
// негайним повторенням, вона виправляється РІДШИМ повторенням. Без паузи
// наглядач крутив би запуск десятки разів на секунду й залив би лог.
#define RESTART_MS_MIN 1000
#define RESTART_MS_MAX 8000

// Скільки має прожити станція, щоб вважатись здоровою. Прожила довше —
// збій разовий, лічильник обнуляємо.
#define HEALTHY_MS 20000

static volatile sig_atomic_t g_stop = 0;
static volatile pid_t g_child = 0;

// РЕЛЕЙ ЗАХВАТУ — окремий, ПАРАЛЕЛЬНИЙ процес. Володіє USB-камерою
// (V4L2 single-consumer) і жене MJPEG у loopback; станція читає його як
// звичайну мережеву камеру. Наглядач тримає його живим так само, як
// станцію, але НЕЗАЛЕЖНО: релей падає — станція працює далі, і навпаки.
static volatile pid_t g_relay = 0;
static char* g_relay_argv[8] = {0};   // NULL[0] = релей не налаштований
static long g_relay_started = 0;
static int g_relay_fails = 0;

static void on_signal(int sig) {
    g_stop = 1;
    // Обидві дитини глушимо тим самим сигналом: станція вміє коректно
    // закритись по SIGTERM (дописати запис, повернути тримінг), релей —
    // закрити пристрій і сокет.
    if (g_child > 0) kill(g_child, sig);
    if (g_relay > 0) kill(g_relay, sig);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// Корінь проєкту від САМОГО СЕБЕ, а не від поточного каталогу.
//
// Наглядача запускає systemd, а той стартує з "/". Шляхи до станції,
// редактора, атласа й конфігу — відносні, тож робочий каталог мусить
// бути коренем проєкту, інакше станція підніметься без телеметрії, і
// мовчки. Той самий прийом, що й у запускача: /proc/self/exe.
static int find_root(char* out, size_t n) {
    char exe[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0) return -1;
    exe[len] = '\0';

    // <корінь>/build/vrx_supervisor -> два рівні вгору.
    char* dir = dirname(exe);           // <корінь>/build
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    char* root = dirname(tmp);          // <корінь>
    snprintf(out, n, "%s", root);
    return 0;
}

// ЗАПИСУВАННЯ НА НОСІЙ — БЕЗПЕРЕРВНО, А НЕ РИВКАМИ.
//
// Типові пороги ядра налаштовані під десктоп: почати фонове записування
// лише коли брудних сторінок назбиралось ~10% RAM, а за віком — аж через
// 30 секунд. Для стрім-запису на флешку це означало: дані копичаться в
// пам'яті сотнями мегабайтів, syncfs у станції потім проштовхує їх одним
// ривком десятки секунд, а зупинка станції (перехід у редактор) чекає,
// поки потік у непереривному D-стані досидить у тому syncfs до кінця.
// Заміряно на живій станції: 3 хв 9 с від кнопки до редактора; після
// цього налаштування — 6 с.
//
// Ставимо: почати писати вже з 8 МБ бруду, вік сторінки до записування
// 3 с, записувач прокидається 10 разів на секунду. Разом із резервуванням
// місця в рекордері (fallocate, див. record/recorder.cpp) це тримає чергу
// на рівні секунд, і гарантія "±5 с при висмикуванні" стає чесною.
//
// Чому тут, а не в /etc/sysctl.d: у системний розділ оновлення з флешки
// не пише, а наглядач їде звичайним VRX-update і стартує першим, під
// root. Значення глобальні для системи, але системний диск тут майже не
// пише (логи на zram), тож зачепити нікого.
static void tune_writeback(void) {
    static const struct { const char* path; const char* val; } knobs[] = {
        { "/proc/sys/vm/dirty_background_bytes",  "8388608" },
        { "/proc/sys/vm/dirty_expire_centisecs",  "300" },
        { "/proc/sys/vm/dirty_writeback_centisecs", "100" },
    };
    for (size_t i = 0; i < sizeof(knobs) / sizeof(knobs[0]); ++i) {
        FILE* f = fopen(knobs[i].path, "w");
        if (!f) {
            fprintf(stderr, "[наглядач] не відкрив %s: %s\n",
                    knobs[i].path, strerror(errno));
            continue;
        }
        if (fputs(knobs[i].val, f) < 0) {
            fprintf(stderr, "[наглядач] не записав %s: %s\n",
                    knobs[i].path, strerror(errno));
        }
        fclose(f);
    }
}

// Піднімає релей захвату у ФОНІ (не чекаємо його — він живе паралельно
// станції). Нічого не робить, якщо релей не налаштований (немає камери).
static void start_relay(void) {
    if (g_stop || !g_relay_argv[0]) return;
    fprintf(stderr, "[наглядач] запускаю релей захвату\n");
    fflush(stderr);
    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[наглядач] fork релею не вдався: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        execv(g_relay_argv[0], g_relay_argv);
        fprintf(stderr, "[наглядач] релей не запустився: %s\n", strerror(errno));
        _exit(127);
    }
    g_relay = pid;
    g_relay_started = now_ms();
}

// Релей помер — піднімаємо назад. Невелика фіксована пауза, щоб зламаний
// бінар не крутився надто швидко; "справжня" смерть тут рідкість, бо
// залізо (unplug, зависання) релей ретраїть сам усередині, не виходячи.
static void restart_relay(void) {
    if (g_stop || !g_relay_argv[0]) return;
    const long lived = now_ms() - g_relay_started;
    fprintf(stderr, "[наглядач] релей вийшов (прожив %ld мс), піднімаю\n", lived);
    for (long s = 0; s < 500 && !g_stop; s += 100) sleep_ms(100);
    start_relay();
}

// Запускає програму й чекає. Повертає код виходу, або -1 якщо процес
// убито сигналом (це не збій станції, це нас зупиняють).
//
// Чекає ПОЖИНАЮЧИ БУДЬ-ЯКУ дитину: поки блокуємось на станції/редакторі,
// може вмерти релей — тоді піднімаємо його й чекаємо далі. Станція й
// релей незалежні, і смерть одного не має чіпати іншого.
static int run_and_wait(const char* what, char* const argv[], char* const envp_extra[]) {
    fprintf(stderr, "[наглядач] запускаю %s\n", what);
    fflush(stderr);

    const pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[наглядач] fork не вдався: %s\n", strerror(errno));
        return -2;
    }
    if (pid == 0) {
        // Дитина. Власна група процесів: інакше Ctrl+C у терміналі
        // прилетів би і їй, і нам одночасно, і хто кого зупинить —
        // питання випадку.
        setpgid(0, 0);
        for (int i = 0; envp_extra && envp_extra[i]; ++i) putenv(envp_extra[i]);
        execv(argv[0], argv);
        fprintf(stderr, "[наглядач] не запустилось %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    g_child = pid;
    int status = 0;
    for (;;) {
        int st = 0;
        const pid_t got = waitpid(-1, &st, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;                          // дітей не лишилось
        }
        if (g_relay > 0 && got == g_relay) {
            g_relay = 0;
            restart_relay();                // релей помер — незалежно від станції
            continue;
        }
        if (got == pid) { status = st; break; }
        // якась інша дитина — ігноруємо
    }
    g_child = 0;

    if (WIFSIGNALED(status)) {
        // Сигнал — це або падіння (SIGSEGV, SIGABRT), або хтось убив
        // процес ззовні. Для наглядача це РІВНО ТАКИЙ САМИЙ збій, як
        // ненульовий код виходу: екран лишився без картинки, і його треба
        // повернути. Відрізняти ці випадки нема сенсу — реакція одна.
        //
        // Спершу я тут виходив із циклу, і наглядач помирав разом зі
        // станцією: тобто рівно в тому випадку, заради якого він і
        // потрібен. Вихід лишився лише один — коли зупиняють НАС самих,
        // і це видно по g_stop.
        fprintf(stderr, "[наглядач] %s зупинено сигналом %d\n", what, WTERMSIG(status));
        return -1;
    }
    const int rc = WEXITSTATUS(status);
    fprintf(stderr, "[наглядач] %s вийшов, код %d\n", what, rc);
    return rc;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    // Дочірній процес може вмерти будь-коли — це нормальний хід подій,
    // а не аварія.
    signal(SIGPIPE, SIG_IGN);

    char root[PATH_MAX];
    if (find_root(root, sizeof(root)) != 0) {
        fprintf(stderr, "[наглядач] не визначив корінь проєкту\n");
        return 1;
    }
    if (chdir(root) != 0) {
        fprintf(stderr, "[наглядач] не перейшов у %s: %s\n", root, strerror(errno));
        return 1;
    }
    fprintf(stderr, "[наглядач] корінь проєкту: %s\n", root);

    tune_writeback();

    char station[PATH_MAX], editor[PATH_MAX];
    snprintf(station, sizeof(station), "%s/build/vrx_gl", root);
    snprintf(editor, sizeof(editor), "%s/build/osd_editor", root);

    // РЕЛЕЙ ЗАХВАТУ. Вмикається, лише коли задано VRX_CAPTURE_DEV — та сама
    // змінна, за якою станція читає третій потік. Шле в LOOPBACK-мультикаст
    // (ttl=0, iface=lo у релеї): НЕ виходить за плату, тож не глушить прийом
    // основного H.265 на end1, але копію отримують усі локальні сокети
    // (показ + рекордер + проба). Адреса/порт збігаються з kCapGroup/kCapPort.
    static char relay_bin[PATH_MAX], relay_dev[64], relay_w[16], relay_h[16], relay_fps[16];
    static char relay_dst[] = "udp://239.255.77.1:5002";
    const char* cap_dev = getenv("VRX_CAPTURE_DEV");
    if (cap_dev && cap_dev[0]) {
        const char* w = getenv("VRX_CAPTURE_W");
        const char* h = getenv("VRX_CAPTURE_H");
        const char* f = getenv("VRX_CAPTURE_FPS");
        snprintf(relay_bin, sizeof(relay_bin), "%s/build/uvc_relay", root);
        snprintf(relay_dev, sizeof(relay_dev), "%s", cap_dev);
        snprintf(relay_w, sizeof(relay_w), "%s", (w && w[0]) ? w : "720");
        snprintf(relay_h, sizeof(relay_h), "%s", (h && h[0]) ? h : "480");
        snprintf(relay_fps, sizeof(relay_fps), "%s", (f && f[0]) ? f : "25");
        g_relay_argv[0] = relay_bin; g_relay_argv[1] = relay_dev;
        g_relay_argv[2] = relay_w;   g_relay_argv[3] = relay_h;
        g_relay_argv[4] = relay_fps; g_relay_argv[5] = relay_dst;
        g_relay_argv[6] = NULL;
        start_relay();
    }

    int fail_streak = 0;

    while (!g_stop) {
        char* st_argv[] = { station, NULL };
        const long started = now_ms();
        const int rc = run_and_wait("станцію", st_argv, NULL);
        const long lived = now_ms() - started;

        if (g_stop) break;                      // зупиняють нас

        if (rc == EXIT_RUN_EDITOR) {
            // РЕДАКТОР ЖИВЕ У ВЛАСНОМУ X-СЕРВЕРІ.
            //
            // Не в робочому столі: на станції його немає й не буде.
            // xinit піднімає голий X рівно на час роботи редактора й
            // прибирає його одразу після виходу — тобто екран повертається
            // станції без жодних слідів.
            //
            // vt7 і :1 задані явно: наглядач може працювати з systemd, де
            // ні термінала, ні "поточної" консолі не існує.
            //
            // -configdir <корінь>/xorg.conf.d — СВІЙ X-КОНФІГ, а не системний.
            //
            // Вендорський /etc/X11/xorg.conf.d вмикає glamor, а той поверх
            // пропрієтарного libmali міняє місцями червоний із синім: у
            // редакторі кнопка "+ додати" ставала червоною, "Вихід" синім.
            // Своп сидить нижче за X-пам'ять, на копії текстури в буфер
            // сканування, тому в самому редакторі виправити його неможливо.
            //
            // Конфіг береться з КОРЕНЯ СТАНЦІЇ, а не з /etc, свідомо: у
            // системний розділ оновлення з флешки не пише (абсолютні шляхи
            // і ".." воно відкидає навмисно), а корінь станції — пише. Тож
            // фікс доїжджає до вже зашитих станцій звичайним VRX-update,
            // без жодного лазіння в образ.
            //
            // Прапорець безумовний і не має стану, який може розійтись, але
            // сам файл перевіряємо: станція зі старим оновленням його ще не
            // має, і X із -configdir на порожнє місце підняв би 640x480
            // замість екрана. Немає файла — йдемо як раніше, з системним
            // конфігом: кольори будуть свопнуті, зате редактор працює.
            //
            // Пристрої вводу від цього не губляться: -configdir заміняє лише
            // /etc/X11/xorg.conf.d, а правила libinput лежать у
            // /usr/share/X11/xorg.conf.d і читаються далі.
            char confdir[PATH_MAX], conffile[PATH_MAX];
            snprintf(confdir, sizeof(confdir), "%s/xorg.conf.d", root);
            snprintf(conffile, sizeof(conffile), "%s/20-modesetting.conf", confdir);

            char* ed_argv[10];
            int n = 0;
            ed_argv[n++] = (char*)"/usr/bin/xinit";
            ed_argv[n++] = editor;
            ed_argv[n++] = (char*)"--";
            ed_argv[n++] = (char*)":1";
            ed_argv[n++] = (char*)"-nolisten";
            ed_argv[n++] = (char*)"tcp";
            ed_argv[n++] = (char*)"vt7";
            if (access(conffile, R_OK) == 0) {
                ed_argv[n++] = (char*)"-configdir";
                ed_argv[n++] = confdir;
            } else {
                fprintf(stderr, "[наглядач] немає %s — X піде з системним"
                                " конфігом (кольори можуть бути свопнуті)\n", conffile);
            }
            ed_argv[n] = NULL;
            char* ed_env[] = { (char*)"VRX_EDITOR_FULLSCREEN=1", NULL };
            run_and_wait("редактор розкладки", ed_argv, ed_env);
            // Чим би він не закінчився — вертаємо станцію. Редактор, що
            // впав, не привід лишити пілота з чорним екраном.
            fail_streak = 0;
            continue;
        }

        // Решта — усе інше: нуль, ненульовий код, смерть від сигналу.
        // Розрізняти нема чого, реакція одна — підняти назад.
        if (lived > HEALTHY_MS) fail_streak = 0;
        if (fail_streak < 4) fail_streak++;

        long back = RESTART_MS_MIN;
        for (int i = 1; i < fail_streak && back < RESTART_MS_MAX; ++i) back *= 2;
        if (back > RESTART_MS_MAX) back = RESTART_MS_MAX;

        fprintf(stderr, "[наглядач] станція прожила %ld мс, %d-й збій поспіль,"
                        " піднімаю через %ld мс\n", lived, fail_streak, back);
        for (long slept = 0; slept < back && !g_stop; slept += 100) sleep_ms(100);
    }

    fprintf(stderr, "[наглядач] зупинився\n");
    return 0;
}
